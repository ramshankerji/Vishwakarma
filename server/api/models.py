from django.db import models


class Installation(models.Model):
    """One installed copy of Vishwakarma, identified solely by its Ed25519 public key.

    The public key is generated locally by the installer and carries no personally
    identifiable information."""
    public_key = models.CharField(max_length=64, unique=True)  # Base64 of 32 raw bytes.
    first_seen = models.DateTimeField(auto_now_add=True)
    last_seen = models.DateTimeField(auto_now=True)
    app_version = models.BigIntegerField(default=0)

    def __str__(self):
        return self.public_key[:12]


class HardwareReport(models.Model):
    """System metrics snapshot. A new row arrives only when the client detects a change."""
    installation = models.ForeignKey(Installation, on_delete=models.CASCADE,
                                     related_name="hardware_reports")
    collected_utc = models.DateTimeField()
    received_utc = models.DateTimeField(auto_now_add=True)
    payload = models.JSONField()

    # Denormalized columns commonly used by the /api/stats dashboard.
    cpu_name = models.CharField(max_length=128, blank=True, default="")
    cpu_physical_cores = models.IntegerField(default=0)
    ram_total_mb = models.BigIntegerField(default=0)
    os_name = models.CharField(max_length=128, blank=True, default="")
    os_build = models.BigIntegerField(default=0)
    gpu_name = models.CharField(max_length=128, blank=True, default="")

    class Meta:
        indexes = [models.Index(fields=["installation", "-received_utc"])]


class UsageRecord(models.Model):
    """One 5 minute usage interval reported by a client."""
    installation = models.ForeignKey(Installation, on_delete=models.CASCADE,
                                     related_name="usage_records")
    client_row_id = models.BigIntegerField()  # UsageLog id in the client's local database.
    interval_start_utc = models.DateTimeField()
    interval_seconds = models.IntegerField(default=0)
    open_seconds = models.IntegerField(default=0)
    focus_seconds = models.IntegerField(default=0)
    left_clicks = models.BigIntegerField(default=0)
    middle_clicks = models.BigIntegerField(default=0)
    right_clicks = models.BigIntegerField(default=0)
    key_presses = models.BigIntegerField(default=0)
    ribbon_actions = models.JSONField(default=dict)  # {"commandId": count, ...}
    received_utc = models.DateTimeField(auto_now_add=True)

    class Meta:
        # A client only deletes rows after our acknowledgement, so retries can resend
        # the same client_row_id; the constraint makes ingestion idempotent.
        constraints = [
            models.UniqueConstraint(fields=["installation", "client_row_id"],
                                    name="unique_installation_client_row"),
        ]
        indexes = [models.Index(fields=["interval_start_utc"])]
