#!/usr/bin/env python3
"""
List Home Assistant states via WebSocket API using main/config.h credentials.

Useful to confirm actual entity_ids created by MQTT discovery (e.g. va_status/va_response).
"""

from __future__ import annotations

import json
import re
import ssl
from pathlib import Path
import sys

import websocket

if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8")


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

    ws = websocket.create_connection(ws_url, timeout=20, sslopt=sslopt)
    try:
        ws.recv()  # auth_required
        ws.send(json.dumps({"type": "auth", "access_token": token}))
        auth = json.loads(ws.recv())
        if auth.get("type") != "auth_ok":
            print(f"AUTH FAILED: {auth.get('type')}")
            return 1

        ws.send(json.dumps({"id": 1, "type": "get_states"}))
        resp = json.loads(ws.recv())
        if not resp.get("success"):
            print("get_states failed")
            return 1

        states = resp.get("result") or []
        print(f"Total states: {len(states)}")

        matches = []
        for st in states:
            eid = st.get("entity_id", "")
            if "voice_assistant" in eid or "esp32p4" in eid or "esp32_p4" in eid:
                matches.append((eid, st.get("state")))

        matches.sort()
        print(f"Matched states: {len(matches)}")
        for eid, val in matches:
            print(f"- {eid} = {val}")
    finally:
        ws.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
