---
title: "Light and Dark Themes"
weight: 100116
---
This page is the Design Document for **Light and Dark Themes**. To be referred by AI for coding
as well as humans. It is a plan; sections marked as decisions are locked, defaults are changeable
until first implementation.

The feature: one application-wide theme setting with exactly three values — **1: System
(default), 2: Light, 3: Dark** — living in the Application Tab's Settings view (tab 0, see
`website/content/software/tabs.md`). There are **no customizable themes**: no user palettes, no
accent pickers, no per-document themes. The only two extra knobs are background-color
**overrides** for the Scene3D sky and the Page2D drawing area. The theme changes the whole shell
— top ribbon, tab bands, panes, widgets — **and** the drawing-area backgrounds, with the classic
CAD black↔white entity remap so 2D content stays visible on a dark canvas. All colour codes and
per-theme palettes are defined in `code-core/colors.h`.

## 1. Current state (anchor points in code)

- `colors.h`: today holds only `kUIDisabledTextGray`, the Scene3D sky gradient pair
  (`kSceneSkyTop*` / `kSceneSkyHorizon*`) and the Page2D background (`kCad2DBackground*`). This
  file becomes the single home of all theme palettes.
- `struct UIColors` (`UserInterface.h` ~line 319): the ribbon palette (tab band, action groups,
  hover, disabled icon) with light-theme defaults. Its own comment already anticipates this
  document: *"The default values specified here are for theme Light. These can be overridden by
  other themes (e.g. Dark)"*. Globals `uiLightDefaultColors, uiActiveColors`
  (`UserInterface.cpp` ~line 122); every band/pane draw already reads `uiActiveColors.*`.
- Hardcoded colour literals bypassing `UIColors` are scattered through `UserInterface.cpp`
  (inventory in §5). One of them even carries the marker `// hover tint (TODO: theme-aware)`
  (`PushInteractiveRect`, ~line 729).
- Scene3D sky: `ClearSceneSkyGradient` (`RenderScene3D-DirectX12.cpp` ~line 105) pushes the two
  gradient stops as **root constants every frame** — runtime re-colouring needs no pipeline or
  resource change at all.
- Page2D background has four coupled consumers, all reading the same constexpr:
  1. Compositor per-frame RTT clear (`RenderCompositor-DirectX12.cpp` ~line 209).
  2. The **baked optimized clear value** at RTT creation, `शंकर::InitD3DPerWindow`
     (`RenderCompositor-DirectX12.cpp` ~line 576).
  3. The same baked value in the resize path, `शंकर::ResizeD3DWindow` (~line 734).
  4. Page2D's own clear (`RenderPage2D-DirectX12.cpp` ~line 651).
  The comment in `colors.h` records the constraint: clearing to any colour other than the baked
  value forfeits the fast clear and trips `CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE`. **A
  theme switch therefore has to recreate the RTT textures**, not just swap a constant.
- Page2D entity colours: every 2D creation path defaults to pure black `0xFF000000u`
  (`RenderPage2D.cpp` lines 129–350). Line/curve colours live in GPU records
  (`Cad2DLineGPURecord.colorABGR`) and are resolved **in the vertex shader**
  (`Shader2D_LineVertex.hlsl` line ~103 already remaps to the selection blue when the SELECTED
  flag is set). Text colours are baked per vertex (`Cad2DTextVertex.colorABGR`) but pass through
  `Shader2D_TextVertex.hlsl`. All three vertex shaders bind the same
  `cbuffer Cad2DViewConstants : register(b0)`, which has an unused `float padding0`
  (`RenderPage2D.h` ~line 276, `static_assert` 32 bytes). This is the perfect slot for a theme
  flag — no ABI growth, no re-tessellation.
- `PrinterController.cpp` (~line 132) fills the **same** `Cad2DViewConstants` for printing.
  Paper is always light; the print path must never see the dark remap.
- `RestartRenderThreads` (`RenderCompositor-DirectX12.cpp` ~line 1049): the proven
  join-threads → drain-fences → rebuild → relaunch pattern used for monitor topology changes and
  per-monitor icon atlas rebuilds. Its "rare event, brief pause is acceptable" reasoning applies
  verbatim to a theme switch.
