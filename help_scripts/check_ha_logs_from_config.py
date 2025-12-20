#!/usr/bin/env python3
"""
Fetch Home Assistant system logs via WebSocket API using values from main/config.h.

This avoids hardcoding secrets in scripts and does not require esphome/secrets.yaml.
"""

from __future__ import annotations

import json
import re
import ssl
from pathlib import Path

import websocket


def read_config_h(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def parse_define_string(src: str, name: str) -> str | None:
    m = re.search(
        rf'^\s*#\s*define\s+{re.escape(name)}\s+"([^"]*)"\s*(?://.*)?$',
        src,
        re.MULTILINE,
    )
    if not m:
        return None
    return m.group(1).strip()


def parse_define_int(src: str, name: str) -> int | None:
    m = re.search(
        rf"^\s*#\s*define\s+{re.escape(name)}\s+([0-9]+)\s*(?://.*)?$",
        src,
        re.MULTILINE,
    )
    if not m:
        return None
    return int(m.group(1))


def parse_define_bool(src: str, name: str) -> bool | None:
    m = re.search(
        rf"^\s*#\s*define\s+{re.escape(name)}\s+([01]|true|false)\s*(?://.*)?$",
        src,
        re.MULTILINE | re.IGNORECASE,
    )
    if not m:
        return None
    v = m.group(1).strip().lower()
    if v in ("1", "true"):
        return True
    if v in ("0", "false"):
        return False
    return None


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    config_h = root / "main" / "config.h"
    if not config_h.exists():
        print("ERROR: main/config.h not found")
        return 2

    src = read_config_h(config_h)
    hostname = parse_define_string(src, "HA_HOST")
    if not hostname:
        hostname = parse_define_string(src, "HA_HOSTNAME")
    token = parse_define_string(src, "HA_TOKEN")
    port = parse_define_int(src, "HA_PORT")
    use_ssl = parse_define_bool(src, "HA_USE_SSL")

    if not hostname or not token or port is None or use_ssl is None:
        print("ERROR: Could not parse HA_* values from main/config.h")
        return 2

    scheme = "wss" if use_ssl else "ws"
    ws_url = f"{scheme}://{hostname}:{port}/api/websocket"

    sslopt = None
    if ws_url.startswith("wss://"):
        sslopt = {"cert_reqs": ssl.CERT_NONE, "check_hostname": False}

    print("HOME ASSISTANT LOG CHECK")
    print(f"WS: {ws_url}")

    ws = websocket.create_connection(ws_url, timeout=20, sslopt=sslopt)
    try:
        auth_required = ws.recv()
        _ = auth_required

        ws.send(json.dumps({"type": "auth", "access_token": token}))
        auth_result = json.loads(ws.recv())
        if auth_result.get("type") != "auth_ok":
            print(f"AUTH FAILED: {auth_result.get('type')}")
            return 1

        ws.send(json.dumps({"id": 1, "type": "system_log/list"}))
        resp = json.loads(ws.recv())
        if not resp.get("success"):
            print("system_log/list failed")
            return 1

        logs = resp.get("result") or []
        print(f"Total logs: {len(logs)}")

        interesting = []
        for entry in logs:
            s = json.dumps(entry, ensure_ascii=False).lower()
            if "esp32" in s or "mqtt" in s or "websocket" in s or "assist" in s or "tts" in s:
                interesting.append(entry)

        print(f"Filtered logs: {len(interesting)} (esp32/mqtt/websocket/assist/tts)")
        for entry in interesting[-12:]:
            level = entry.get("level", "INFO")
            name = entry.get("name", "N/A")
            msg_val = entry.get("message")
            if msg_val is None:
                message = ""
            elif isinstance(msg_val, str):
                message = msg_val
            else:
                message = json.dumps(msg_val, ensure_ascii=False)
            message = message.replace("\n", " ")
            if len(message) > 240:
                message = message[:240] + "..."
            print(f"- [{level}] {name}: {message}")
    finally:
        ws.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
