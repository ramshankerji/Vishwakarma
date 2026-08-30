---
name: dev-machine-monitors
description: "Ram's dev machine is dual-monitor with the 4K panel ABOVE the primary, so negative window Y is normal — capture the virtual screen, never PrimaryScreen"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: eee53d25-1773-4a53-8b8a-efcbd54c8ab9
  modified: 2026-07-22T21:26:26.817Z
---

Ram's development machine has two monitors: `\\.\DISPLAY1` primary 1920x1080 at (0,0), and
`\\.\DISPLAY2` 3840x2160 at **(-959, -2160)** — physically *above* the primary. Virtual screen is
therefore origin (-959,-2160), size 3840x3240. He routinely drags Vishwakarma's extracted
tab/view windows up onto DISPLAY2.

**Why:** I screenshotted with `[System.Windows.Forms.Screen]::PrimaryScreen.Bounds`, saw two app
windows reported at y ≈ -890, and told him they were "off-screen at negative coordinates" and
possibly a spawn bug. They were simply on his second monitor, where he had moved them himself.
Negative X/Y is ordinary virtual-desktop geometry here, not evidence of anything.

**How to apply:** when verifying the app visually, capture
`[System.Windows.Forms.SystemInformation]::VirtualScreen` (its X/Y are the negative origin to pass
to `CopyFromScreen`), not `PrimaryScreen.Bounds` — otherwise a whole 4K monitor is invisible to me
and any window on it looks missing. Same when clicking: `SetCursorPos` takes virtual-desktop
coordinates, so a target on DISPLAY2 has negative Y. And never infer "off-screen", "hidden" or
"bug" from a negative window rect on this machine — enumerate monitors first, then say what is
actually true. Related: [[per-monitor-icon-atlas]], [[multi-window-subtabs]],
[[ribbon-command-recipe]] (whose screenshot recipe still says PrimaryScreen — read it with this
correction in mind), [[application-tab-plan]].