- Icon atlases are theme-safe as-is: monochrome icons are tinted by vertex colour at draw time,
  multi-colour icons are drawn with a white tint (shader multiplies). **No atlas rebuild is
  needed for a theme change** (the ride through `RestartRenderThreads` rebuilds them anyway,
  which is harmless).
- Data tree text colour already flips by content (`useDarkDataTreeText`, `UserInterface.cpp`
  ~line 975) — dark text over the light Page2D/Scene3D canvases, white otherwise. The condition
  is keyed on container type, which becomes wrong the moment canvases can be dark (§6).
- Settings persistence: **none exists yet**. `%LOCALAPPDATA%\Mission Vishwakarma\` is the
  established per-installation data folder (`AccountManager.cpp` ~line 26,
  `ImprovementData.cpp` ~line 449). This feature introduces the first settings file.
- System theme detection: none exists. No `dwmapi` usage anywhere yet either, so OS title bars
  are always light today.
- Pre-existing dead constants `COLOR_UI_TAB_ACTIVE` / `COLOR_UI_TAB_INACTIVE`
  (`UserInterface.h` lines 184–185) are defined but never used — noted here, not touched.

## 2. Locked design decisions

1. **Exactly two compile-time palettes.** `colors.h` defines `kUIColorsLight` and
   `kUIColorsDark` as `constexpr UIColors` instances plus per-theme background constants. Users
   pick a mode; they never edit palette entries. The only user-supplied colours in the whole
   system are the two background overrides.
2. **Three-state mode, two-state result.** `enum class ThemeMode : uint8_t { System = 1,
   Light = 2, Dark = 3 };` (numbering per the requirement) is what is stored;
   `ResolveActiveTheme()` collapses it to Light or Dark. Everything downstream sees only the
   resolved two-state value — no code outside the resolver ever asks "is mode System?".
3. **`colors.h` is the single home of colour definitions.** `struct UIColors` moves from
   `UserInterface.h` into `colors.h` (which `UserInterface.h` then includes), joined by both
   palette instances, the per-theme sky/page background constants, the shared
   theme-independent colours (§4), and the luminance helper. Every hardcoded UI colour literal
   in §5 migrates into a `UIColors` field or a named shared constant. After Phase 1, a colour
   literal outside `colors.h` is a code-review defect.
4. **Theme is global.** One theme for the whole process — every window, every monitor, every
   tab. No per-window or per-document theming.
5. **Palette globals mutate only while render threads are joined.** `uiActiveColors` and the new
   `g_activeBackgrounds` (§6) are written exclusively inside the `RestartRenderThreads` drain
   window, on the UI thread. Render threads keep reading them lock-free with zero risk of a
   torn frame. This is the single most important correctness rule of this feature.
6. **A theme switch rides `RestartRenderThreads`.** Because the Page2D background is a baked
   RTT clear value, applying a theme means recreating every window's RTT textures. Rather than
   inventing a second drain path, `ApplyThemeChange()` records the pending palette and calls
   `RestartRenderThreads()`, which gains one step: after the join+drain, if a theme apply is
   pending, swap the palette globals and recreate each published window's RTTs (same descriptor
   slots, new baked clear value). Theme switches are rare user events; the brief pause is the
   same one users already accept for monitor changes. The fast clear stays intact and the debug
   layer stays silent.
7. **Page2D entity visibility is solved in the shader, not the data.** Stored entity colours
   are never rewritten. The three 2D vertex shaders remap **pure black ↔ pure white**
   (`0xFF000000` ↔ `0xFFFFFFFF`, both directions) when the view constants say the background is
   dark. Any other colour passes through untouched. This is the AutoCAD-familiar behaviour for
   "foreground" entities, costs one compare per primitive, needs no re-tessellation, and takes
   effect the same frame. The selection highlight `0xFFA6260D` is not remapped.
8. **The remap and all "over the canvas" colours key on effective background luminance, not on
   theme name.** Overrides can make a Light-theme canvas dark (or vice versa); black entities,
   the data-tree text and the canvas cursor crosshair must follow the actual background. Rule:
   `IsDarkBackground(r,g,b)` ⇔ Rec.709 luma `0.2126R + 0.7152G + 0.0722B < 0.5` (normalized),
   as a constexpr helper in `colors.h`, evaluated once per apply (not per frame).
9. **Overrides are plain RGB values, one per view type, theme-independent.** `scene3d.background`
   and `page2d.background` each hold either "absent = use active theme default" or one
   `#RRGGBB`. A Scene3D override replaces **both** gradient stops (flat sky — simplest thing
   that works; auto-derived gradient is a changeable default, §11). No per-theme override pairs
   — that is customization creep this feature explicitly rejects.
