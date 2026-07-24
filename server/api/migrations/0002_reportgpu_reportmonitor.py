# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
import django.db.models.deletion
from django.db import migrations, models


def backfill_gpus(apps, schema_editor):
    """Reports collected before the per-GPU array carried a single flat gpuName/gpuVramMB pair.
    Move those into ReportGpu so the dashboard has one uniform source; the PCI ids stay 0,
    which gpu_names.gpu_label() renders as the bare driver description."""
    HardwareReport = apps.get_model("api", "HardwareReport")
    ReportGpu = apps.get_model("api", "ReportGpu")
    rows = []
    for report in HardwareReport.objects.all().iterator():
        payload = report.payload if isinstance(report.payload, dict) else {}
        name = str(payload.get("gpuName", ""))[:128]
        if not name:
            continue
        try:
            vram = int(payload.get("gpuVramMB", 0) or 0)
        except (TypeError, ValueError):
            vram = 0
        rows.append(ReportGpu(
            report=report,
            name=name,
            vram_mb=vram,
            driver_version=str(payload.get("gpuDriverVersion", ""))[:32],
            discrete=bool(payload.get("gpuDiscrete", False)),
        ))
    ReportGpu.objects.bulk_create(rows, batch_size=500)


class Migration(migrations.Migration):

    dependencies = [
        ("api", "0001_initial"),
    ]

    operations = [
        migrations.CreateModel(
            name="ReportGpu",
            fields=[
                ("id", models.BigAutoField(auto_created=True, primary_key=True, serialize=False, verbose_name="ID")),
                ("name", models.CharField(blank=True, default="", max_length=128)),
                ("vendor_id", models.IntegerField(default=0)),
                ("device_id", models.IntegerField(default=0)),
                ("vram_mb", models.BigIntegerField(default=0)),
                ("driver_version", models.CharField(blank=True, default="", max_length=32)),
                ("discrete", models.BooleanField(default=False)),
                ("report", models.ForeignKey(on_delete=django.db.models.deletion.CASCADE, related_name="gpus", to="api.hardwarereport")),
            ],
        ),
        migrations.CreateModel(
            name="ReportMonitor",
            fields=[
                ("id", models.BigAutoField(auto_created=True, primary_key=True, serialize=False, verbose_name="ID")),
                ("width_px", models.IntegerField(default=0)),
                ("height_px", models.IntegerField(default=0)),
                ("refresh_hz", models.IntegerField(default=0)),
                ("width_mm", models.IntegerField(default=0)),
                ("height_mm", models.IntegerField(default=0)),
                ("report", models.ForeignKey(on_delete=django.db.models.deletion.CASCADE, related_name="monitors", to="api.hardwarereport")),
            ],
        ),
        migrations.RunPython(backfill_gpus, migrations.RunPython.noop),
    ]
