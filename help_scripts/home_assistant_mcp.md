# Upute za spajanje na Home Assistant preko MCP-a

Ovaj dokument opisuje provjereni postupak kako drugi AI agent može dohvatiti logove ili druge podatke iz Home Assistanta. Ovaj repozitorij ne sadrži MCP server skripte; koristi `help_scripts/` ili vlastiti MCP server ako ga već imaš.

## 1. Preduvjeti

- Instaliran MCP CLI za odgovarajući Python interpreter: `python -m pip install "mcp[cli]"`.
- U Windows PowerShellu je dostupan Python 3.11 s `pip`-om (koristi se za skripte koje rade s Home Assistantom). WSL-ov `python3` u ovom okruženju nema `pip` ni `ensurepip`, pa ga ne koristi za dodatne pakete.
- HA postavke su u `main/config.h` (lokalno, ne ide u git).
- Za dohvat logova potreban je paket `websocket-client`: `python -m pip install --user websocket-client`.

## 2. Konfiguracija tajni

1. Otvori `main/config.h` i provjeri:
   - `HA_HOST` / `HA_HOSTNAME`: npr. `<HA_IP>`.
   - `HA_PORT`: `9000`.
   - `HA_TOKEN`: valjani long-lived token (20+ znakova).
   - `HA_USE_SSL`: `false` ako HA koristi HTTP.
2. Vrijednosti se mogu privremeno nadjačati iz PowerShella:
   ```powershell
   $env:HOME_ASSISTANT_BASE_URL = 'https://example.local:9000'
   $env:HOME_ASSISTANT_TOKEN = 'xyz...'
   $env:HOME_ASSISTANT_VERIFY_SSL = '0'
   ```
3. Ako koristiš WSL/Git Bash, pazi da `$HOME_ASSISTANT_BASE_URL` ne završi interpretiran kao binarka; zato postoji PowerShell primjer iznad.

## 3. Pokretanje Home Assistant MCP servera

### 3.1 Bash/WSL

Ovaj repo nema MCP server skripte. Ako koristiš vanjski MCP server, pokreni ga prema njegovim uputama.

### 3.2 Windows PowerShell

Ako Git Bash ne vidi `mcp.exe`, pokreni server direktno (prema dokumentaciji MCP servera koji koristiš).

### 3.3 Pregled preko MCP Inspectora

U drugom terminalu:

```bash
mcp dev <path_to_mcp_server_module>:server
```

Konzola će ispisati lokalni URL i token za FastMCP Inspector; drži proces aktivnim dok pregledavaš.

## 4. Tipični alati dostupni agentu

- `list_home_assistant_entities(domain?, search?, limit?, attribute_filter?)`
- `get_home_assistant_state(entity_id)`
- `call_home_assistant_service(domain, service, entity_id?, service_data?)`

Svi koriste REST API, pa je dovoljno imati valjane tajne.

## 5. Dohvat logova (WebSocket)

Home Assistant 2025.11.* više ne izlaže logove preko REST ruta (`/api/error_log`, `/api/system_log*` vraćaju 404). Za logove koristi WebSocket API `system_log/list`.

Primjer skripte iz PowerShella (čita `main/config.h`):

```powershell
@'
import json
import ssl
from pathlib import Path

import websocket

ROOT = Path('.').resolve()
cfg = (ROOT / 'main' / 'config.h').read_text(encoding='utf-8', errors='replace')
def define(name):
    import re
    m = re.search(rf'^\\s*#\\s*define\\s+{name}\\s+\"([^\"]*)\"', cfg, re.MULTILINE)
    return m.group(1).strip() if m else None
def define_int(name):
    import re
    m = re.search(rf'^\\s*#\\s*define\\s+{name}\\s+([0-9]+)', cfg, re.MULTILINE)
    return int(m.group(1)) if m else None
def define_bool(name):
    import re
    m = re.search(rf'^\\s*#\\s*define\\s+{name}\\s+(true|false|0|1)', cfg, re.MULTILINE | re.IGNORECASE)
    if not m:
        return None
    return m.group(1).lower() in ('1', 'true')

host = define('HA_HOST') or define('HA_HOSTNAME')
port = define_int('HA_PORT') or 9000
token = define('HA_TOKEN')
use_ssl = define_bool('HA_USE_SSL') or False
base_url = f"{'https' if use_ssl else 'http'}://{host}:{port}"
verify_ssl = use_ssl

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
import requests, re
cfg = open('main/config.h', encoding='utf-8').read()
def define(name):
    m = re.search(rf'^\\s*#\\s*define\\s+{name}\\s+\"([^\"]*)\"', cfg, re.MULTILINE)
    return m.group(1).strip() if m else None
def define_int(name):
    m = re.search(rf'^\\s*#\\s*define\\s+{name}\\s+([0-9]+)', cfg, re.MULTILINE)
    return int(m.group(1)) if m else None
def define_bool(name):
    m = re.search(rf'^\\s*#\\s*define\\s+{name}\\s+(true|false|0|1)', cfg, re.MULTILINE | re.IGNORECASE)
    if not m:
        return None
    return m.group(1).lower() in ('1', 'true')
host = define('HA_HOST') or define('HA_HOSTNAME')
port = define_int('HA_PORT') or 9000
token = define('HA_TOKEN')
use_ssl = define_bool('HA_USE_SSL') or False
url = f"{'https' if use_ssl else 'http'}://{host}:{port}/api/"
headers = {'Authorization': f"Bearer {token}"}
resp = requests.get(url, headers=headers, verify=use_ssl)
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
| SSL greške pri spajanju | Samopotpisani certifikat | Postavi `HA_USE_SSL` na `false` u `main/config.h` ili koristi validan certifikat |

## 8. Što agent treba prijaviti korisniku

1. Koju je MCP komandu pokrenuo (`run-home-assistant-mcp.sh`, `mcp run ...`).
2. Jesu li tajne uredno pročitane i jesu li varijable nadjačane.
3. Sažetak logova (npr. broj zapisa i ključne greške).
4. Svaki problem na koji je naišao (npr. nedostupan HA, nevažeći token).

S ovim koracima drugi AI agent dobiva sve što treba da se pouzdano spoji na Home Assistant i interpretira logove putem MCP okvira.
