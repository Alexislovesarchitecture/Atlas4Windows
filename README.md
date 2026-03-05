# Atlas Windows v0 (One-Shot Internal MVP)

This repository contains a scaffold for an OWL-style Client/Host split browser shell on Windows using
`HWND` embedding for the first MVP.

## What is implemented
- `atlas-client` Win32 shell with:
  - Tabs (create/switch/close)
  - Omnibox + Go button
  - Basic top-level navigation commands
  - HWND-hosted embedded view area
- `atlas-host` host process with:
  - Mojo-inspired command surface mapped to named-pipe handlers
  - Session/tab/view lifecycle
  - Child window-backed embedded view handles returned to client
- Shared protocol/types in `src/common`
- IPC transport over a named pipe (`\\.\pipe\AtlasOwlHost`)
- Download event pass-through:
  - `SetDownloadDirectory` carries a download destination + callback pipe name
  - host emits `DOWNLOAD` events and client exposes them in the status line

## What is intentionally not production-complete
- Real Chromium/WebView embedding is not yet implemented in this scaffold.
- Download events are simulated based on navigated URLs for v0.
- Input routing calls are currently acknowledged but not forwarded as a full automation layer.
- Host reconnect/rebind is now implemented for client survival on host restarts.

## Chromium OWL browser bring-up (reference + patch queue)

This repo includes a reproducible Chromium workflow:
- pin Chromium to a known upstream `refs/heads/main` commit,
- sync checkout state,
- apply Atlas-managed Chromium patch queue,
- build/run/test OWL + Chromium browser targets.

1) Bootstrap Chromium workspace (first time only):
```powershell
cd C:\Users\alexi\Documents\GitHub\Atlas4Windows
.\scripts\chromium\setup_owl_workspace.ps1 -WorkspaceRoot "C:\Users\alexi\Documents\GitHub" -ChromiumDir "chromium"
```

2) Optional: refresh pinned Chromium reference to latest `main`:
```powershell
.\scripts\chromium\pin_reference.ps1
```

3) Sync checkout to pinned commit and apply patch queue:
```powershell
.\scripts\chromium\sync_apply_reference.ps1 -ChromiumRoot "C:\Users\alexi\Documents\GitHub\chromium"
```

4) Check toolchain prerequisites:
```powershell
.\scripts\chromium\preflight.ps1 -RequireAtl -RequireArm64Runtime
```

5) Build OWL + Chromium browser in one flow:
```powershell
.\scripts\chromium\build.ps1 -ChromiumRoot "C:\Users\alexi\Documents\GitHub\chromium" -IncludeChrome -SkipArm64IfMissingPrereqs
```

6) Run OWL orchestration (optionally launches Chromium browser):
```powershell
.\scripts\chromium\run_owl.ps1 -ChromiumRoot "C:\Users\alexi\Documents\GitHub\chromium" -Arch x64 -LaunchChrome -RemoteDebuggingPort 9222
```

7) Smoke tests:
```powershell
.\scripts\chromium\test_owl.ps1 -ChromiumRoot "C:\Users\alexi\Documents\GitHub\chromium" -Arch x64 -LaunchChrome
.\scripts\chromium\test_chrome_playwright.ps1 -ChromiumRoot "C:\Users\alexi\Documents\GitHub\chromium"
```

8) Single-pass orchestrator:
```powershell
.\scripts\chromium\bringup.ps1 -ChromiumRoot "C:\Users\alexi\Documents\GitHub\chromium"
```

Reference/patch queue files:
- `scripts/chromium/reference/chromium-reference.json`
- `scripts/chromium/reference/patch-manifest.json`
- `scripts/chromium/patches/*.patch`

Mojom templates:
- `scripts/chromium/templates/owl/public/mojom/owl_host.mojom`
- `scripts/chromium/templates/owl/public/mojom/agent_gate.mojom`

## Build (Windows)
Prerequisites:
- `cmake`
- `cl` (`MSVC` compiler from Visual Studio Build Tools / Visual Studio)
- `msbuild`

You can run the build with:

1. Open a Visual Studio Developer Command Prompt (or an initialized VS environment shell).
2. Run `scripts\build_windows.bat` (defaults to `Release`), or run the two-step manual equivalent:
3. `cmake -S . -B build`
4. `cmake --build build --config Debug`
5. Start client: `build\Debug\atlas-client.exe`

The client will spawn `atlas-host.exe` automatically if it is in the same directory.

## Next steps for true browsing parity
1. Replace placeholder host child surface with Chromium engine host surface creation.
2. Replace simulated download triggers with Chromium `DownloadManager` event integration.
3. Layer in IME/hotkey/accessibility and DPI test hardening.

## Run + local launch package (Windows)

Build:
```bat
scripts\build_windows.bat
```

Launch client directly:
```bat
scripts\run_windows.bat
```

One-shot Parallels launch (cd + build + run):
```bat
scripts\run_in_parallels.bat
```

Create a zip package from built binaries:
```bat
scripts\package_windows.bat
```

To include a Release build in scripts, pass config name as the first arg (for example, `scripts\run_windows.bat Release`).

### Windows PowerShell quick commands

From PowerShell, always prefix scripts in the current folder with `.`:

```powershell
Set-Location "Y:\Desktop\CodexWorkspace\Atlas4Windows"
.\run_in_parallels.bat "Y:\Desktop\CodexWorkspace\Atlas4Windows" x64 "Y:\Desktop\CodexWorkspace\chromium" Release
```

If the Chromium tree is not present yet, bootstrap it through the one-shot helper with execution-policy bypass:

```powershell
Set-Location "Y:\Desktop\CodexWorkspace\Atlas4Windows"
powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\chromium\run_in_parallels_owl.ps1" -WorkspaceRoot "Y:\Desktop\CodexWorkspace" -ChromiumDir "chromium" -Arch x64 -Config Release
```

Smoke-test status surfacing:
- Launch with `scripts\run_windows.bat`.
- Type a URL that looks like a download target (for example, `https://example.com/file.zip`).
- Confirm the bottom status row updates through `Download ... STARTED/IN_PROGRESS/COMPLETE`.
