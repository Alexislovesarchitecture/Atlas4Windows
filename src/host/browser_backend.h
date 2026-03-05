#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "common/protocol.h"

namespace atlas::host {

enum class BackendKind {
  HeadlessCdp,
  Owl,
};

struct BackendConfig {
  BackendKind kind = BackendKind::HeadlessCdp;
  bool capture_dom_snapshot = false;
  std::string owl_host_endpoint;
  std::string remote_debug_host = "127.0.0.1";
  uint16_t remote_debug_port = 9222;
  std::string chrome_binary_path;
};

struct ViewBinding {
  BackendKind kind = BackendKind::HeadlessCdp;
  uint64_t backend_view_id = 0;
  uint64_t backend_profile_id = 0;
  HWND placeholder_hwnd = nullptr;
  HWND child_hwnd = nullptr;
  HWND parent_hwnd = nullptr;
  HANDLE process_handle = nullptr;
  uint16_t remote_debug_port = 0;
  std::string user_data_dir;
  std::string devtools_endpoint;
  std::string cdp_target_id;
  std::string cdp_target_ws_url;
  bool accessibility_enabled = false;
  bool visible = true;
  bool focused = false;
};

struct CaptureRequest {
  uint32_t session_id = 0;
  uint32_t tab_id = 0;
  uint32_t view_id = 0;
  std::string url;
  std::string title;
  RECT rect{0, 0, 0, 0};
  atlas::DpiScale dpi{1.0f, 1.0f};
};

struct MouseDispatchParams {
  std::string type = "mouseMoved";
  std::string button = "none";
  int x = 0;
  int y = 0;
  int delta_x = 0;
  int delta_y = 0;
  int click_count = 1;
  uint32_t modifiers = 0;
};

struct KeyDispatchParams {
  UINT message = 0;
  WPARAM wparam = 0;
  LPARAM lparam = 0;
  uint32_t modifiers = 0;
};

class BrowserBackend {
 public:
  virtual ~BrowserBackend() = default;

  virtual BackendKind kind() const = 0;
  virtual bool CreateView(uint32_t view_id, HWND placeholder_hwnd, const RECT& bounds,
                          const atlas::DpiScale& dpi, ViewBinding& binding,
                          std::string& error) = 0;
  virtual bool DestroyView(ViewBinding& binding, std::string& error) = 0;
  virtual bool Navigate(ViewBinding& binding, const std::string& url, std::string& error) = 0;
  virtual bool ResizeView(ViewBinding& binding, const RECT& bounds, const atlas::DpiScale& dpi,
                          std::string& error) = 0;
  virtual bool SetVisible(ViewBinding& binding, bool visible, std::string& error) = 0;
  virtual bool SetFocus(ViewBinding& binding, bool focused, std::string& error) = 0;
  virtual bool CaptureContext(ViewBinding& binding, const CaptureRequest& request,
                              std::string& payload, std::string& error) = 0;
  virtual bool DispatchMouse(ViewBinding& binding, const MouseDispatchParams& params,
                             std::string& error) = 0;
  virtual bool DispatchWheel(ViewBinding& binding, const MouseDispatchParams& params,
                             std::string& error) = 0;
  virtual bool DispatchKeyboard(ViewBinding& binding, const KeyDispatchParams& params,
                                std::string& error) = 0;
  virtual std::string GetDevToolsEndpoint(const ViewBinding& binding) const = 0;
};

BackendKind ParseBackendKind(const std::string& token);
const char* BackendKindName(BackendKind kind);
std::unique_ptr<BrowserBackend> CreateBrowserBackend(const BackendConfig& config);
bool EnablePerMonitorV2DpiAwareness();

}  // namespace atlas::host
