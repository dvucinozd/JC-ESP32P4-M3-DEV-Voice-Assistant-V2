#!/usr/bin/env python3
"""Check Home Assistant logs via WebSocket API"""
import json
import ssl
from pathlib import Path
import websocket
import yaml

# Load secrets
ROOT = Path('.').resolve()
secrets = yaml.safe_load((ROOT / 'esphome' / 'secrets.yaml').read_text(encoding='utf-8'))
base_url = secrets['home_assistant_base_url'].rstrip('/')
token = secrets['home_assistant_token']
verify_ssl = secrets.get('home_assistant_verify_ssl', True)

# Build WebSocket URL
ws_url = ('wss://' if base_url.startswith('https://') else 'ws://') + base_url.split('://', 1)[1] + '/api/websocket'

# SSL options
sslopt = None
if ws_url.startswith('wss://'):
    sslopt = {}
    if not verify_ssl:
        sslopt['cert_reqs'] = ssl.CERT_NONE
        sslopt['check_hostname'] = False

print("=" * 80)
print("HOME ASSISTANT LOG CHECKER")
print("=" * 80)
print(f"Connecting to: {ws_url}")

# Connect to WebSocket
ws = websocket.create_connection(ws_url, timeout=20, sslopt=sslopt)

# Receive auth_required
auth_msg = ws.recv()
print(f"\n[1] Auth required: {auth_msg[:100]}...")

# Send auth
ws.send(json.dumps({'type': 'auth', 'access_token': token}))
auth_result = ws.recv()
print(f"[2] Auth result: {auth_result[:100]}...")

# Request system logs
print("\n[3] Requesting system logs...")
ws.send(json.dumps({'id': 1, 'type': 'system_log/list'}))
log_response = json.loads(ws.recv())

if log_response.get('success'):
    logs = log_response.get('result', [])
    print(f"    Total logs: {len(logs)}")

    # Filter for ESP32 and TTS related logs
    esp32_logs = [log for log in logs if 'esp32' in str(log).lower() or 'tts' in str(log).lower()]
    print(f"    ESP32/TTS related logs: {len(esp32_logs)}")

    print("\n[4] Recent ESP32/TTS logs (last 10):")
    for log in esp32_logs[-10:]:
        level = log.get('level', 'INFO')
        message = log.get('message', 'N/A')
        timestamp = log.get('timestamp', 'N/A')
        name = log.get('name', 'N/A')

        print(f"\n    [{timestamp}] {level} - {name}")
        print(f"    {message[:200]}")

    # Look for automation execution logs
    automation_logs = [log for log in logs if 'automation' in str(log).lower() and 'esp32' in str(log).lower()]
    if automation_logs:
        print(f"\n[5] Automation logs (last 5):")
        for log in automation_logs[-5:]:
            print(f"\n    {log.get('timestamp')} | {log.get('level')}")
            print(f"    {log.get('message')[:300]}")

else:
    print(f"    ERROR: {log_response}")

ws.close()

print("\n" + "=" * 80)
print("LOG CHECK COMPLETE")
print("=" * 80)
