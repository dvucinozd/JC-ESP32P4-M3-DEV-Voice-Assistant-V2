#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Test Home Assistant WebSocket connection with SSL"""
import json
import ssl
import sys
from pathlib import Path
import websocket

# Fix Windows console encoding
if sys.platform == 'win32':
    sys.stdout.reconfigure(encoding='utf-8')

def _to_bool(v, default=True):
    if v is None:
        return default
    if isinstance(v, bool):
        return v
    s = str(v).strip().lower()
    if s in ("1", "true", "yes", "y", "on"):
        return True
    if s in ("0", "false", "no", "n", "off"):
        return False
    return default


def load_ha_secrets():
    # Prefer environment variables (useful for CI / temporary overrides)
    base_url = (os.environ.get("HOME_ASSISTANT_BASE_URL") or "").strip()
    token = (os.environ.get("HOME_ASSISTANT_TOKEN") or "").strip()
    verify_ssl = _to_bool(os.environ.get("HOME_ASSISTANT_VERIFY_SSL"), default=True)

    if base_url and token:
        return base_url, token, verify_ssl

    # Fall back to esphome/secrets.yaml (repo convention in this project)
    try:
        import yaml
    except Exception:
        yaml = None

    root = Path(__file__).resolve().parents[1]
    secrets_path = root / "esphome" / "secrets.yaml"

    if yaml is None or not secrets_path.exists():
        return base_url, token, verify_ssl

    secrets = yaml.safe_load(secrets_path.read_text(encoding="utf-8")) or {}
    base_url = (secrets.get("home_assistant_base_url") or base_url).strip()
    token = (secrets.get("home_assistant_token") or token).strip()
    verify_ssl = _to_bool(secrets.get("home_assistant_verify_ssl"), default=verify_ssl)
    return base_url, token, verify_ssl


import os

# Configuration
HA_BASE_URL, HA_TOKEN, VERIFY_SSL = load_ha_secrets()
if not HA_BASE_URL or not HA_TOKEN:
    print("Missing Home Assistant credentials.")
    print("Set HOME_ASSISTANT_BASE_URL and HOME_ASSISTANT_TOKEN, or fill esphome/secrets.yaml.")
    sys.exit(2)

# Build WebSocket URL
ws_url = ('wss://' if HA_BASE_URL.startswith('https://') else 'ws://') + HA_BASE_URL.split('://', 1)[1] + '/api/websocket'

# SSL options - skip certificate verification
sslopt = None
if ws_url.startswith('wss://'):
    sslopt = {}
    if not VERIFY_SSL:
        sslopt['cert_reqs'] = ssl.CERT_NONE
        sslopt['check_hostname'] = False

print("=" * 80)
print("HOME ASSISTANT WEBSOCKET TEST")
print("=" * 80)
print(f"Connecting to: {ws_url}")
print(f"SSL verification: {VERIFY_SSL}")
print(f"SSL options: {sslopt}")
print()

try:
    # Connect to WebSocket
    print("[1] Connecting to WebSocket...")
    ws = websocket.create_connection(ws_url, timeout=20, sslopt=sslopt)
    print("    ✅ WebSocket connected!")

    # Receive auth_required
    print("\n[2] Waiting for auth_required message...")
    auth_msg = json.loads(ws.recv())
    print(f"    ✅ Received: {json.dumps(auth_msg, indent=2)}")

    # Send auth
    print("\n[3] Sending authentication...")
    ws.send(json.dumps({'type': 'auth', 'access_token': HA_TOKEN}))
    auth_result = json.loads(ws.recv())
    print(f"    ✅ Auth result: {json.dumps(auth_result, indent=2)}")

    if auth_result.get('type') == 'auth_ok':
        print("\n    ✅✅✅ AUTHENTICATION SUCCESSFUL! ✅✅✅")

        # Request system logs
        print("\n[4] Requesting system logs...")
        ws.send(json.dumps({'id': 1, 'type': 'system_log/list'}))
        log_response = json.loads(ws.recv())

        if log_response.get('success'):
            logs = log_response.get('result', [])
            print(f"    ✅ Total logs: {len(logs)}")
            print(f"    First 3 log entries:")
            for log in logs[:3]:
                print(f"      - [{log.get('level')}] {log.get('name')}: {log.get('message')[:80]}")
        else:
            print(f"    ❌ Log request failed: {log_response}")
    else:
        print(f"\n    ❌ AUTHENTICATION FAILED: {auth_result}")

    ws.close()
    print("\n[5] WebSocket closed")

except Exception as e:
    print(f"\n❌ ERROR: {type(e).__name__}: {e}")
    import traceback
    traceback.print_exc()

print("\n" + "=" * 80)
print("TEST COMPLETE")
print("=" * 80)
