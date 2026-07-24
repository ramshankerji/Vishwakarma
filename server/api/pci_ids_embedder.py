# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""Generate api/gpu_pci_ids.py from the upstream PCI ID Repository.

The driver-reported adapter description is generic on most integrated parts, so
/api/stats labels adapters by their PCI vendor:device pair instead. Upstream carries
every PCI device ever made (~1.6 MB); only the display-controller vendors are kept.

Upstream data is dual licensed GPL-2.0-or-later OR BSD-3-Clause. We take the BSD
option, which requires the notice below to travel with the generated file.

Run manually when the table should pick up newly released GPUs:
    python pci_ids_embedder.py            # download and regenerate
    python pci_ids_embedder.py pci.ids    # regenerate from a local copy
"""

import re
import sys
import urllib.request
from pathlib import Path

SOURCE_URL = "https://pci-ids.ucw.cz/v2.2/pci.ids"
OUTPUT_FILE = Path(__file__).with_name("gpu_pci_ids.py")

# Vendors that ship a display controller. Anything outside this set is dropped: it is
# what takes the table from every PCI device ever made down to a reviewable size.
GPU_VENDORS = {
    "1002": "AMD/ATI",
    "10de": "NVIDIA",
    "8086": "Intel",
    "1414": "Microsoft",   # WARP software adapter, Hyper-V synthetic video.
    "13b5": "ARM",         # Mali, for a future non-x86 port.
    "5143": "Qualcomm",    # Adreno, ditto.
    "1af4": "Red Hat",     # virtio-gpu, seen on Linux VMs.
    "15ad": "VMware",
    "1234": "QEMU",
}

# Upstream indents device lines with one tab and subsystem lines with two. Vendor lines
# sit at column zero; so do the "C 03  Display controller" class lines, which the
# 4-hex-digit match rejects.
VENDOR_LINE = re.compile(r"^([0-9a-f]{4})\s+(.+)$")
DEVICE_LINE = re.compile(r"^\t([0-9a-f]{4})\s+(.+)$")

LICENCE_NOTICE = '''"""GPU names from the PCI ID Repository - GENERATED FILE, DO NOT EDIT.

Regenerate with api/pci_ids_embedder.py. Curated overrides belong in api/gpu_names.py,
which consults this table only when it has no entry of its own.

Source: {url}
Snapshot: {version}

The PCI ID Repository data is distributed under the terms of either the GNU General
Public License (version 2 or later) or of the 3-clause BSD License. This file is
redistributed under the 3-clause BSD License:

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice, this
     list of conditions and the following disclaimer in the documentation and/or
     other materials provided with the distribution.
  3. Neither the name of the copyright holder nor the names of its contributors may
     be used to endorse or promote products derived from this software without
     specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
  SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
  DAMAGE.
"""'''


def parse(text):
    """{"vendor:device": name} for the GPU vendors, plus the snapshot date upstream states."""
    version = "unknown"
    names = {}
    vendor = None
    for line in text.splitlines():
        if line.startswith("#"):
            if "Date:" in line:
                version = line.split("Date:", 1)[1].strip()
            continue
        if not line.strip():
            continue
        device = DEVICE_LINE.match(line)
        if device and vendor:
            names["%s:%s" % (vendor, device.group(1))] = device.group(2).strip()
            continue
        if line.startswith("\t"):       # Subsystem line, or a device under a skipped vendor.
            continue
        found = VENDOR_LINE.match(line)
        vendor = found.group(1) if found and found.group(1) in GPU_VENDORS else None
    return names, version


def main():
    if len(sys.argv) > 1:
        text = Path(sys.argv[1]).read_text(encoding="utf-8")
    else:
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as response:
            text = response.read().decode("utf-8")

    names, version = parse(text)
    if len(names) < 1000:
        sys.exit("Only %d devices parsed - upstream format may have changed." % len(names))

    lines = [LICENCE_NOTICE.format(url=SOURCE_URL, version=version), "", "PCI_GPU_NAMES = {"]
    lines += ['    "%s": %s,' % (key, repr(names[key])) for key in sorted(names)]
    lines += ["}", ""]
    OUTPUT_FILE.write_text("\n".join(lines), encoding="utf-8")
    print("Wrote %s: %d devices, upstream snapshot %s" % (OUTPUT_FILE.name, len(names), version))


if __name__ == "__main__":
    main()
