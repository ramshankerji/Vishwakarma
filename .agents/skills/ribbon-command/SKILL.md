---
name: ribbon-command
description: >-
  Use this skill when adding, modifying, or testing top-ribbon buttons, UI commands, or modal cursor modes in Vishwakarma. Covers the 8-file pipeline, translation compiler, SVG icon manifest, dispatch handling, and visual verification.
---

# Ribbon Command & UI Control Workflow

This runbook details the end-to-end process for adding and verifying a new top-ribbon command or UI button in Vishwakarma.

---

## 1. The 8-File Implementation Pipeline

Follow these steps in sequence when adding a new ribbon command:

1. **Define the Command ID (`code-core/ListOfCommands.h`)**:
   - Add an entry to the `Commands` enum with a unique 10-digit ID in the range `1000000000..4294967295`.

2. **Add Translation String (`code-miscellaneous/UserInterfaceTranslation.csv`)**:
   - Add a row: `<cmdID>,<SHORT_NAME>,<English>,<comment>`.
   - Recompile translation headers by running:
     ```bat
     python code-miscellaneous/UserInterfaceTranslationCompiler.py
     ```
   - *Note:* Commit the generated `UserInterfaceTranslationCompiled.{h,cpp}` files.

3. **Register Control in UI Layout (`code-core/UserInterface.h`)**:
   - Add a struct row to `AllUIControls[]`:
     - Fields: `{ action, nameStringID, UIIconForCommand(cmd), type(1=button), noOfVerticalSlots, verticalSlotNo, zIndex, showText, isEnabled, actionGroupIndex, actionSubGroupIndex }`.
     - **Set `isEnabled` to `true`**: Since 2026-07-19, `isEnabled` denotes whether the command has an active dispatcher. Unwired commands stay `false` and render dimmed (`kUIDisabledTextGray`).

4. **Author & Register SVG Icon**:
   - Create `website/static/SVGIcons/icon_<cmdID>_<slug>.svg` (`viewBox="0 0 64 64"`, stroke `#333` width 4 style).
   - Add entry in `code-core/SVGIconManifest.h` in **numerical sorted order**.
   - Pre-build tool `svg_icon_embedder.py` automatically embeds the icon into the atlas.

5. **Dispatch Action (`Main.cpp`)**:
   - In `ProcessPendingUIActions()`, map the `Commands` enum to `PushSystemTodoToTab(GetActiveTabForUIAction(), ACTION_TYPE::...)`.

6. **Define Action Type (`code-core/UserInputProcessing.h`)**:
   - Add new `ACTION_TYPE` in the `30001+` range.

7. **Implement Handler (`code-core/विश्वकर्मा.cpp`)**:
   - Handle the `ACTION_TYPE` in the main todo/queue processing loop (~line 1470).

---

## 2. Modal Cursor Modes (If Applicable)

When adding interactive tool modes (e.g. `ZOOM_WINDOW`, placement tools):
- Store an atomic mode flag on `DATASETTAB`.
- **Render Thread**: Reads flag to trail icon next to cursor in `UserInterface-DirectX12.cpp` gated on `activeInternalSubTabType`.
- **Engineering Thread**: Handles mouse/keyboard input via `Handle*Input()` at the top of the `userInputQueue` loop.
- **Escape / Mutual Exclusion**: `Esc` cancels or re-clicking the ribbon button toggles off; `Begin` resets other active modes.

---

## 3. Visual Verification Recipe

To verify UI buttons or modal states via script:
- Launch `build\Debug\Vishwakarma.exe` via PowerShell `Start-Process`.
- Allow ~14 seconds for DirectX12 initialization.
- Click ribbon coordinates using `user32` `SetCursorPos` + `mouse_event`.
- Capture output with `System.Drawing.Graphics.CopyFromScreen` and save PNG.

### 3D / Viewport Driving Gotchas
- **Window Focus**: `Start-Process` does not focus the window. A physical click inside the window is needed before `SendKeys` works.
- **Turn off Random & Orbit**: Disable **Auto Random** (ribbon) and camera auto-orbit (`r`) before taking screenshots to prevent diff noise.
- **Console Capture**: The application uses `AllocConsole()` and `CONOUT$`. Standard output redirection does not capture console logs. Minimize main window and screenshot `ConsoleWindowClass` if needed.