10. **Printing is always light.** `PrinterController` sets the new view-constants theme field to
    0 (light) unconditionally and never applies the remap or dark background. Paper wins.
11. **The splash screen is brand, not theme.** `kSplashBackgroundColor` / `kSplashTextColor`
    and the tricolour logo stay identical in both themes.
12. **Settings live in a new plain-text file** `%LOCALAPPDATA%\Mission Vishwakarma\Settings.txt`
    (§8) — the file that tabs.md's Launcher open question ("likely next to the AccountManager
    identity data") anticipated. Read once at startup on the UI thread **before render threads
    launch**; rewritten in full on every change. The UI thread is the only reader and writer.

## 3. What is themed, what is not

| Surface | Light → Dark behaviour |
|---------|------------------------|
| Tab band, sub-tab band, ribbon action groups | Full palette swap (`uiActiveColors`) |
| Properties pane, right icon bar, data tree panel, dropdowns, scrollbars, toasts, text fields | Palette swap via migrated fields (§5) |
| Scene3D sky gradient | Per-theme gradient pair, or flat override colour |
| Page2D drawing background | Per-theme colour, or override; RTT baked clear follows |
| Page2D entities | Pure black ↔ pure white shader remap on dark canvas; all other colours untouched |
| Canvas cursor crosshair + caret glyphs drawn over canvas | Black/white by canvas luminance |
| OS window title bar | `DWMWA_USE_IMMERSIVE_DARK_MODE` per window (until the frameless window of tabs.md removes it) |
| Splash screen | **Not themed** (brand) |
| Icon colours inside buttons | Unchanged (per-icon colours / dev placeholder `StableRandomUIColour`) |
| Printed output | **Always light** |
| Website / documentation | Out of scope (Hugo has its own theme) |

## 4. `colors.h` layout after Phase 1

```cpp
enum class ThemeMode : uint8_t { System = 1, Light = 2, Dark = 3 }; // Stored setting.
enum class ActiveTheme : uint8_t { Light, Dark };                   // Resolved, two-state.

struct UIColors { /* moved from UserInterface.h, grown by the §5 fields */ };
constexpr UIColors kUIColorsLight { /* today's values */ };
constexpr UIColors kUIColorsDark  { /* table below */ };

// Scene3D sky, per theme (existing kSceneSky* become the Light set):
constexpr float kSceneSkyLightTop[3], kSceneSkyLightHorizon[3];
constexpr float kSceneSkyDarkTop[3], kSceneSkyDarkHorizon[3];
// Page2D background, per theme (existing kCad2DBackground* become the Light value):
constexpr float kCad2DBackgroundLight[3];
constexpr float kCad2DBackgroundDark[3];

// Theme-independent (shared) colours:
//   kUIDisabledTextGray (existing), selection blue 0xFF3399FF, hover saffron 0xFFFF9933,
//   Page2D selection highlight 0xFFA6260D, splash colours.

constexpr bool IsDarkBackground(float r, float g, float b); // Decision 8.
```

Proposed dark palette — **changeable defaults until first implementation**. Values keep the
green family of the light theme and the saffron brand hover. Reminder from the splash-colour
comment (`UserInterface.cpp` ~line 913): these are **ABGR** (`0xAABBGGRR`) — get the channel
order right or greens come out purple.

