# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

import json
import logging
from collections import Counter
from datetime import datetime, timedelta, timezone

from django.core.cache import cache
from django.db import transaction
from django.db.models import Count, Sum
from django.http import HttpResponse, JsonResponse
from django.shortcuts import render
from django.utils import timezone as django_timezone
from django.views.decorators.cache import cache_page
from django.views.decorators.csrf import csrf_exempt
from django.views.decorators.http import require_GET, require_POST

from .commands import COMMAND_NAMES
from .crypto import verify_request
from .gpu_names import gpu_label
from .models import HardwareReport, Installation, ReportGpu, ReportMonitor, UsageRecord

logger = logging.getLogger("api")

MAX_BODY_BYTES = 1 * 1024 * 1024
MAX_USAGE_ROWS_PER_REQUEST = 4096
MAX_GPUS_PER_REPORT = 8
MAX_MONITORS_PER_REPORT = 16
RATE_LIMIT_PER_HOUR = 120  # Per installation key; normal clients need ~2 requests a day.


def _parse_iso_utc(value):
    try:
        return datetime.fromisoformat(str(value).replace("Z", "+00:00")).astimezone(timezone.utc)
    except (ValueError, TypeError):
        return None


def _int(value, limit):
    """Clamped integer out of untrusted json. Anything unusable becomes 0."""
    try:
        return max(0, min(int(value or 0), limit))
    except (TypeError, ValueError):
        return 0


def _store_gpus(report, gpus):
    """One ReportGpu row per adapter. Element 0 is the highest-VRAM one (client sorts them)."""
    if not isinstance(gpus, list):
        return
    rows = [ReportGpu(
        report=report,
        name=str(gpu.get("name", ""))[:128],
        vendor_id=_int(gpu.get("vendorId"), 0xFFFF),
        device_id=_int(gpu.get("deviceId"), 0xFFFF),
        vram_mb=_int(gpu.get("vramMB"), 1024 * 1024),
        driver_version=str(gpu.get("driverVersion", ""))[:32],
        discrete=bool(gpu.get("discrete", False)),
    ) for gpu in gpus[:MAX_GPUS_PER_REPORT] if isinstance(gpu, dict)]
    ReportGpu.objects.bulk_create(rows)


def _store_monitors(report, monitors):
    """One ReportMonitor row per attached display."""
    if not isinstance(monitors, list):
        return
    rows = [ReportMonitor(
        report=report,
        width_px=_int(monitor.get("widthPx"), 100000),
        height_px=_int(monitor.get("heightPx"), 100000),
        refresh_hz=_int(monitor.get("refreshHz"), 1000),
        width_mm=_int(monitor.get("widthMm"), 10000),
        height_mm=_int(monitor.get("heightMm"), 10000),
    ) for monitor in monitors[:MAX_MONITORS_PER_REPORT] if isinstance(monitor, dict)]
    ReportMonitor.objects.bulk_create(rows)


def _rate_limited(key: str) -> bool:
    cache_key = "rate:" + key
    count = cache.get_or_set(cache_key, 0, timeout=3600)
    if count >= RATE_LIMIT_PER_HOUR:
        return True
    try:
        cache.incr(cache_key)
    except ValueError:
        cache.set(cache_key, 1, timeout=3600)
    return False


