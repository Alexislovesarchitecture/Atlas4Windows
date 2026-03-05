#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <cstdint>
#include <thread>
#include <string>
#include <vector>
#include <sstream>

#include "common/ipc_client.h"
#include "common/protocol.h"

namespace {

constexpr wchar_t kClientClass[] = L"AtlasClientWindow";
constexpr wchar_t kAddressClass[] = L"EDIT";
constexpr wchar_t kWindowTitle[] = L"Atlas Browser";
constexpr int kPad = 8;
constexpr int kTopPad = 10;
constexpr int kHostRetryCount = 30;
constexpr int kHostRetryDelayMs = 200;
constexpr char kDefaultProfilePath[] = "%TEMP%/atlas-session";
constexpr UINT_PTR kHostHealthTimerId = 1;
constexpr int kHostHealthTimerMs = 1500;
constexpr UINT kDownloadEventMessage = WM_APP + 7;
constexpr int kNavButtonWidth = 38;
constexpr int kNavButtonHeight = 30;
constexpr int kGoButtonWidth = 56;
constexpr int kCaptureButtonWidth = 116;
constexpr int kContextPanelWidth = 320;
constexpr int kRowGap = 8;
constexpr int kStatusHeight = 22;
constexpr uint16_t kHostRemoteDebugPort = 9222;
constexpr wchar_t kBackLabel[] = L"\u2039";
constexpr wchar_t kForwardLabel[] = L"\u203A";
constexpr wchar_t kReloadLabel[] = L"\u21BB";
constexpr wchar_t kNewTabLabel[] = L"+";
constexpr wchar_t kCloseTabLabel[] = L"\u00D7";
constexpr wchar_t kGoLabel[] = L"Go";
constexpr wchar_t kCaptureLabel[] = L"Capture Ctx";
constexpr wchar_t kWindowFontFace[] = L"Segoe UI";

enum ControlIds {
  IDC_BTN_BACK = 1001,
  IDC_BTN_FWD = 1002,
  IDC_BTN_RELOAD = 1003,
  IDC_BTN_NEW_TAB = 1004,
  IDC_BTN_CLOSE_TAB = 1005,
  IDC_BTN_GO = 1006,
  IDC_BTN_CAPTURE_CONTEXT = 1007,
  IDC_TAB_COMBO = 2001,
  IDC_ADDRESS = 2002,
  IDC_STATUS = 2003,
  IDC_CONTEXT_PANEL = 2004,
};
struct ClientTab {
  uint32_t session_id = 0;
  uint32_t tab_id = 0;
  uint32_t view_id = 0;
  uint64_t view_hwnd = 0;
  std::wstring url;
};

HWND g_window = nullptr;
HWND g_address = nullptr;
HWND g_tab_combo = nullptr;
HWND g_content_host = nullptr;
HWND g_status = nullptr;
HWND g_capture_btn = nullptr;
HWND g_context_panel = nullptr;
HFONT g_ui_font = nullptr;
HFONT g_title_font = nullptr;
HFONT g_url_font = nullptr;
atlas::IpcClient g_ipc;
uint32_t g_session_id = 0;
uint32_t g_current_tab_index = 0;
std::vector<ClientTab> g_tabs;
HANDLE g_host_process = nullptr;
std::string g_download_dir;
std::string g_download_event_pipe_name;
void* g_download_pipe = nullptr;
std::thread g_download_listener_thread;
std::atomic<bool> g_download_listener_running{false};
std::wstring g_status_base;
std::wstring g_download_status;
std::wstring g_capture_context_json;

struct DownloadEvent {
  uint32_t id = 0;
  std::wstring state;
  std::wstring file_path;
  int error_code = 0;
};

std::vector<DownloadEvent> g_download_history;
constexpr size_t kMaxRecentDownloads = 3;
bool EnsureHostConnection();
void UpdateTabCombo();
void ApplyTabState();
void UpdateGeometry();
void AttachCurrentView();
void CaptureCurrentContext();
bool IsAutomationInputMode();
void RouteMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam);
void RouteWheelEvent(WPARAM wParam, LPARAM lParam);
void RouteKeyboardEvent(UINT msg, WPARAM wParam, LPARAM lParam);

UINT ComputeCdpModifiers() {
  UINT modifiers = 0;
  if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= 1u;
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= 2u;
  if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) modifiers |= 4u;
  if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= 8u;
  return modifiers;
}

