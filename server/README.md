# Mission Vishwakarma - Telemetry Server (Improvement Data)

Security hardened Django server receiving anonymous usage statistics and system metrics
from Vishwakarma.exe (`code-core/ImprovementData.cpp`). Deployed on a Raspberry Pi behind
a Cloudflare tunnel as **https://mv-server.ramshanker.in**.

## Endpoints

| URL           | Method | Purpose |
|---------------|--------|---------|
| `/api/logs`   | POST   | Telemetry ingestion. Ed25519 signature chain required (see below). |
| `/api/login`  | POST   | Reserved for future AccountManager based login. CORS allowed **only** from `https://mv.ramshanker.in` (future website telemetry). Returns 501 for now. |
| `/api/stats`  | GET    | Public dashboard of global, aggregated usage statistics (cached 10 minutes). |
| `/api/health` | GET    | Plain `ok` for tunnel / uptime monitoring. |

## Authentication of /api/logs

No accounts, no cookies, no PII. Each installation authenticates cryptographically
(mirrors `code-core/AccountManager.cpp`):

1. The installer creates a per-installation Ed25519 key pair; the public key is the
   installation's only identity.
2. On every application launch, an ephemeral session key pair is created; its public key
   is signed by the installation key (`sessionKeySignature` in the body).
3. Every request body is signed by the session key; the signature travels in the
   `X-MV-Signature` header.

The server verifies the whole chain before touching the database. Additional protection:
1 MB body cap, per-installation rate limit, idempotent ingestion (retried rows are not
duplicated), strict security headers (CSP, HSTS, nosniff), no Django admin/auth/session
apps installed at all.

## Local development (matches the Debug build of Vishwakarma.exe)

```bash
cd server
python -m venv venv
venv/bin/pip install -r requirements.txt        # Windows: venv\Scripts\pip
MV_DEBUG=1 venv/bin/python manage.py migrate
MV_DEBUG=1 venv/bin/python manage.py runserver 127.0.0.1:8000
```

Debug builds of Vishwakarma.exe post to `http://127.0.0.1:8000/api/logs`; release builds
post to `https://mv-server.ramshanker.in/api/logs` (constant `kTelemetryUrl` in
`code-core/ImprovementData.cpp`).

## Raspberry Pi deployment

```bash
sudo apt install python3-venv
git clone <repo> && cd Vishwakarma/server
python3 -m venv venv && venv/bin/pip install -r requirements.txt
export MV_SECRET_KEY="$(python3 -c 'import secrets; print(secrets.token_urlsafe(64))')"
venv/bin/python manage.py migrate
venv/bin/gunicorn --bind 127.0.0.1:8000 --workers 2 improvement_server.wsgi
```

Persist `MV_SECRET_KEY` (e.g. in a root-only environment file consumed by the systemd
unit). Example unit `/etc/systemd/system/mv-telemetry.service`:

```ini
[Unit]
Description=Mission Vishwakarma telemetry server
After=network.target

[Service]
User=pi
WorkingDirectory=/home/pi/Vishwakarma/server
EnvironmentFile=/etc/mv-telemetry.env
ExecStart=/home/pi/Vishwakarma/server/venv/bin/gunicorn --bind 127.0.0.1:8000 --workers 2 improvement_server.wsgi
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Cloudflare tunnel (`cloudflared`) config — TLS terminates at Cloudflare, the tunnel
forwards plain HTTP to gunicorn on localhost:

```yaml
tunnel: <tunnel-id>
credentials-file: /home/pi/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: mv-server.ramshanker.in
    service: http://127.0.0.1:8000
  - service: http_status:404
```

## Regenerating the command name mapping

`api/commands.py` maps ribbon command IDs to names for the dashboard and is generated
from `code-core/ListOfCommands.h`. Regenerate after adding commands:

```python
import re, io
src = io.open('code-core/ListOfCommands.h', encoding='utf-8').read()
pairs = re.findall(r'^\s*([A-Z0-9_]+)\s*=\s*(\d{10})\s*,', src, re.M)
out = ['# Generated from code-core/ListOfCommands.h - command id to name mapping.',
       '# Regenerate when new commands are added (see server/README.md).', '', 'COMMAND_NAMES = {']
out += ['    %s: "%s",' % (num, name) for name, num in pairs] + ['}']
io.open('server/api/commands.py', 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
```
