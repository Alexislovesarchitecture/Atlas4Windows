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
- `RouteMouse`
- `RouteWheel`
- `RouteKeyboard`
- `SetFocus`
- `SetDownloadDirectory`
- `OnDownloadStateChanged`
- `Ping`

### Download status eventing (v0)

- `SetDownloadDirectory` now accepts destination path and event-pipe name:
  - arg 1: download directory path
  - arg 2: named-pipe for host download event callbacks
- host emits `DOWNLOAD|id|state|path|error` messages onto that pipe
- client subscribes to the callback pipe and surfaces the most recent state in status row

## Recovery behavior (v0)

- Client monitors host liveness using a periodic timer and reconnect attempts.
- On command failure or host restart detection, client:
  - Restarts/reattaches `atlas-host.exe` if needed.
  - Recreates a fresh session and view tabs.
  - Replays saved tab URLs to rebuild host-side state.

## Data types implemented
- `RectPx`
- `DpiScale`
- `InputEventEnvelope`
- `DownloadState`
- `SessionConfig`