| `UIColors` field | Light (existing) | Dark proposed | Dark ABGR |
|------------------|------------------|---------------|-----------|
| `tabBackground` | rgb(221,229,212) | rgb(25,29,23) `#191D17` | `0xFF171D19` |
| `tabBackgroundText` | rgb(84,98,78) | rgb(156,178,164) | `0xFFA4B29C` |
| `tabActive` | rgb(248,251,241) | rgb(36,41,38) | `0xFF262924` |
| `tabActiveText` | rgb(23,29,25) | rgb(233,237,235) | `0xFFEBEDE9` |
| `tabBackgroundHover` | rgb(196,213,205)* | rgb(45,52,47) | `0xFF2F342D` |
| `actionGroupBackground` | rgb(255,255,255) | rgb(30,30,32) | `0xFF201E1E` |
| `actionGroupSeperator` | rgb(209,209,209) | rgb(72,72,76) | `0xFF4C4848` |
| `actionGroupHoverBackground` | saffron `0xFFFF9933` | **same** (brand) | `0xFFFF9933` |
| `actionText` | rgb(36,36,36) | rgb(228,228,228) | `0xFFE4E4E4` |
| `actionIconDisabled` | rgb(136,136,136) | rgb(105,105,105) | `0xFF696969` |

\* light `tabBackgroundHover` shown decoded from `0xFFC4D5CD`.

Backgrounds — changeable defaults:

- **Scene3D dark sky**: top `#0D1117` (13,17,23), horizon `#202731` (32,39,49) — a deep
  night-blue gradient, dark enough that white/light geometry pops, not pure black.
- **Page2D dark background**: `#212830` (33,40,48) — the near-black blue-gray CAD users know
  from AutoCAD's model space, deliberately not `#000000` so pure-black→white remapped entities
  and the true-black selection rectangle remain distinguishable.

## 5. Hardcoded-literal migration inventory (Phase 1)

Every literal below moves into a (new) `UIColors` field with light+dark values, or is declared a
named shared constant in `colors.h`. Line numbers are approximate anchors in `UserInterface.cpp`
unless noted.

| Anchor | Literal today | Disposition |
|--------|---------------|-------------|
| `PushInteractiveRect` ~729–731 | hover `0xFF555555`, pressed `0xFF333333`, disabled `0xFF1E1E1E` | fields `interactiveHover/Pressed/DisabledBase` (the existing `TODO: theme-aware`) |
| Widget field ~886–887, ~1933 | field bg `0xFFF7F7F7`, text `0xFF000000` | fields `widgetFieldBackground/Text` |
| Dropdown ~899–904 | border `0xFFCCCCCC`, row hover `0xFFC4D5CD`, selected `0xFFE8F2ED`, row `0xFFFFFFFF`, label `0xFF666666` | fields `widgetBorder`, `dropdownRow*`, `widgetLabelText` |
| Data tree ~979–981 | text black/white flip, active `0xFF3399FF`, hover `0xFFFF9933` | text: luminance rule (§6); active/hover: shared constants |
| Sub-tab band ~1360–1367, ~1478 | `0xFF1E1E1E`, `0xFF2D2D30`, `0xFF444444` | fields for the sub-tab close/hover chrome |
| Scrollbar ~1619–1622 | track `0x66333333`, thumb `0xFF3399FF` / `0xFF5CB4FF` / `0xCC8A8A8A` | fields `scrollbar*` |
| Toast ~2189–2191 | bg `0xFF333333`, text `0xFFFFFFFF` | fields `toast*` |
| Right pane dropdown panel ~1996 | `0xFF1E1E1E` | field (shared with sub-tab chrome) |
| Canvas cursor icons ~2041, 2155, 2173 | `0xFF000000` | **luminance rule** — black cursor on light canvas, white on dark (§6) |
| Text caret ~1952, 2108 | `0xFF000000` | follows `widgetFieldText` |
| Tab separators / accents ~1252, 1293 | `0xFF3399FF`, `0xFF555555` | accent: shared constant; separator: field |
| `RenderPage2D.cpp` entity defaults | `0xFF000000` | **unchanged** — stored data stays black; §7 handles display |

