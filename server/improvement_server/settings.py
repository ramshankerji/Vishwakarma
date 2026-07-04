"""
Security hardened Django settings for the Mission Vishwakarma telemetry server.

Deployment target: Raspberry Pi behind a Cloudflare tunnel, public name
https://mv-server.ramshanker.in . TLS terminates at Cloudflare; cloudflared
forwards plain HTTP to gunicorn on 127.0.0.1:8000 with X-Forwarded-Proto set.

Environment variables (production):
  MV_SECRET_KEY   required. Long random string (Django SECRET_KEY).
  MV_DEBUG        "1" enables debug mode. Never set in production.
  MV_DB_PATH      optional path for the SQLite database file.
"""
import os
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent

DEBUG = os.environ.get("MV_DEBUG") == "1"

SECRET_KEY = os.environ.get("MV_SECRET_KEY", "")
if not SECRET_KEY:
    if DEBUG:
        SECRET_KEY = "insecure-dev-only-key-do-not-deploy"
    else:
        raise RuntimeError("MV_SECRET_KEY environment variable must be set in production.")

ALLOWED_HOSTS = ["mv-server.ramshanker.in", "localhost", "127.0.0.1"]

# Deliberately minimal application surface: no admin, no auth, no sessions.
INSTALLED_APPS = [
    "corsheaders",
    "api",
]

MIDDLEWARE = [
    "django.middleware.security.SecurityMiddleware",
    "corsheaders.middleware.CorsMiddleware",
    "django.middleware.common.CommonMiddleware",
    "api.middleware.SecurityHeadersMiddleware",
]

ROOT_URLCONF = "improvement_server.urls"

TEMPLATES = [
    {
        "BACKEND": "django.template.backends.django.DjangoTemplates",
        "DIRS": [],
        "APP_DIRS": True,
        "OPTIONS": {"context_processors": []},
    },
]

WSGI_APPLICATION = "improvement_server.wsgi.application"

DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": os.environ.get("MV_DB_PATH", BASE_DIR / "db.sqlite3"),
        "OPTIONS": {
            "init_command": "PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000;",
        },
    }
}

CACHES = {
    # locmem is per-process: rate limiting is per gunicorn worker, which is an
    # acceptable coarse abuse damper for this deployment size.
    "default": {"BACKEND": "django.core.cache.backends.locmem.LocMemCache"}
}

LANGUAGE_CODE = "en-us"
TIME_ZONE = "UTC"
USE_TZ = True

DEFAULT_AUTO_FIELD = "django.db.models.BigAutoField"

# ------------------------------------------------------------------ Hardening
# Telemetry uploads are small; reject anything bigger long before parsing.
DATA_UPLOAD_MAX_MEMORY_SIZE = 1 * 1024 * 1024  # 1 MB
DATA_UPLOAD_MAX_NUMBER_FIELDS = 100

SECURE_CONTENT_TYPE_NOSNIFF = True
SECURE_REFERRER_POLICY = "same-origin"
SECURE_CROSS_ORIGIN_OPENER_POLICY = "same-origin"
X_FRAME_OPTIONS = "DENY"

if not DEBUG:
    # TLS is terminated by Cloudflare; trust its forwarded protocol header.
    SECURE_PROXY_SSL_HEADER = ("HTTP_X_FORWARDED_PROTO", "https")
    SECURE_SSL_REDIRECT = True
    SECURE_HSTS_SECONDS = 31536000
    SECURE_HSTS_INCLUDE_SUBDOMAINS = False  # Other subdomains are served elsewhere.
    SESSION_COOKIE_SECURE = True
    CSRF_COOKIE_SECURE = True

CSRF_TRUSTED_ORIGINS = ["https://mv-server.ramshanker.in"]

# CORS: only the /api/login endpoint may be called cross-origin, and only from
# our own website (future website telemetry / AccountManager based login).
CORS_ALLOWED_ORIGINS = ["https://mv.ramshanker.in"]
CORS_URLS_REGEX = r"^/api/login$"
CORS_ALLOW_METHODS = ["POST", "OPTIONS"]

LOGGING = {
    "version": 1,
    "disable_existing_loggers": False,
    "handlers": {"console": {"class": "logging.StreamHandler"}},
    "root": {"handlers": ["console"], "level": "WARNING"},
    "loggers": {
        "django.security": {"handlers": ["console"], "level": "WARNING", "propagate": False},
        "api": {"handlers": ["console"], "level": "INFO", "propagate": False},
    },
}
