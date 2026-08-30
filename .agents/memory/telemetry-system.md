---
name: telemetry-system
description: "Telemetry/identity architecture — AccountManager Ed25519 keys, ImprovementData SQLite stats, Django server in ./server, canonical host mv-server.ramshanker.in"
metadata: 
  node_type: memory
  type: project
  originSessionId: dd9ebf8b-b9fa-4e47-be6a-d085bc3d9738
---

Telemetry + installation identity implemented 2026-07-04:

- **AccountManager** (code-core): Ed25519 installation key at `%LOCALAPPDATA%\Mission Vishwakarma\Credentials\InstallationKey.pem`, created by installer, re-created by app if missing. Per-launch session key signed by installation key. Compiled into both app and VishwakarmaSetup (installer now links libcrypto directly).
- **ImprovementData** (code-core): `ImprovementStatistics.db` (SQLite, WAL) in `%LOCALAPPDATA%\Mission Vishwakarma`. 5-min UsageLog rows from dedicated thread; HardwareStatistics row only when SHA-256 fingerprint of stable fields changes (free disk space deliberately excluded from fingerprint). Uploads: hardware ASAP (60s retry), usage every 24h; rows deleted on server ack. Wire protocol: body signed by session key in `X-MV-Signature` header.
- **Server** (./server): minimal Django (no admin/auth/sessions), endpoints /api/logs, /api/login (CORS only from https://mv.ramshanker.in, 501 placeholder), /api/stats dashboard, /api/health. `api/commands.py` is GENERATED from ListOfCommands.h — run `python code-miscellaneous/command_names_generator.py` (self-contained, repo-root anchored; replaced the old inline README snippet).
- **DEPLOYED LIVE 2026-07-17** on Raspberry Pi (user `ram`) behind Cloudflare tunnel. Hardened deploy assets committed: `server/gunicorn.conf.py` + `server/deploy/{mv-telemetry.service, mv-telemetry.env.example, cloudflared-config.yml.example}`. Layout: code in /opt/vishwakarma (read-only), dedicated system user `mvtelemetry`, DB at /var/lib/mv-telemetry/db.sqlite3, secret in root-only /etc/mv-telemetry.env. Gotchas: (1) gunicorn 26 control socket needs `AF_UNIX` in systemd RestrictAddressFamilies (else EAFNOSUPPORT error, telemetry still works); (2) prod DEBUG=off enables SECURE_SSL_REDIRECT so loopback health check 301s — test with `-H 'X-Forwarded-Proto: https'`. Stats appear: hardware ~60s after a RELEASE build runs, usage counts up to 24h later, dashboard cached 10min.
- **Canonical telemetry host**: `https://mv-server.ramshanker.in` — user's message also mentioned `mv-improvementdata.ramshanker.in`; mv-server was chosen, single constants: `kTelemetryUrl` in ImprovementData.cpp + settings.py/README. Debug builds use http://127.0.0.1:8000.
- Installer shows EULA+Privacy (RCDATA 203/204 from website/content/start) with Accept/Reject; skipped in --update mode.

**Why:** cross-cutting design spans C++ app, installer and server; the signature chain and URL choice must stay consistent on all three.
**How to apply:** when touching telemetry, keep client JSON field names in sync with server/api/views.py; regenerate commands.py when ListOfCommands.h changes. Related: [[release-signing-setup]], [[extension-system-mvp]].
