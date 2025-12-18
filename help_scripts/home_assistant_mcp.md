# Upute za spajanje na Home Assistant preko MCP-a

Ovaj dokument opisuje provjereni postupak kako drugi AI agent može pokretati FastMCP server iz ovog repozitorija, spajati se na Home Assistant instancu i dohvaćati logove ili druge podatke.

## 1. Preduvjeti

- Instaliran MCP CLI za odgovarajući Python interpreter: `python -m pip install "mcp[cli]"`.
- U Windows PowerShellu je dostupan Python 3.11 s `pip`-om (koristi se za skripte koje rade s Home Assistantom). WSL-ov `python3` u ovom okruženju nema `pip` ni `ensurepip`, pa ga ne koristi za dodatne pakete.
- U `esphome/secrets.yaml` su popunjeni `home_assistant_base_url`, `home_assistant_token` i `home_assistant_verify_ssl`.
- Za dohvat logova potreban je paket `websocket-client`: `python -m pip install --user websocket-client`.

## 2. Konfiguracija tajni

1. Otvori `esphome/secrets.yaml` i provjeri:
   - `home_assistant_base_url`: npr. `http://kucni.local:9000`.
   - `home_assistant_token`: valjani long-lived token (20+ znakova).
   - `home_assistant_verify_ssl`: `false` ako HA koristi lokalni certifikat koji nije u trust storeu.
2. Vrijednosti se mogu privremeno nadjačati iz PowerShella:
   ```powershell
   $env:HOME_ASSISTANT_BASE_URL = 'https://example.local:9000'
   $env:HOME_ASSISTANT_TOKEN = 'xyz...'
   $env:HOME_ASSISTANT_VERIFY_SSL = '0'
   ```
3. Ako koristiš WSL/Git Bash, pazi da `$HOME_ASSISTANT_BASE_URL` ne završi interpretiran kao binarka; zato postoji PowerShell primjer iznad.

## 3. Pokretanje Home Assistant MCP servera

### 3.1 Bash/WSL

```bash
scripts/esphome/run-home-assistant-mcp.sh --base-url http://kucni.local:9000
```

- Opcionalno dodaj `--transport sse` ili `--transport streamable-http` ako treba drugi MCP transport.

### 3.2 Windows PowerShell

Ako Git Bash ne vidi `mcp.exe`, pokreni server direktno:

```powershell
$env:HOME_ASSISTANT_BASE_URL='http://kucni.local:9000'
python -m pip install --user "mcp[cli]"   # prvi put
mcp run D:/AI/esp32-s3-voice-assistant/scripts/esphome/home_assistant_mcp_server.py:server
```

### 3.3 Pregled preko MCP Inspectora

U drugom terminalu:

```bash
mcp dev scripts/esphome/home_assistant_mcp_server.py:server
```

Konzola će ispisati lokalni URL i token za FastMCP Inspector; drži proces aktivnim dok pregledavaš.

## 4. Tipični alati dostupni agentu

- `list_home_assistant_entities(domain?, search?, limit?, attribute_filter?)`
- `get_home_assistant_state(entity_id)`
- `call_home_assistant_service(domain, service, entity_id?, service_data?)`

Svi koriste REST API, pa je dovoljno imati valjane tajne.

## 5. Dohvat logova (WebSocket)

Home Assistant 2025.11.* više ne izlaže logove preko REST ruta (`/api/error_log`, `/api/system_log*` vraćaju 404). Za logove koristi WebSocket API `system_log/list`.

Primjer skripte iz PowerShella:

```powershell
@'
import json
import ssl
from pathlib import Path

import websocket
import yaml

ROOT = Path('.').resolve()
secrets = yaml.safe_load((ROOT / 'esphome' / 'secrets.yaml').read_text())
base_url = secrets['home_assistant_base_url'].rstrip('/')
token = secrets['home_assistant_token']
verify_ssl = secrets.get('home_assistant_verify_ssl', True)

ws_url = ('wss://' if base_url.startswith('https://') else 'ws://') + base_url.split('://', 1)[1] + '/api/websocket'
sslopt = None
if ws_url.startswith('wss://'):
    sslopt = {}
    if not verify_ssl:
        sslopt['cert_reqs'] = ssl.CERT_NONE
        sslopt['check_hostname'] = False

ws = websocket.create_connection(ws_url, timeout=20, sslopt=sslopt)
ws.recv()  # auth_required
ws.send(json.dumps({'type': 'auth', 'access_token': token}))
ws.recv()  # auth_ok
ws.send(json.dumps({'id': 1, 'type': 'system_log/list'}))
response = json.loads(ws.recv())
print(json.dumps(response, indent=2))
ws.close()
'@ | python
```

Rezultat sadrži listu log zapisa (`name`, `message`, `level`, `source`, `timestamp`, `count`). Po potrebi filtriraj ili ispiši samo `response["result"]`.

## 6. Provjera povezivosti

Brza REST provjera:

```powershell
python - <<'PY'
import requests, yaml
secrets = yaml.safe_load(open('esphome/secrets.yaml', encoding='utf-8'))
url = secrets['home_assistant_base_url'].rstrip('/') + '/api/'
headers = {'Authorization': f"Bearer {secrets['home_assistant_token']}"}
resp = requests.get(url, headers=headers, verify=bool(secrets.get('home_assistant_verify_ssl', True)))
print(resp.status_code, resp.text)
PY
```

- Odgovor `200 {"message": "API running."}` potvrđuje da su URL, token i SSL postavke ispravni.

## 7. Tipične poteškoće i rješenja

| Problem | Uzrok | Rješenje |
| --- | --- | --- |
| `mcp: not found` u WSL/Git Bash | `mcp.exe` je samo u Windows PATH-u | Pokreni direktno iz PowerShella ili dodaj Windows `mcp.exe` u WSL PATH |
| REST `/api/error_log` vraća 404 | Endpoint uklonjen | Koristi WebSocket `system_log/list` kao gore |
| `ModuleNotFoundError: websocket` | Paket nije instaliran | `python -m pip install --user websocket-client` (PowerShell) |
| `python3 -m pip` u WSL javlja “No module named pip” | OS image bez ensurepip | Pokreći skripte koje trebaju dodatne pakete iz Windows Pythona |
| SSL greške pri spajanju | Samopotpisani certifikat | Stavi `home_assistant_verify_ssl: false` u `secrets.yaml` (ili exportaj `HOME_ASSISTANT_VERIFY_SSL=0`) |

## 8. Što agent treba prijaviti korisniku

1. Koju je MCP komandu pokrenuo (`run-home-assistant-mcp.sh`, `mcp run ...`).
2. Jesu li tajne uredno pročitane i jesu li varijable nadjačane.
3. Sažetak logova (npr. broj zapisa i ključne greške).
4. Svaki problem na koji je naišao (npr. nedostupan HA, nevažeći token).

S ovim koracima drugi AI agent dobiva sve što treba da se pouzdano spoji na Home Assistant i interpretira logove putem MCP okvira.
