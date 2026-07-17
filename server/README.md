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

The origin is never exposed publicly: gunicorn binds to loopback only and the
Cloudflare tunnel makes outbound-only connections to it. Committed deploy assets
live in `deploy/` and `gunicorn.conf.py`. Run as a dedicated unprivileged system
user with the code under `/opt` (read-only) and the database on a writable state
directory under `/var/lib`.

```bash
sudo apt update && sudo apt install -y python3-venv git

# Dedicated service account (system user, home on the writable state dir).
sudo useradd --system --create-home --home-dir /var/lib/mv-telemetry \
    --shell /usr/sbin/nologin mvtelemetry

# Code (stays read-only to the service under ProtectSystem=strict).
sudo git clone <repo-url> /opt/vishwakarma
cd /opt/vishwakarma/server
sudo python3 -m venv venv
sudo ./venv/bin/pip install -r requirements.txt

# Root-only env file with the secret and the DB path.
sudo install -m 600 -o root -g root deploy/mv-telemetry.env.example /etc/mv-telemetry.env
sudo sed -i "s|__SECRET__|$(python3 -c 'import secrets; print(secrets.token_urlsafe(64))')|" \
    /etc/mv-telemetry.env

# Initialise the database in the writable state directory.
sudo -u mvtelemetry MV_SECRET_KEY=dummy MV_DB_PATH=/var/lib/mv-telemetry/db.sqlite3 \
    ./venv/bin/python manage.py migrate

# Install and start the sandboxed service.
sudo cp deploy/mv-telemetry.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mv-telemetry
systemctl status mv-telemetry
```

The systemd unit (`deploy/mv-telemetry.service`) runs gunicorn via
`gunicorn.conf.py` (loopback bind, request-size limits, worker recycling) under a
strict sandbox: read-only filesystem except `/var/lib/mv-telemetry`, no new
privileges, no capabilities, and a `@system-service` syscall filter. Audit it with
`systemd-analyze security mv-telemetry`; smoke-test with
`curl -sS http://127.0.0.1:8000/api/health` (expects `ok`).

## Cloudflare tunnel

TLS terminates at Cloudflare; the tunnel forwards plain HTTP to gunicorn on
loopback, so no inbound firewall ports are required.

```bash
# Install cloudflared for arm64, then authenticate and create the tunnel:
cloudflared tunnel login
cloudflared tunnel create mv-server
cloudflared tunnel route dns mv-server mv-server.ramshanker.in

# Fill in the tunnel id + credentials path, then run cloudflared as a service:
sudo mkdir -p /etc/cloudflared
sudo cp /opt/vishwakarma/server/deploy/cloudflared-config.yml.example /etc/cloudflared/config.yml
sudo nano /etc/cloudflared/config.yml
sudo cloudflared service install
sudo systemctl enable --now cloudflared
```

Recommended edge settings: SSL/TLS mode "Full", "Always Use HTTPS" on, and a WAF
rate-limit rule on `/api/logs`. Keep the host firewall default-deny inbound
(`sudo ufw default deny incoming && sudo ufw allow OpenSSH && sudo ufw enable`) —
the tunnel needs no open ports.

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
