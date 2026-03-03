# Chromium OWL MVP build plan (x64 + ARM64)

## High-level OWL flow on Windows

```mermaid
graph LR
  subgraph OwlClient[OWL Client (Windows App Shell)]
    UI["Tab strip / URL bar / Back / Forward / Reload / Agent"]
    Viewport["Viewport HWND"]
  end
  subgraph OwlHost[Owl Host (Chromium process)
    HostSvc["owl.mojom.OwlHost"]
    WebContents["content::WebContents"]
  end
  subgraph ChromiumInt[Chromium internals]
    Renderers["Renderers (Blink/V8)"]
    GPU["GPU (Viz/Skia/ANGLE)"]
  end

  UI -->|Mojo IPC| HostSvc
  Viewport -->|CreateWebView(parent_hwnd, bounds, scale, profile)| HostSvc
  HostSvc --> WebContents
  WebContents --> Renderers
  Renderers --> GPU
  GPU --> Viewport
```

## MVP agent capture sequence (read-only)

```mermaid
graph LR
  User[User]
  Client[OWL Client]
  Host[OWL Host]
  Stub["Agent Stub (read-only)"]

  User -->|"Click Capture"| Client
  Client -->|CaptureFrame(webview)| Host
  Host -->|PNG + metadata| Client
  Client -->|"Image + URL/title"| Stub
  Stub -->|Read-only analysis result| Client
  Client -->|No-actions | User
```

## Target architecture constraints

- Initial embed backend: `HwndPresenter`.
- Secondary backend (`DCompPresenter`) is intentionally deferred.
- Profiles must support ephemeral logged-out mode using storage partition isolation.
- Host should survive and be relaunched by client on failure where possible.
