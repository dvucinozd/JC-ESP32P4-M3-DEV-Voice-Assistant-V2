# Instructions for connecting to Home Assistant via MCP

This document describes a proven procedure for another AI agent to fetch logs or other data from Home Assistant. This repo does not contain MCP server scripts; use `help_scripts/` or your own MCP server if you already have one.

## 1. Prerequisites

- MCP CLI installed for the correct Python interpreter: `python -m pip install "mcp[cli]"`.
- Windows PowerShell has Python 3.11 with `pip` available (used for scripts that talk to Home Assistant). The WSL `python3` in this environment has no `pip` or `ensurepip`, so do not use it for extra packages.
- HA settings are in `main/config.h` (local only, not committed).
- To fetch logs, install `websocket-client`: `python -m pip install --user websocket-client`.

## 2. Secrets configuration

1. Open `main/config.h` and verify:
   - `HA_HOST` / `HA_HOSTNAME`: e.g. `<HA_IP>`.
   - `HA_PORT`: `9000`.
   - `HA_TOKEN`: valid long-lived token (20+ chars).
   - `HA_USE_SSL`: `false` if HA uses HTTP.
2. Values can be temporarily overridden in PowerShell:
   ```powershell
   $env:HOME_ASSISTANT_BASE_URL = 'https://example.local:9000'
   $env:HOME_ASSISTANT_TOKEN = 'xyz...'
   $env:HOME_ASSISTANT_VERIFY_SSL = '0'
   ```
3. If you use WSL/Git Bash, make sure `$HOME_ASSISTANT_BASE_URL` is not interpreted as a binary; use the PowerShell example above.

## 3. Starting the Home Assistant MCP server

### 3.1 Bash/WSL

This repo does not include MCP server scripts. If you use an external MCP server, start it per its instructions.

### 3.2 Windows PowerShell

If Git Bash cannot see `mcp.exe`, start the server directly (per the MCP server documentation you use).

### 3.3 Inspecting via MCP Inspector

In another terminal:

```bash
mcp dev <path_to_mcp_server_module>:server
```

The console prints a local URL and token for FastMCP Inspector; keep the process running while you inspect.

## 4. Typical tools available to the agent

- `list_home_assistant_entities(domain?, search?, limit?, attribute_filter?)`
- `get_home_assistant_state(entity_id)`
- `call_home_assistant_service(domain, service, entity_id?, service_data?)`

All use the REST API, so valid secrets are enough.

## 5. Fetching logs (WebSocket)

Home Assistant 2025.11.* no longer exposes logs via REST (`/api/error_log`, `/api/system_log*` return 404). For logs, use the WebSocket API `system_log/list`.

Example PowerShell script (reads `main/config.h`):

```powershell
@"
import json
import ssl
from pathlib import Path

import websocket

ROOT = Path('.').resolve()
cfg = (ROOT / 'main' / 'config.h').read_text(encoding='utf-8', errors='replace')
def define(name):
    import re
    m = re.search(rf'^\s*#\s*define\s+{name}\s+"([^"]*)"', cfg, re.MULTILINE)
    return m.group(1).strip() if m else None
def define_int(name):
    import re
    m = re.search(rf'^\s*#\s*define\s+{name}\s+([0-9]+)', cfg, re.MULTILINE)
    return int(m.group(1)) if m else None
def define_bool(name):
    import re
    m = re.search(rf'^\s*#\s*define\s+{name}\s+(true|false|0|1)', cfg, re.MULTILINE | re.IGNORECASE)
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
"@ | python
```

The result contains a list of log entries (`name`, `message`, `level`, `source`, `timestamp`, `count`). Filter as needed or print only `response["result"]`.

## 6. Connectivity check

Quick REST check:

```powershell
python - <<'PY'
import requests, re
cfg = open('main/config.h', encoding='utf-8').read()
def define(name):
    m = re.search(rf'^\s*#\s*define\s+{name}\s+"([^"]*)"', cfg, re.MULTILINE)
    return m.group(1).strip() if m else None
def define_int(name):
    m = re.search(rf'^\s*#\s*define\s+{name}\s+([0-9]+)', cfg, re.MULTILINE)
    return int(m.group(1)) if m else None
def define_bool(name):
    m = re.search(rf'^\s*#\s*define\s+{name}\s+(true|false|0|1)', cfg, re.MULTILINE | re.IGNORECASE)
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

- A `200 {"message": "API running."}` response confirms that the URL, token, and SSL settings are correct.

## 7. Common issues and fixes

| Problem | Cause | Fix |
| --- | --- | --- |
| `mcp: not found` in WSL/Git Bash | `mcp.exe` is only in Windows PATH | Run directly in PowerShell or add Windows `mcp.exe` to WSL PATH |
| REST `/api/error_log` returns 404 | Endpoint removed | Use WebSocket `system_log/list` as above |
| `ModuleNotFoundError: websocket` | Package not installed | `python -m pip install --user websocket-client` (PowerShell) |
| `python3 -m pip` in WSL says "No module named pip" | OS image without ensurepip | Run scripts that need extra packages from Windows Python |
| SSL errors when connecting | Self-signed certificate | Set `HA_USE_SSL` to `false` in `main/config.h` or use a valid cert |

## 8. What the agent should report to the user

1. Which MCP command was run (`run-home-assistant-mcp.sh`, `mcp run ...`).
2. Whether secrets were read correctly and whether variables were overridden.
3. Log summary (e.g., number of entries and key errors).
4. Any issues encountered (e.g., HA unavailable, invalid token).

With these steps, another AI agent has everything needed to connect to Home Assistant and interpret logs reliably via MCP.
