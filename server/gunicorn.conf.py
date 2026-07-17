"""Gunicorn configuration for the Mission Vishwakarma telemetry server.

Runs behind a Cloudflare tunnel: cloudflared makes outbound-only connections and
forwards plain HTTP to this socket. The origin is never publicly exposed, so the
bind must stay on loopback.
"""

# Loopback only. cloudflared connects from 127.0.0.1; nothing else should.
bind = "127.0.0.1:8000"
forwarded_allow_ips = "127.0.0.1"

# A Raspberry Pi has few cores and the traffic is tiny (~2 requests/day/client).
workers = 2

# Reject oversized request lines / headers at the edge of the process, before
# Django parses anything. Bodies are capped separately by Django
# (DATA_UPLOAD_MAX_MEMORY_SIZE = 1 MB).
limit_request_line = 8190
limit_request_fields = 50
limit_request_field_size = 8190

# Drop slow-loris style connections and recycle workers to bound memory growth.
timeout = 30
graceful_timeout = 30
keepalive = 5
max_requests = 1000
max_requests_jitter = 100

# Log to stdout/stderr so journald owns the logs (no files under ProtectSystem).
accesslog = "-"
errorlog = "-"
loglevel = "info"
