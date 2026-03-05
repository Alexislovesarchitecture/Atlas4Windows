# One-Shot Contract Mapping

## Public contract implemented in code
- `CreateSession`
- `CreateTab`
- `Navigate`
- `GoBack`
- `GoForward`
- `Reload`
- `CloseTab`
- `CreateEmbeddedView`
- `AttachToParentHwnd`
- `ResizeView`
- `SetViewVisibility`
- `DestroyView`
- `RouteMouse` *(automation hook)*
- `RouteWheel` *(automation hook)*
- `RouteKeyboard` *(automation hook)*
- `SetFocus`
- `SetDownloadDirectory`
- `OnDownloadStateChanged`
- `Ping`
- `CaptureContext` *(CDP-oriented read-only extraction path; payload in JSON)*

### Download status eventing (v0)

- `SetDownloadDirectory` now accepts destination path and event-pipe name:
  - arg 1: download directory path
  - arg 2: named-pipe for host download event callbacks
- host emits `DOWNLOAD|id|state|path|error` messages onto that pipe
- client subscribes to the callback pipe and surfaces the most recent state in status row

### Recovery behavior (v0)

- Client monitors host liveness using a periodic timer and reconnect attempts.
- On command failure or host restart detection, client:
  - Restarts/reattaches `atlas-host.exe` if needed.
  - Recreates a fresh session and view tabs.
  - Replays saved tab URLs to rebuild host-side state.

### Data types implemented
- `RectPx`
- `DpiScale`
- `InputEventEnvelope`
- `DownloadState`
- `SessionConfig`

### v1 capture context contract

- `CaptureContext` command signature: `CaptureContext <tab_id>`.
- Response is `CAPTURE_CONTEXT|{json}` where JSON includes:
  - `schema_version`
  - `session_id`
  - `timestamp_utc`
  - `url`
  - `title`
  - `viewport_css`
  - `device_scale_factor`
  - `screenshot` object (`mime`, `sha256`, `temp_file`)
  - `ax` object (`source`, `nodes`)
  - `selected_text`
  - `focused_element`

### Input posture

- `RouteMouse`, `RouteWheel`, and `RouteKeyboard` are retained for automation tooling and manual
  scripting, not as the default human interaction path.
- Normal browsing UI input remains routed by Win32 controls / native focus/input flow.