Behaviour-preserving check for the whole phase: with theme forced Light, the UI must be
pixel-identical to today.

## 6. Background pipeline and the apply sequence

New global (written under Decision 5's joined-threads rule only):

```cpp
struct ActiveBackgroundColors {
    float scene3DTop[3]; float scene3DHorizon[3]; // theme default or flat override
    float page2D[3];                              // theme default or override
    bool  scene3DIsDark; bool page2DIsDark;       // IsDarkBackground(), precomputed
};
inline ActiveBackgroundColors g_activeBackgrounds; // starts as Light defaults
```

Call-site conversion (all four Page2D-background consumers of §1 plus the sky constants read
`g_activeBackgrounds` instead of the constexprs):

- `ClearSceneSkyGradient`: root constants from `g_activeBackgrounds.scene3D*`.
- Compositor clear ~209, Page2D clear ~651: `g_activeBackgrounds.page2D`.
- `InitD3DPerWindow` ~576 and `ResizeD3DWindow` ~734 baked clear value:
  `g_activeBackgrounds.page2D`. Both always run while the affected window has no in-flight
  frames, so baked value and per-frame clear can never disagree.
- Data-tree text over canvas (~975): replace the container-type condition with the
  luminance flag of whatever the active sub-tab shows (`page2DIsDark` / `scene3DIsDark`);
  canvas cursor icons and canvas carets use the same flag.

`ApplyThemeChange()` (UI thread; no-op if the resolved palette + backgrounds are unchanged):

1. Resolve `ThemeMode` → `ActiveTheme` (System consults `QuerySystemPrefersDark()`, §8).
2. Compose the pending `UIColors` + `ActiveBackgroundColors` (overrides applied, luminance
   flags computed). Store into a pending slot; set `g_themeApplyPending = true`.
3. Call `RestartRenderThreads()`. Inside, after the existing join + per-monitor fence drain:
   if `g_themeApplyPending`, copy pending → `uiActiveColors` / `g_activeBackgrounds`, then for
   every published window release and recreate its `renderTextures[FRAMES_PER_RENDERTARGETS]`
   with the new baked clear value, re-writing the same RTV/SRV descriptor slots (factor the
   RTT block of `ResizeD3DWindow` ~731 into a small `RecreateWindowRTTs(dx)` shared by both
   callers). Fences are already drained, so immediate release is safe — the same justification
   the icon-atlas drain uses.
4. Set `DWMWA_USE_IMMERSIVE_DARK_MODE` on every top-level window (§8).
5. Threads relaunch as usual; the first new frame is fully in the new theme.

## 7. Page2D entity remap (shader ABI change)

`Cad2DViewConstants.padding0` is renamed to `float backgroundIsDark` (0.0 or 1.0) in
`RenderPage2D.h` and all three vertex-shader cbuffers — size stays 32 bytes, the
`static_assert` is the tripwire. `RenderPage2D-DirectX12.cpp` (~670) fills it from
`g_activeBackgrounds.page2DIsDark`; `PrinterController.cpp` leaves it 0 with a comment citing
Decision 10.

Remap applied identically in `Shader2D_LineVertex.hlsl`, `Shader2D_CurveVertex.hlsl` (on the
record colour, before the selection override) and `Shader2D_TextVertex.hlsl` (on the baked
vertex colour):

```hlsl
uint c = rec.colorABGR;
if (backgroundIsDark != 0.0) {
    if (c == 0xFF000000u) c = 0xFFFFFFFFu;      // black -> white on dark canvas
    else if (c == 0xFFFFFFFFu) c = 0xFF000000u; // white -> black (symmetric)
}
```

DXF round-trips benefit for free: imported colour-7 ("black/white") entities behave exactly as
they did in the source CAD package. Snapping markers, grid/axis helpers and the selection
rectangle are audited in Phase 2 — anything drawn through the same record path inherits the
remap; anything hardcoded joins the §5 inventory.

## 8. System theme detection and OS chrome (Windows)

- `QuerySystemPrefersDark()`: read `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\
  Personalize` value `AppsUseLightTheme` (`RegGetValueW`, DWORD; 0 ⇒ dark, missing ⇒ light).
- Live change: `WndProc` handles `WM_SETTINGCHANGE` where `lParam` points to the string
  `"ImmersiveColorSet"`; if the stored mode is System and the resolved theme actually changed,
  call `ApplyThemeChange()`. Guard with `gpu.isGPUEngineInitialized` exactly like the adjacent
  `WM_DISPLAYCHANGE` handler (`Main.cpp` ~line 1390) — same deadlock reasoning.
- Title bars: `DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE /*20*/, &darkBOOL,
  sizeof(BOOL))` at every window creation and inside `ApplyThemeChange`. Adds a `dwmapi.lib`
  link (first use in the project). This is interim polish only — the Chrome-style frameless
  window (tabs.md future phase) deletes the OS title bar entirely.
- Linux/Mac: `QuerySystemPrefersDark()` is the single platform seam; other platforms supply
  their own implementation later. Nothing else in this design is platform-specific.

## 9. Settings persistence

First settings file of the application: `%LOCALAPPDATA%\Mission Vishwakarma\Settings.txt`,
UTF-8, one `key = value` per line, no comments, unknown keys preserved on rewrite (the file is
read into an ordered key→value map, mutated, written back whole — future features like the
Launcher's recent files add keys without a format change).

```
theme = system            (system | light | dark; missing/unparseable => system)
scene3d.background = #0D1117   (line absent => theme default)
page2d.background = #212830    (line absent => theme default)
```

New portable file pair `Settings.h` / `Settings.cpp` hosts the map load/save, the typed
accessors for these three keys, `QuerySystemPrefersDark()` (the one `#ifdef _WIN32` corner) and
`ResolveActiveTheme()`. Loaded in `wWinMain` before render threads launch, so the very first
frame is already themed — no light flash on startup.

## 10. Settings view UI (Application Tab)

Lives in tab 0's Settings view and follows tabs.md's rendering model (immediate-mode overlay,
UI-thread state ownership). Until tabs.md Phase 3 ships, Phases 1–3 of this document are
exercised by editing `Settings.txt` and restarting — the UI is deliberately the last phase.

- **Theme row**: label + `BuildUIDropdown` (the existing reusable dropdown) with the three
  items System / Light / Dark.
- **Two override rows**: label + text field (`UITextEditState` machinery, accepts `#RRGGBB`) +
  a small colour swatch rect + a "Default" button that clears the override. Invalid input is
  rejected on commit (field border flips to the existing error red, value untouched).
- Commit path: control emits `kThemeSettingUIAction = 0xE0000040u` (p1 = which setting,
  p2 = packed value) → `ProcessPendingUIActions` on the UI thread updates the settings map,
  rewrites `Settings.txt`, calls `ApplyThemeChange()`. Tab 0 actions are UI-thread-handled by
  design (tabs.md Decision 2), so this needs no engineering-thread involvement.

## 11. Phases

### Phase 1 — Palette consolidation (zero visible change)
`UIColors` moves to `colors.h`; both palettes defined; §5 literal inventory migrated; shared
constants named; `ThemeMode`/`ActiveTheme`/`IsDarkBackground` added. App still hardwired Light.
**Verify:** warning-free build; screenshot comparison of ribbon, properties pane, data tree,
dropdown, toast and splash is pixel-identical to before; `grep '0xFF[0-9A-Fa-f]\{6\}'` over
`UserInterface.cpp` finds no colour literals left (excluding the splash block).

### Phase 2 — Dark rendering end to end (forced via settings file)
`Settings.txt` load; `g_activeBackgrounds` + call-site conversion; RTT rebuild step inside
`RestartRenderThreads`; `Cad2DViewConstants.backgroundIsDark` + three shader remaps; printer
pinned light; DWM title bar; overrides honoured.
**Verify:** `theme = dark` renders dark ribbon, dark sky, dark Page2D with black entities shown
white and selection highlight still legible; debug layer stays silent (fast clear intact — no
`CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE`); print output unchanged vs light theme;
`page2d.background = #FFFFFF` under dark theme flips entities back to black (luminance rule,
not theme rule); window resize in dark theme keeps the dark baked clear; cursor crosshair
visible on both canvas polarities.

### Phase 3 — System mode + live switching
`QuerySystemPrefersDark()`, `WM_SETTINGCHANGE` handler, `ApplyThemeChange` wired.
**Verify:** with `theme = system`, toggling Windows Settings light↔dark restyles the running app
(all windows, all monitors) after one brief render pause; toggling with mode Light/Dark does
nothing; rapid toggling doesn't leak (GPU memory flat across 20 switches — the §6 rebuild
releases old RTTs); no torn half-themed frame is ever visible.

### Phase 4 — Settings view UI (depends on tabs.md Phase 3)
Dropdown + override fields + persistence round-trip as §10.
**Verify:** every control round-trips through `Settings.txt` and survives restart; live apply on
commit; invalid hex rejected; "Default" removes the line from the file.

## 12. File touch list

| File | Change |
|------|--------|
| `code-core/colors.h` | **Major.** §4 layout: enums, both palettes, per-theme backgrounds, shared constants, luminance helper. |
| `code-core/Settings.h` / `Settings.cpp` | **New.** Settings file map, typed accessors, `QuerySystemPrefersDark`, `ResolveActiveTheme`. |
| `code-core/UserInterface.h` | `UIColors` moves out; includes `colors.h`. |
| `code-core/UserInterface.cpp` | §5 literal migration; luminance-keyed data-tree text + canvas cursor/caret colours. |
| `code-core/RenderCompositor-DirectX12.cpp` | Clear + baked-clear call sites → `g_activeBackgrounds`; `RecreateWindowRTTs` factored; theme-apply step in `RestartRenderThreads`. |
| `code-core/RenderScene3D-DirectX12.cpp` | Sky root constants from `g_activeBackgrounds`. |
| `code-core/RenderPage2D-DirectX12.cpp` | Clear colour + `backgroundIsDark` fill. |
| `code-core/RenderPage2D.h` | `padding0` → `backgroundIsDark`. |
| `code-core/Shader2D_LineVertex.hlsl`, `Shader2D_CurveVertex.hlsl`, `Shader2D_TextVertex.hlsl` | cbuffer field rename + §7 remap. |
| `code-core/PrinterController.cpp` | Explicit `backgroundIsDark = 0` + Decision-10 comment. |
| `code-core/Main.cpp` | Settings load in `wWinMain`; `WM_SETTINGCHANGE`; DWM attribute at window creation; `dwmapi.lib`. |
| `code-core/ApplicationTab.cpp` (once it exists) | §10 Settings view rows + `kThemeSettingUIAction` handling. |
| `code-core/Vishwakarma.vcxproj` | Add `Settings.h/.cpp`. |

## 13. Open questions and changeable defaults

- **Exact dark palette values** (§4 tables) — tune on a real monitor during Phase 1; the field
  list is the contract, the numbers are not.
- **Page2D dark background**: `#212830` (AutoCAD-familiar) vs neutral `#1E1E1E` — pick once
  during Phase 2.
- **Scene3D override = flat sky**: acceptable, or derive a subtle auto-gradient (e.g. horizon
  = override lightened 8%)? Flat is the MVP default.
- **Remap breadth**: exact `0xFF000000`/`0xFFFFFFFF` swap only (current decision) vs a
  near-black/near-white luminance band. Exact-match is deterministic and explainable; widen
  only if imported DXF files show e.g. `#0A0A0A` entities vanishing in practice.
- **Saffron hover on dark**: `actionGroupHoverBackground` stays brand-saffron in both themes;
  if it reads too loud on dark, a muted saffron becomes a dark-palette-only tweak.
- **Telemetry**: record theme mode in `ImprovementData` (one enum per session) to learn actual
  dark-mode adoption — cheap, but only if wanted.
- **`Settings.txt` name**: `Settings.txt` vs `Settings.ini` — content format is identical
  either way; decide at first implementation.
