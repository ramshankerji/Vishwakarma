"""Ed25519 verification of telemetry uploads.

Trust chain (mirrors code-core/AccountManager.cpp):
  installation key  --signs-->  session public key  --signs-->  request body
The client sends its installation public key, per-launch session public key and the
installation-key signature over the raw session public key inside the JSON body; the
session-key signature over the exact body bytes travels in the X-MV-Signature header.
"""
import base64

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

RAW_KEY_LENGTH = 32
RAW_SIGNATURE_LENGTH = 64


def _decode(b64: str, expected_length: int) -> bytes | None:
    try:
        raw = base64.b64decode(b64, validate=True)
    except (ValueError, TypeError):
        return None
    return raw if len(raw) == expected_length else None


def verify_request(installation_key_b64: str, session_key_b64: str,
                   session_signature_b64: str, body_signature_b64: str,
                   body: bytes) -> bool:
    """Returns True only when the full signature chain is valid."""
    installation_raw = _decode(installation_key_b64, RAW_KEY_LENGTH)
    session_raw = _decode(session_key_b64, RAW_KEY_LENGTH)
    session_sig = _decode(session_signature_b64, RAW_SIGNATURE_LENGTH)
    body_sig = _decode(body_signature_b64, RAW_SIGNATURE_LENGTH)
    if not (installation_raw and session_raw and session_sig and body_sig):
        return False
    try:
        Ed25519PublicKey.from_public_bytes(installation_raw).verify(session_sig, session_raw)
        Ed25519PublicKey.from_public_bytes(session_raw).verify(body_sig, body)
        return True
    except (InvalidSignature, ValueError):
        return False