void EnablePerMonitorV2DpiAwareness() {
  HMODULE user32 = LoadLibraryW(L"user32.dll");
  if (user32) {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (fn && fn(reinterpret_cast<HANDLE>(-4))) {
      FreeLibrary(user32);
      return;
    }
    FreeLibrary(user32);
  }
  SetProcessDPIAware();
}

HFONT CreateAppFont(int point_size, int weight, const wchar_t* face = kWindowFontFace) {
  HDC hdc = GetDC(nullptr);
  int dpi_y = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
  if (hdc) ReleaseDC(nullptr, hdc);
  return CreateFontW(-MulDiv(point_size, dpi_y, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, face);
}

void ApplyControlFonts() {
  if (!g_window) return;
  if (g_ui_font) {
    SendMessageW(g_window, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(GetDlgItem(g_window, IDC_BTN_BACK), WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(GetDlgItem(g_window, IDC_BTN_FWD), WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(GetDlgItem(g_window, IDC_BTN_RELOAD), WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(GetDlgItem(g_window, IDC_BTN_NEW_TAB), WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(GetDlgItem(g_window, IDC_BTN_CLOSE_TAB), WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(GetDlgItem(g_window, IDC_BTN_GO), WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(g_tab_combo, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
  }
  if (g_title_font) {
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_title_font, TRUE);
  }
  if (g_url_font) {
    SendMessageW(g_address, WM_SETFONT, (WPARAM)g_url_font, TRUE);
  }
  if (g_capture_btn) {
    SendMessageW(g_capture_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
  }
  if (g_context_panel) {
    SendMessageW(g_context_panel, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
  }
}

inline std::string WideToUtf8(const std::wstring& w) {
  int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(needed ? needed - 1 : 0, 0);
  if (needed > 0) {
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), needed, nullptr, nullptr);
  }
  return out;
}

inline std::wstring Utf8ToWide(const std::string& s) {
  int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(needed ? needed - 1 : 0, 0);
  if (needed > 0) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), needed);
  }
  return out;
}

std::wstring DefaultDownloadDir() {
  wchar_t temp_path[MAX_PATH]{};
  if (GetTempPathW(MAX_PATH, temp_path) == 0) return L"C:\\Temp\\AtlasDownloads";
  std::wstring path = temp_path;
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
  path += L"AtlasDownloads";
  CreateDirectoryW(path.c_str(), nullptr);
  return path;
}

void ApplyStatusText() {
  if (!g_status) return;
  std::wstring status = g_status_base;
  if (!g_download_status.empty()) {
    status += L" | ";
    status += g_download_status;
  }
  SetWindowTextW(g_status, status.empty() ? L"" : status.c_str());
}

void SetStatus(const wchar_t* text) {
  g_status_base = text ? text : L"";
  ApplyStatusText();
}

void SetDownloadStatus(const std::wstring& text) {
  g_download_status = text;
  ApplyStatusText();
}

void SetContextPanelText(const std::wstring& text) {
  g_capture_context_json = text;
  if (!g_context_panel) return;
  SetWindowTextW(g_context_panel, g_capture_context_json.c_str());
}

std::wstring ToLowerCopy(const std::string& value) {
  std::wstring wide = Utf8ToWide(value);
  for (wchar_t& ch : wide) {
    ch = static_cast<wchar_t>(towlower(ch));
  }
  return wide;
}

std::wstring BuildDownloadStatusLine(const DownloadEvent& event) {
  std::wstringstream out;
  out << L"Download " << event.id << L": " << event.state;
  if (!event.file_path.empty()) {
    out << L" (" << event.file_path << L")";
  }
  if (event.error_code != 0) {
    out << L" [err " << event.error_code << L"]";
  }
  return out.str();
}

void RecordDownloadEvent(const DownloadEvent& event) {
  if (g_download_history.empty() || g_download_history.back().id != event.id ||
      g_download_history.back().state != event.state ||
      g_download_history.back().file_path != event.file_path) {
    g_download_history.push_back(event);
    if (g_download_history.size() > kMaxRecentDownloads) g_download_history.erase(g_download_history.begin());
  }

  std::wstring aggregate;
  for (auto it = g_download_history.rbegin(); it != g_download_history.rend(); ++it) {
    if (!aggregate.empty()) aggregate += L" | ";
    aggregate += BuildDownloadStatusLine(*it);
  }
  SetDownloadStatus(aggregate);
}

bool IsHostProcessAlive() {
  if (!g_host_process) return false;
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(g_host_process, &exit_code)) {
    CloseHandle(g_host_process);
    g_host_process = nullptr;
    return false;
  }
  if (exit_code != STILL_ACTIVE) {
    CloseHandle(g_host_process);
    g_host_process = nullptr;
    return false;
  }
  return true;
}

bool StartHostProcess() {
  if (IsHostProcessAlive()) return true;
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring path(exe_path);
  auto pos = path.find_last_of(L"\\/");
  if (pos != std::wstring::npos) path = path.substr(0, pos + 1);
  std::wstring host_exe = path + L"atlas-host.exe";

  STARTUPINFOW si{};
  PROCESS_INFORMATION pi{};
  si.cb = sizeof(si);

  std::wstring cmd = L"\"" + host_exe + L"\"";
  if (g_host_process) {
    CloseHandle(g_host_process);
    g_host_process = nullptr;
  }

  cmd += L" --remote_debugging_port=" + std::to_wstring(kHostRemoteDebugPort);
  if (!CreateProcessW(host_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, path.c_str(),
                     &si, &pi)) {
    return false;
  }
  g_host_process = pi.hProcess;
  if (pi.hThread) CloseHandle(pi.hThread);
  return true;
}

bool SendCommandInternal(const std::vector<std::string>& fields, std::string& response) {
  if (!g_ipc.isConnected()) {
    if (!EnsureHostConnection()) return false;
  }
  auto msg = atlas::BuildMessage(fields);
  return g_ipc.sendMessage(msg, response);
}

bool ConfigureDownloadEndpointInHost() {
  if (g_download_event_pipe_name.empty()) return false;
  g_download_dir = WideToUtf8(DefaultDownloadDir());
  std::vector<std::string> fields = {
      "SetDownloadDirectory", atlas::EscapeField(g_download_dir),
      atlas::EscapeField(g_download_event_pipe_name)};
  std::string response;
  if (!SendCommandInternal(fields, response)) return false;
  auto parts = atlas::SplitMessage(response);
  return parts.size() >= 2 && parts[0] == "OK";
}

void ProcessDownloadEvent(const std::string& line) {
  auto parts = atlas::SplitMessage(line);
  if (parts.empty() || parts[0] != "DOWNLOAD" || parts.size() < 5) return;
  DownloadEvent event;
  try {
    event.id = static_cast<uint32_t>(std::stoul(parts[1]));
  } catch (...) {
    return;
  }
  event.state = Utf8ToWide(parts[2]);
  if (event.state.empty()) event.state = L"UNKNOWN";
  event.state = ToLowerCopy(atlas::UnescapeField(parts[2]));
  if (event.state == L"started") event.state = L"STARTED";
  if (event.state == L"in_progress") event.state = L"IN_PROGRESS";
  event.file_path = Utf8ToWide(atlas::UnescapeField(parts[3]));
  try {
    event.error_code = std::stoi(parts[4]);
  } catch (...) {
    event.error_code = 0;
  }
  DownloadEvent* payload = new DownloadEvent(event);
  if (!g_window || !IsWindow(g_window)) {
    delete payload;
    return;
  }
  PostMessageW(g_window, kDownloadEventMessage, 0, reinterpret_cast<LPARAM>(payload));
}

void DownloadEventPump() {
  while (g_download_listener_running.load()) {
    g_download_pipe =
        CreateNamedPipeA(g_download_event_pipe_name.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE |
                                                               PIPE_READMODE_MESSAGE | PIPE_WAIT,
                        1, 4096, 4096, 0, nullptr);
    if (!g_download_pipe || g_download_pipe == INVALID_HANDLE_VALUE) {
      g_download_pipe = nullptr;
      Sleep(250);
      continue;
    }
    if (!ConnectNamedPipe((HANDLE)g_download_pipe, nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
      CloseHandle((HANDLE)g_download_pipe);
      g_download_pipe = nullptr;
      Sleep(100);
      continue;
    }

    std::string pending;
    char buffer[1024];
    while (g_download_listener_running.load()) {
      DWORD bytes_read = 0;
      BOOL ok = ReadFile((HANDLE)g_download_pipe, buffer, sizeof(buffer) - 1, &bytes_read, nullptr);
      if (!ok || bytes_read == 0) {
        break;
      }
      buffer[bytes_read] = 0;
      pending.append(buffer, bytes_read);
      while (true) {
        const auto line_end = pending.find('\n');
        if (line_end == std::string::npos) break;
        std::string line = pending.substr(0, line_end + 1);
        pending.erase(0, line_end + 1);
        ProcessDownloadEvent(line);
      }
    }

    DisconnectNamedPipe((HANDLE)g_download_pipe);
    CloseHandle((HANDLE)g_download_pipe);
    g_download_pipe = nullptr;
    if (!g_download_listener_running.load()) break;
    Sleep(100);
  }
}

void StartDownloadPipeListener() {
  if (g_download_listener_running.load()) return;
  g_download_dir = WideToUtf8(DefaultDownloadDir());
  g_download_event_pipe_name = "\\\\.\\pipe\\AtlasDownloadEvents-" + std::to_string(GetCurrentProcessId());
  g_download_listener_running = true;
  g_download_listener_thread = std::thread(DownloadEventPump);
  g_download_listener_thread.detach();
}

bool EnsureDownloadPipeConfigured() {
  if (g_download_event_pipe_name.empty() || g_download_dir.empty()) return false;
  return ConfigureDownloadEndpointInHost();
}

bool EnsureHostConnection() {
  if (!StartHostProcess()) return false;
  for (int i = 0; i < kHostRetryCount; ++i) {
    g_ipc.close();
    if (g_ipc.connect(atlas::kPipeName)) {
      if (EnsureDownloadPipeConfigured()) {
        SetDownloadStatus(L"download channel active");
      } else {
        SetDownloadStatus(L"download channel unavailable");
      }
      return true;
    }
    Sleep(kHostRetryDelayMs);
  }
  return false;
}

bool SendCommand(const std::vector<std::string>& fields, std::string& response) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (SendCommandInternal(fields, response)) return true;
    SetStatus(L"host connection lost; restoring...");
    if (!EnsureHostConnection()) return false;
    if (!g_tabs.empty()) {
      // best-effort recovery path after reconnect
      bool recovered = false;
      std::vector<ClientTab> previous = g_tabs;
      g_tabs.clear();
      g_session_id = 0;
      uint32_t previous_index = g_current_tab_index;
      std::string session_response;
      if (!SendCommandInternal({"CreateSession", kDefaultProfilePath, "false"},
                               session_response)) {
        g_tabs = previous;
        g_current_tab_index = previous_index;
        continue;
      }
      auto session_parts = atlas::SplitMessage(session_response);
      if (session_parts.size() >= 2 && session_parts[0] == "SESSION") {
        g_session_id = (uint32_t)std::stoul(session_parts[1]);
      } else {
        g_tabs = previous;
        g_current_tab_index = previous_index;
        continue;
      }
      for (const auto& old_tab : previous) {
        std::string tab_response;
        SendCommandInternal({"CreateTab", std::to_string(g_session_id)}, tab_response);
        auto tab_parts = atlas::SplitMessage(tab_response);
        if (tab_parts.size() < 2 || tab_parts[0] != "TAB") {
          continue;
        }
        uint32_t tid = (uint32_t)std::stoul(tab_parts[1]);
        std::string view_response;
        SendCommandInternal({"CreateEmbeddedView", std::to_string(tid)}, view_response);
        auto view_parts = atlas::SplitMessage(view_response);
        uint64_t hwnd = 0;
        uint32_t vid = 0;
        if (view_parts.size() >= 3 && view_parts[0] == "VIEW") {
          vid = (uint32_t)std::stoul(view_parts[1]);
          hwnd = std::stoull(view_parts[2]);
        }
        ClientTab restored{g_session_id, tid, vid, hwnd, old_tab.url};
        if (!old_tab.url.empty() && old_tab.url != L"about:blank") {
          SendCommandInternal({"Navigate", std::to_string(tid), atlas::EscapeField(WideToUtf8(old_tab.url))},
                              response);
        }
        g_tabs.push_back(restored);
      }
      if (g_tabs.empty()) {
        g_session_id = 0;
        continue;
      }
      g_current_tab_index = std::min(previous_index, (uint32_t)g_tabs.size() - 1);
      UpdateTabCombo();
      ApplyTabState();
      UpdateGeometry();
      AttachCurrentView();
      recovered = true;

      if (recovered) {
        SetStatus(L"host restored");
        continue;
      }
      g_tabs = previous;
      g_current_tab_index = previous_index;
    }
  }
  return false;
}

void UpdateTabCombo() {
  SendMessageW(g_tab_combo, CB_RESETCONTENT, 0, 0);
  for (size_t i = 0; i < g_tabs.size(); ++i) {
    std::wstring label = L"Tab " + std::to_wstring(i + 1);
    SendMessageW(g_tab_combo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
  }
  if (!g_tabs.empty()) {
    SendMessageW(g_tab_combo, CB_SETCURSEL, g_current_tab_index, 0);
  }
}

void UpdateGeometry() {
  RECT rc;
  GetClientRect(g_window, &rc);
  int status_y = std::max(0, static_cast<int>(rc.bottom) - kStatusHeight - kPad);
  int status_w = std::max(1, static_cast<int>(rc.right) - 2 * kPad);
  MoveWindow(g_status, kPad, status_y, status_w, kStatusHeight, TRUE);

  int left = kPad;
  int top = kTopPad;
  int content_top = top + kNavButtonHeight + kRowGap + kNavButtonHeight + kRowGap;
  int address_x = left + (kNavButtonWidth + kPad) * 5;
  int address_w = std::max(140, static_cast<int>(rc.right) - address_x - kGoButtonWidth - 4 * kPad);
  if (address_x + address_w + kGoButtonWidth + kPad > rc.right - kPad) {
    address_w = std::max(120, static_cast<int>(rc.right) - kPad - address_x - kGoButtonWidth - kPad);
  }
  if (address_x + address_w + kGoButtonWidth + kPad > static_cast<int>(rc.right) - kPad) {
    address_x = left;
    address_w = std::max(120, static_cast<int>(rc.right) - kPad * 3 - kGoButtonWidth);
  }

  MoveWindow(GetDlgItem(g_window, IDC_BTN_BACK), left, top, kNavButtonWidth, kNavButtonHeight, TRUE);
  MoveWindow(GetDlgItem(g_window, IDC_BTN_FWD), left + kNavButtonWidth + kPad, top, kNavButtonWidth,
             kNavButtonHeight, TRUE);
  MoveWindow(GetDlgItem(g_window, IDC_BTN_RELOAD),
             left + (kNavButtonWidth + kPad) * 2, top, kNavButtonWidth, kNavButtonHeight, TRUE);
  MoveWindow(GetDlgItem(g_window, IDC_BTN_NEW_TAB),
             left + (kNavButtonWidth + kPad) * 3, top, kNavButtonWidth, kNavButtonHeight, TRUE);
  MoveWindow(GetDlgItem(g_window, IDC_BTN_CLOSE_TAB),
             left + (kNavButtonWidth + kPad) * 4, top, kNavButtonWidth, kNavButtonHeight, TRUE);

  int row2_top = top + kNavButtonHeight + kRowGap;
  int capture_x = std::max(kPad, static_cast<int>(rc.right) - kPad - kCaptureButtonWidth);
  int combo_width = std::max(1, capture_x - (kPad * 2));
  if (combo_width < 180) combo_width = 180;
  MoveWindow(g_tab_combo, left, row2_top, combo_width, kNavButtonHeight, TRUE);
  MoveWindow(GetDlgItem(g_window, IDC_BTN_GO), address_x + address_w + kPad, top, kGoButtonWidth,
             kNavButtonHeight, TRUE);
  MoveWindow(g_address, address_x, top, std::max(120, address_w), kNavButtonHeight, TRUE);
  MoveWindow(g_capture_btn, capture_x, row2_top, kCaptureButtonWidth, kNavButtonHeight, TRUE);

  const int host_height =
      std::max(1, static_cast<int>(rc.bottom) - content_top - (kStatusHeight + 2 * kPad));
  int host_width = std::max(1, static_cast<int>(rc.right) - (3 * kPad) - kContextPanelWidth);
  MoveWindow(g_content_host, kPad, content_top, host_width, host_height, TRUE);
  if (g_context_panel) {
    const int panel_x = kPad + host_width + kPad;
    MoveWindow(g_context_panel, panel_x, content_top, std::max(180, kContextPanelWidth),
               host_height, TRUE);
  }
  if (!g_tabs.empty()) {
    atlas::RectPx rect{kPad, content_top, host_width, host_height};
    auto& tab = g_tabs[g_current_tab_index];
    if (tab.view_id) {
      std::string response;
      SendCommand({"ResizeView", std::to_string(tab.view_id), std::to_string(rect.x),
                   std::to_string(rect.y), std::to_string(rect.width), std::to_string(rect.height)},
                  response);
    }
  }
}

void ApplyTabState() {
  if (g_tabs.empty()) return;

  auto& tab = g_tabs[g_current_tab_index];
  SetWindowTextW(g_address, tab.url.c_str());
  for (size_t i = 0; i < g_tabs.size(); ++i) {
    const auto& t = g_tabs[i];
    if (t.view_id) {
      std::string response;
      SendCommand({"SetViewVisibility", std::to_string(t.view_id),
                   i == g_current_tab_index ? "1" : "0"},
                 response);
    }
  }
}

void AttachCurrentView() {
  if (g_tabs.empty()) return;
  auto& tab = g_tabs[g_current_tab_index];
  if (tab.view_id == 0) return;
  RECT rc;
  GetClientRect(g_content_host, &rc);

  std::string response;
  SendCommand({"AttachToParentHwnd", std::to_string(tab.view_id),
               std::to_string((uint64_t)(uintptr_t)g_content_host),
               std::to_string(0), std::to_string(0), std::to_string(rc.right - rc.left),
               std::to_string(rc.bottom - rc.top), "1.0", "1.0"},
              response);
}

void EnsureStartedTab() {
  if (!g_session_id) {
    std::string response;
    std::vector<std::string> fields = {"CreateSession", kDefaultProfilePath, "false"};
    if (!SendCommand(fields, response)) return;
    auto parts = atlas::SplitMessage(response);
    if (parts.size() >= 2 && parts[0] == "SESSION") {
      g_session_id = std::stoul(parts[1]);
    }
  }

  std::string response;
  SendCommand({"CreateTab", std::to_string(g_session_id)}, response);
  auto tab_parts = atlas::SplitMessage(response);
  if (tab_parts.size() < 2 || tab_parts[0] != "TAB") return;
  uint32_t tid = (uint32_t)std::stoul(tab_parts[1]);
  g_tabs.push_back({g_session_id, tid, 0, 0, L"about:blank"});
  g_current_tab_index = (uint32_t)g_tabs.size() - 1;
  UpdateTabCombo();

  response.clear();
  SendCommand({"CreateEmbeddedView", std::to_string(tid)}, response);
  auto view_parts = atlas::SplitMessage(response);
  if (view_parts.size() >= 3 && view_parts[0] == "VIEW") {
    g_tabs[g_current_tab_index].view_id = (uint32_t)std::stoul(view_parts[1]);
    g_tabs[g_current_tab_index].view_hwnd = std::stoull(view_parts[2]);
  }
  AttachCurrentView();
  ApplyTabState();
  UpdateGeometry();
}

void NavigateCurrent(const std::wstring& raw_url) {
  if (g_tabs.empty()) return;
  auto& tab = g_tabs[g_current_tab_index];
  std::string url = WideToUtf8(raw_url);
  std::string response;
  SendCommand({"Navigate", std::to_string(tab.tab_id), url}, response);
  tab.url = raw_url;
}

void AddTab() {
  if (!g_session_id) return;
  std::string response;
  SendCommand({"CreateTab", std::to_string(g_session_id)}, response);
  auto tab_parts = atlas::SplitMessage(response);
  if (tab_parts.size() < 2 || tab_parts[0] != "TAB") return;
  uint32_t tid = (uint32_t)std::stoul(tab_parts[1]);

  g_tabs.push_back({g_session_id, tid, 0, 0, L"about:blank"});
  auto tab_index = (uint32_t)(g_tabs.size() - 1);
  UpdateTabCombo();

  response.clear();
  SendCommand({"CreateEmbeddedView", std::to_string(tid)}, response);
  auto view_parts = atlas::SplitMessage(response);
  if (view_parts.size() >= 3 && view_parts[0] == "VIEW") {
    g_tabs[tab_index].view_id = (uint32_t)std::stoul(view_parts[1]);
    g_tabs[tab_index].view_hwnd = std::stoull(view_parts[2]);
  }

  g_current_tab_index = tab_index;
  ApplyTabState();
  UpdateGeometry();
  AttachCurrentView();
}

void CloseCurrentTab() {
  if (g_tabs.empty()) return;
  auto& tab = g_tabs[g_current_tab_index];
  std::string response;
  SendCommand({"CloseTab", std::to_string(tab.tab_id)}, response);

  g_tabs.erase(g_tabs.begin() + g_current_tab_index);
  if (g_tabs.empty()) {
    g_current_tab_index = 0;
    EnsureStartedTab();
    return;
  }
  if (g_current_tab_index >= g_tabs.size()) g_current_tab_index = (uint32_t)g_tabs.size() - 1;
  UpdateTabCombo();
  ApplyTabState();
  AttachCurrentView();
}

bool IsAutomationInputMode() {
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
         (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

void CaptureCurrentContext() {
  if (g_tabs.empty()) return;
  std::string response;
  SendCommand({"CaptureContext", std::to_string(g_tabs[g_current_tab_index].tab_id)}, response);
  auto parts = atlas::SplitMessage(response);
  if (parts.size() >= 2 && parts[0] == "CAPTURE_CONTEXT") {
    SetContextPanelText(Utf8ToWide(atlas::UnescapeField(parts[1])));
    SetStatus(L"CaptureContext ready");
    return;
  }
  SetStatus(L"CaptureContext failed");
}

void RouteMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (!IsAutomationInputMode()) return;
  if (g_tabs.empty()) return;
  auto& tab = g_tabs[g_current_tab_index];
  if (!tab.view_id) return;
  POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  MapWindowPoints(g_window, g_content_host, &p, 1);

  std::string response;
  std::string event = "mouseMoved";
  std::string button = "none";
  int click_count = 0;
  switch (msg) {
    case WM_LBUTTONDOWN:
      event = "mousePressed";
      button = "left";
      click_count = 1;
      break;
    case WM_LBUTTONUP:
      event = "mouseReleased";
      button = "left";
      click_count = 1;
      break;
    case WM_RBUTTONDOWN:
      event = "mousePressed";
      button = "right";
      click_count = 1;
      break;
    case WM_RBUTTONUP:
      event = "mouseReleased";
      button = "right";
      click_count = 1;
      break;
    case WM_MOUSEMOVE:
      break;
    default:
      return;
  }
  SendCommand({"RouteMouse", std::to_string(tab.view_id), event, button, std::to_string(p.x),
               std::to_string(p.y), std::to_string(click_count),
               std::to_string(ComputeCdpModifiers())},
              response);
}

void RouteWheelEvent(WPARAM wParam, LPARAM lParam) {
  if (!IsAutomationInputMode()) return;
  if (g_tabs.empty()) return;
  auto& tab = g_tabs[g_current_tab_index];
  if (!tab.view_id) return;
  POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  ScreenToClient(g_content_host, &p);
  int delta = GET_WHEEL_DELTA_WPARAM(wParam);
  std::string response;
  SendCommand({"RouteWheel", std::to_string(tab.view_id), "mouseWheel", std::to_string(p.x),
               std::to_string(p.y), std::to_string(delta),
               std::to_string(ComputeCdpModifiers())},
              response);
}

void RouteKeyboardEvent(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (!IsAutomationInputMode()) return;
  if (g_tabs.empty()) return;
  auto& tab = g_tabs[g_current_tab_index];
  if (!tab.view_id) return;
  std::string response;
  SendCommand({"RouteKeyboard", std::to_string(tab.view_id), std::to_string(msg),
               std::to_string(wParam), std::to_string(lParam),
               std::to_string(ComputeCdpModifiers())},
              response);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        g_ui_font = CreateAppFont(11, FW_MEDIUM);
        g_title_font = CreateAppFont(11, FW_NORMAL);
        g_url_font = CreateAppFont(12, FW_NORMAL);

        g_address = CreateWindowW(kAddressClass, L"about:blank",
                                 WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | WS_BORDER, 0, 0, 0, 0,
                                 hwnd, (HMENU)IDC_ADDRESS, instance, nullptr);
        g_tab_combo = CreateWindowW(WC_COMBOBOXW, L"",
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, hwnd,
                                   (HMENU)IDC_TAB_COMBO, instance, nullptr);
        g_content_host = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_BORDER, 0, 0, 0, 0,
                                      hwnd, (HMENU)0, instance, nullptr);
        g_status = CreateWindowW(L"STATIC", L"starting...", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                (HMENU)IDC_STATUS, instance, nullptr);
        CreateWindowW(L"BUTTON", kBackLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0,
                     0, hwnd, (HMENU)IDC_BTN_BACK, instance, nullptr);
        CreateWindowW(L"BUTTON", kForwardLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                     0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_FWD, instance, nullptr);
        CreateWindowW(L"BUTTON", kReloadLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0,
                     0, hwnd, (HMENU)IDC_BTN_RELOAD, instance, nullptr);
        CreateWindowW(L"BUTTON", kNewTabLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0,
                     0, hwnd, (HMENU)IDC_BTN_NEW_TAB, instance, nullptr);
        CreateWindowW(L"BUTTON", kCloseTabLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0,
                     0, 0, hwnd, (HMENU)IDC_BTN_CLOSE_TAB, instance, nullptr);
        CreateWindowW(L"BUTTON", kGoLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0,
                     hwnd, (HMENU)IDC_BTN_GO, instance, nullptr);
        g_capture_btn = CreateWindowW(
            L"BUTTON", kCaptureLabel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_BTN_CAPTURE_CONTEXT, instance, nullptr);
        g_context_panel = CreateWindowW(
            L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                            ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)IDC_CONTEXT_PANEL, instance, nullptr);
        ApplyControlFonts();
      }
      StartDownloadPipeListener();
      SetStatus(L"waiting for host");
      if (!StartHostProcess() || !EnsureHostConnection()) {
        SetStatus(L"host not available");
      }
      SetTimer(hwnd, kHostHealthTimerId, kHostHealthTimerMs, nullptr);
      EnsureStartedTab();
      SetStatus(L"Atlas Browser ready");
      return 0;
    case WM_SIZE:
      UpdateGeometry();
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wParam);
      if (id == IDC_BTN_NEW_TAB) {
        AddTab();
        return 0;
      }
      if (id == IDC_BTN_CLOSE_TAB) {
        CloseCurrentTab();
        return 0;
      }
      if (id == IDC_BTN_CAPTURE_CONTEXT) {
        CaptureCurrentContext();
        return 0;
      }
      if (id == IDC_BTN_GO) {
        wchar_t url[2048] = {};
        GetWindowTextW(g_address, url, 2048);
        NavigateCurrent(url);
        return 0;
      }
      if (id == IDC_BTN_BACK) {
        if (!g_tabs.empty()) {
          std::string response;
          SendCommand({"GoBack", std::to_string(g_tabs[g_current_tab_index].tab_id)}, response);
        }
        return 0;
      }
      if (id == IDC_BTN_FWD) {
        if (!g_tabs.empty()) {
          std::string response;
          SendCommand({"GoForward", std::to_string(g_tabs[g_current_tab_index].tab_id)}, response);
        }
        return 0;
      }
      if (id == IDC_BTN_RELOAD) {
        if (!g_tabs.empty()) {
          std::string response;
          SendCommand({"Reload", std::to_string(g_tabs[g_current_tab_index].tab_id)}, response);
        }
        return 0;
      }
      if (id == IDC_TAB_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
        g_current_tab_index = (uint32_t)SendMessageW(g_tab_combo, CB_GETCURSEL, 0, 0);
        UpdateGeometry();
        ApplyTabState();
        AttachCurrentView();
        return 0;
      }
      return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      if (IsAutomationInputMode()) {
        RouteMouseEvent(msg, wParam, lParam);
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
      if (IsAutomationInputMode()) {
        RouteWheelEvent(wParam, lParam);
        return 0;
      }
      break;
    case WM_TIMER:
      if (wParam == kHostHealthTimerId) {
        if (!IsHostProcessAlive()) {
          SetStatus(L"host check: attempting reconnect");
        } else if (!g_ipc.isConnected()) {
          EnsureHostConnection();
          return 0;
        }
        std::string response;
        SendCommand({"Ping"}, response);
      }
      return 0;
    case kDownloadEventMessage:
      if (lParam) {
        auto* event = reinterpret_cast<DownloadEvent*>(lParam);
        if (event) {
          RecordDownloadEvent(*event);
          delete event;
        }
      }
      return 0;
    case WM_KEYDOWN: {
      if ((GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        switch (wParam) {
          case 'L':
            SetFocus(g_address);
            SendMessageW(g_address, EM_SETSEL, 0, -1);
            return 0;
          case 'T':
            AddTab();
            return 0;
          case 'W':
            CloseCurrentTab();
            return 0;
        }
      }
      if (IsAutomationInputMode()) {
        RouteKeyboardEvent(msg, wParam, lParam);
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      g_download_listener_running = false;
      if (g_download_pipe && g_download_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle((HANDLE)g_download_pipe);
        g_download_pipe = nullptr;
      }
      KillTimer(hwnd, kHostHealthTimerId);
      if (g_host_process) {
        CloseHandle(g_host_process);
        g_host_process = nullptr;
      }
      if (g_ui_font) {
        DeleteObject(g_ui_font);
        g_ui_font = nullptr;
      }
      if (g_title_font) {
        DeleteObject(g_title_font);
        g_title_font = nullptr;
      }
      if (g_url_font) {
        DeleteObject(g_url_font);
        g_url_font = nullptr;
      }
      g_ipc.close();
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
  EnablePerMonitorV2DpiAwareness();
  InitCommonControls();
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = kClientClass;
  RegisterClassW(&wc);

  g_window = CreateWindowW(kClientClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr, hInstance,
                          nullptr);
  if (!g_window) return 1;
  ShowWindow(g_window, nCmdShow);
  UpdateWindow(g_window);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