@csrf_exempt
@require_POST
def logs(request):
    """Telemetry ingestion endpoint used by Vishwakarma.exe (ImprovementData.cpp).

    The request is authenticated by the Ed25519 chain: installation key signs the session
    key, session key signs the body (X-MV-Signature header). Nothing in the payload is
    personally identifiable; the installation public key is the only identifier."""
    body = request.body
    if len(body) > MAX_BODY_BYTES:
        return JsonResponse({"status": "error", "reason": "too large"}, status=413)
    try:
        payload = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return JsonResponse({"status": "error", "reason": "invalid json"}, status=400)
    if not isinstance(payload, dict):
        return JsonResponse({"status": "error", "reason": "invalid json"}, status=400)

    installation_key = str(payload.get("installationPublicKey", ""))[:64]
    session_key = str(payload.get("sessionPublicKey", ""))[:64]
    session_signature = str(payload.get("sessionKeySignature", ""))[:96]
    body_signature = request.headers.get("X-MV-Signature", "")[:96]
    if not verify_request(installation_key, session_key, session_signature,
                          body_signature, body):
        logger.warning("Rejected /api/logs request with invalid signature chain.")
        return JsonResponse({"status": "error", "reason": "invalid signature"}, status=401)

    if _rate_limited(installation_key):
        return JsonResponse({"status": "error", "reason": "rate limited"}, status=429)

    app_version = payload.get("appVersion", 0)
    if not isinstance(app_version, int) or app_version < 0:
        app_version = 0

    with transaction.atomic():
        installation, _ = Installation.objects.get_or_create(public_key=installation_key)
        installation.app_version = app_version
        installation.save(update_fields=["app_version", "last_seen"])

        hardware_acked = False
        hardware = payload.get("hardware")
        if isinstance(hardware, dict):
            collected = _parse_iso_utc(payload.get("hardwareCollectedUtc")) \
                or django_timezone.now()
            gpus = hardware.get("gpus")
            primary_gpu = gpus[0] if isinstance(gpus, list) and gpus \
                and isinstance(gpus[0], dict) else {}
            report = HardwareReport.objects.create(
                installation=installation,
                collected_utc=collected,
                payload=hardware,
                cpu_name=str(hardware.get("cpuName", ""))[:128],
                cpu_physical_cores=int(hardware.get("cpuPhysicalCores", 0) or 0),
                ram_total_mb=int(hardware.get("ramTotalMB", 0) or 0),
                os_name=str(hardware.get("osName", ""))[:128],
                os_build=int(hardware.get("osBuild", 0) or 0),
                gpu_name=str(primary_gpu.get("name", ""))[:128],
            )
            _store_gpus(report, gpus)
            _store_monitors(report, hardware.get("monitors"))
            hardware_acked = True

        ack_ids = []
        usage = payload.get("usage")
        if isinstance(usage, list):
            for row in usage[:MAX_USAGE_ROWS_PER_REQUEST]:
                if not isinstance(row, dict):
                    continue
                client_row_id = row.get("id")
                start = _parse_iso_utc(row.get("start"))
                if not isinstance(client_row_id, int) or start is None:
                    continue
                actions = row.get("actions")
                if not isinstance(actions, dict):
                    actions = {}
                UsageRecord.objects.get_or_create(
                    installation=installation,
                    client_row_id=client_row_id,
                    defaults={
                        "interval_start_utc": start,
                        "interval_seconds": int(row.get("interval", 0) or 0),
                        "open_seconds": int(row.get("open", 0) or 0),
                        "focus_seconds": int(row.get("focus", 0) or 0),
                        "left_clicks": int(row.get("left", 0) or 0),
                        "middle_clicks": int(row.get("middle", 0) or 0),
                        "right_clicks": int(row.get("right", 0) or 0),
                        "key_presses": int(row.get("keys", 0) or 0),
                        "ribbon_actions": actions,
                    },
                )
                ack_ids.append(client_row_id)

    return JsonResponse({"status": "ok", "ackUsageIds": ack_ids,
                         "hardwareAcked": hardware_acked})


@csrf_exempt
@require_POST
def login(request):
    """Future AccountManager based login. Reserved now so that the URL, its CORS policy
    (https://mv.ramshanker.in only) and the signature scheme are fixed early."""
    return JsonResponse({"status": "not_implemented"}, status=501)


