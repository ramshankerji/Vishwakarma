# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

"""Display names for graphics adapters whose driver-reported description is generic.

DXGI hands the client whatever string the driver chooses to expose. Intel and AMD integrated
parts share one description across many different chips - "AMD Radeon(TM) Graphics" is the
literal product name of every RDNA2 APU - so the PCI vendor:device pair collected alongside
it is the only thing that separates them.

Names are resolved in three steps, first hit wins:

1. GPU_MODELS below - hand written, for ids the upstream table only knows by codename.
2. gpu_pci_ids.PCI_GPU_NAMES - the PCI ID Repository snapshot, ~8700 adapters. Discrete
   cards come out well ("GA106 [RTX A2000]"); integrated parts are often a bare codename
   ("Barcelo"), which is what step 1 exists to improve on.
3. The driver's own description with the raw id appended, so an adapter missing from both
   tables still lands in its own bucket rather than merging with a different chip.
"""

import logging

try:
    from .gpu_pci_ids import PCI_GPU_NAMES
except ImportError:
    # Generated on the host, not tracked in git. Its absence costs nicer labels on one
    # dashboard panel, so degrade instead of taking telemetry ingestion down with it.
    logging.getLogger("api").warning(
        "api/gpu_pci_ids.py is missing, GPU names fall back to raw PCI ids. "
        "Generate it with: python api/pci_ids_embedder.py")
    PCI_GPU_NAMES = {}

# "vendor:device" in lowercase hex -> name to show instead of the upstream one. Keep this
# to ids where upstream is genuinely unhelpful; anything it already names well belongs
# nowhere near here, or it silently goes stale as upstream improves.
GPU_MODELS = {
    "1002:15e7": "AMD Radeon Vega (Barcelo APU)",  # Upstream: bare "Barcelo".
}


def gpu_label(name, vendor_id, device_id):
    """Display name for one adapter, per the three steps in the module docstring."""
    if not vendor_id and not device_id:
        return name  # Report collected before PCI ids were sent.
    key = "%04x:%04x" % (vendor_id, device_id)
    if key in GPU_MODELS:
        return GPU_MODELS[key]
    if key in PCI_GPU_NAMES:
        return PCI_GPU_NAMES[key]
    return "%s [%s]" % (name or "Unknown", key)
