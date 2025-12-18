#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Test HA API direktno (simulacija MCP server poziva)
"""
import asyncio
import aiohttp
import json
import sys
import os
from pathlib import Path

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
    base_url = (os.environ.get("HOME_ASSISTANT_BASE_URL") or "").strip()
    token = (os.environ.get("HOME_ASSISTANT_TOKEN") or "").strip()
    verify_ssl = _to_bool(os.environ.get("HOME_ASSISTANT_VERIFY_SSL"), default=True)

    if base_url and token:
        return base_url.rstrip("/"), token, verify_ssl

    try:
        import yaml
    except Exception:
        yaml = None

    root = Path(__file__).resolve().parents[1]
    secrets_path = root / "esphome" / "secrets.yaml"
    if yaml is None or not secrets_path.exists():
        return base_url.rstrip("/"), token, verify_ssl

    secrets = yaml.safe_load(secrets_path.read_text(encoding="utf-8")) or {}
    base_url = (secrets.get("home_assistant_base_url") or base_url).strip().rstrip("/")
    token = (secrets.get("home_assistant_token") or token).strip()
    verify_ssl = _to_bool(secrets.get("home_assistant_verify_ssl"), default=verify_ssl)
    return base_url, token, verify_ssl


HA_URL, HA_TOKEN, VERIFY_SSL = load_ha_secrets()
if not HA_URL or not HA_TOKEN:
    print("Missing Home Assistant credentials.")
    print("Set HOME_ASSISTANT_BASE_URL and HOME_ASSISTANT_TOKEN, or fill esphome/secrets.yaml.")
    sys.exit(2)

HEADERS = {
    "Authorization": f"Bearer {HA_TOKEN}",
    "Content-Type": "application/json"
}

async def test_ha_api():
    print("=" * 70)
    print("TEST HOME ASSISTANT API (Simulacija MCP Server)")
    print("=" * 70)
    print()

    async with aiohttp.ClientSession() as session:

        # ================================================================
        # TEST 1: Get Config
        # ================================================================
        print("[TEST 1] Dohvaćam HA config...")
        try:
            async with session.get(
                f"{HA_URL}/api/config",
                headers=HEADERS,
                timeout=aiohttp.ClientTimeout(total=5),
                ssl=VERIFY_SSL  # Set false to skip SSL verification for self-signed cert
            ) as resp:
                if resp.status == 200:
                    config = await resp.json()
                    print(f"✅ HA verzija: {config.get('version')}")
                    print(f"   Location: {config.get('location_name')}")
                    print(f"   Timezone: {config.get('time_zone')}")
                else:
                    print(f"❌ Error: HTTP {resp.status}")
        except Exception as e:
            print(f"❌ Exception: {e}")

        print()

        # ================================================================
        # TEST 2: List ESP32 Entities
        # ================================================================
        print("[TEST 2] Dohvaćam ESP32 entitete...")
        try:
            async with session.get(
                f"{HA_URL}/api/states",
                headers=HEADERS,
                timeout=aiohttp.ClientTimeout(total=10),
                ssl=VERIFY_SSL
            ) as resp:
                if resp.status == 200:
                    all_entities = await resp.json()
                    esp32_entities = [
                        e for e in all_entities
                        if 'esp32' in e.get('entity_id', '').lower()
                    ]

                    print(f"✅ Pronađeno {len(esp32_entities)} ESP32 entiteta:")
                    for entity in esp32_entities[:10]:
                        entity_id = entity.get('entity_id')
                        state = entity.get('state')
                        name = entity.get('attributes', {}).get('friendly_name', entity_id)
                        print(f"   - {entity_id}: {state}")
                else:
                    print(f"❌ Error: HTTP {resp.status}")
        except Exception as e:
            print(f"❌ Exception: {e}")

        print()

        # ================================================================
        # TEST 3: Get Automation State
        # ================================================================
        print("[TEST 3] Dohvaćam stanje TTS automation-a...")
        automation_id = "automation.esp32s3_full_voice_assistant_stt_ai_tts"
        try:
            async with session.get(
                f"{HA_URL}/api/states/{automation_id}",
                headers=HEADERS,
                timeout=aiohttp.ClientTimeout(total=5),
                ssl=VERIFY_SSL
            ) as resp:
                if resp.status == 200:
                    data = await resp.json()
                    print(f"✅ Automation: {automation_id}")
                    print(f"   State: {data.get('state')}")
                    print(f"   Last triggered: {data.get('attributes', {}).get('last_triggered', 'nikad')}")
                    print(f"   Current: {data.get('attributes', {}).get('current', 0)}")
                elif resp.status == 404:
                    print(f"⚠️  Automation ne postoji!")
                else:
                    print(f"❌ Error: HTTP {resp.status}")
        except Exception as e:
            print(f"❌ Exception: {e}")

        print()

        # ================================================================
        # TEST 4: Get Logs
        # ================================================================
        print("[TEST 4] Dohvaćam HA logove (filter: 'esp32')...")
        try:
            async with session.get(
                f"{HA_URL}/api/error_log",
                headers=HEADERS,
                timeout=aiohttp.ClientTimeout(total=10),
                ssl=VERIFY_SSL
            ) as resp:
                if resp.status == 200:
                    log_text = await resp.text()
                    lines = log_text.split('\n')

                    # Filtriraj ESP32 log linije
                    esp32_lines = [
                        line for line in lines
                        if 'esp32' in line.lower()
                    ]

                    print(f"✅ Pronađeno {len(esp32_lines)} ESP32 log linija (zadnjih 10):")
                    for line in esp32_lines[-10:]:
                        print(f"   {line[:100]}")
                else:
                    print(f"❌ Error: HTTP {resp.status}")
        except Exception as e:
            print(f"❌ Exception: {e}")

        print()

        # ================================================================
        # TEST 5: Send MQTT Message (Test MCP functionality)
        # ================================================================
        print("[TEST 5] Šaljem MQTT poruku (test MCP call_service)...")
        try:
            async with session.post(
                f"{HA_URL}/api/services/mqtt/publish",
                headers=HEADERS,
                json={
                    "topic": "esp32s3-va/show_text",
                    "payload": "MCP Server test!"
                },
                timeout=aiohttp.ClientTimeout(total=10),
                ssl=VERIFY_SSL
            ) as resp:
                if resp.status in [200, 201]:
                    print(f"✅ MQTT poruka poslana na ESP32 display!")
                else:
                    print(f"❌ Error: HTTP {resp.status} - {await resp.text()}")
        except Exception as e:
            print(f"❌ Exception: {e}")

    print()
    print("=" * 70)
    print("TEST ZAVRŠEN")
    print("=" * 70)

if __name__ == "__main__":
    asyncio.run(test_ha_api())