def _gb_label(megabytes):
    """Whole gigabytes, rounded up: the OS always reports slightly less than the installed
    amount because firmware and integrated graphics reserve a slice of it."""
    if not megabytes:
        return ""
    return "%d GB" % -(-int(megabytes) // 1024)


@require_GET
@cache_page(600)
def stats(request):
    """Public dashboard of global, fully aggregated usage statistics."""
    now = django_timezone.now()
    month_ago = now - timedelta(days=30)

    totals = UsageRecord.objects.aggregate(
        open_seconds=Sum("open_seconds"), focus_seconds=Sum("focus_seconds"),
        records=Count("id"))

    def top_counts(queryset, field, limit=8):
        rows = (queryset.exclude(**{field: ""}).values(field)
                .annotate(n=Count("installation", distinct=True)).order_by("-n")[:limit])
        items = [(r[field], r["n"]) for r in rows]
        peak = items[0][1] if items else 1
        return [{"label": label, "count": n, "percent": round(100 * n / peak)}
                for label, n in items]

    def top_labels(pairs, limit=8):
        """Same ranking as top_counts for values that have to be computed in python (unit
        conversion, bucketing, joining several columns into one label). pairs yields
        (label, installation_id); empty labels are dropped."""
        installations_per_label = {}
        for label, installation_id in pairs:
            if label:
                installations_per_label.setdefault(label, set()).add(installation_id)
        items = sorted(((label, len(ids)) for label, ids in installations_per_label.items()),
                       key=lambda item: (-item[1], item[0]))[:limit]
        peak = items[0][1] if items else 1
        return [{"label": label, "count": n, "percent": round(100 * n / peak)}
                for label, n in items]

    # Latest hardware report per installation would need window functions; for dashboard
    # purposes counting distinct installations per value is a good approximation.
    hardware = HardwareReport.objects.all()

    # One row per adapter / per display, so multi-GPU and multi-monitor machines contribute
    # to every bucket they actually occupy.
    gpu_rows = list(ReportGpu.objects.values_list(
        "name", "vendor_id", "device_id", "vram_mb", "discrete", "report__installation_id"))
    # The EDID panel size is stored per monitor but deliberately not aggregated: physical
    # millimetres vary by a millimetre or two between otherwise identical panels.
    monitor_rows = list(ReportMonitor.objects.values_list(
        "width_px", "height_px", "refresh_hz", "report__installation_id"))

    command_counter = Counter()
    for actions in (UsageRecord.objects.exclude(ribbon_actions={})
                    .values_list("ribbon_actions", flat=True)[:100000]):
        if isinstance(actions, dict):
            for command_id, count in actions.items():
                try:
                    command_counter[int(command_id)] += int(count)
                except (ValueError, TypeError):
                    continue
    top_commands = command_counter.most_common(15)
    peak_command = top_commands[0][1] if top_commands else 1
    commands = [{"label": COMMAND_NAMES.get(cid, str(cid)), "count": n,
                 "percent": round(100 * n / peak_command)} for cid, n in top_commands]

    context = {
        "generated_utc": now.strftime("%Y-%m-%d %H:%M UTC"),
        "installations": Installation.objects.count(),
        "active_30d": Installation.objects.filter(last_seen__gte=month_ago).count(),
        "open_hours": round((totals["open_seconds"] or 0) / 3600, 1),
        "focus_hours": round((totals["focus_seconds"] or 0) / 3600, 1),
        "records": totals["records"] or 0,
        "os_list": top_counts(hardware, "os_name"),
        "cpu_list": top_counts(hardware, "cpu_name"),
        "gpu_list": top_labels(
            (gpu_label(name, vendor, device), installation)
            for name, vendor, device, _vram, _discrete, installation in gpu_rows),
        # An integrated GPU's "dedicated" memory is an arbitrary UMA carve-out (495 MB on a
        # Ryzen 5825U), so only discrete cards get a size bucket.
        "vram_list": top_labels(
            (_gb_label(vram) if discrete else "Shared / integrated", installation)
            for _name, _vendor, _device, vram, discrete, installation in gpu_rows),
        "ram_list": top_labels(
            (_gb_label(mb), installation)
            for mb, installation in hardware.values_list("ram_total_mb", "installation_id")),
        "resolution_list": top_labels(
            ("%d x %d" % (w, h) if w and h else "", installation)
            for w, h, _hz, installation in monitor_rows),
        "refresh_list": top_labels(
            ("%d Hz" % hz if hz else "", installation)
            for _w, _h, hz, installation in monitor_rows),
        "display_mode_list": top_labels(
            ("%d x %d @ %d Hz" % (w, h, hz) if w and h and hz else "", installation)
            for w, h, hz, installation in monitor_rows),
        "commands": commands,
    }
    return render(request, "api/stats.html", context)


@require_GET
def health(request):
    return HttpResponse("ok", content_type="text/plain")
