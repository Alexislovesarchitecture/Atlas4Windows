#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "common/ipc_server.h"
#include "common/protocol.h"

namespace {

using AtlasWindowId = uint32_t;

struct TabState;
struct ViewState;

struct TabState {
  AtlasWindowId tab_id = 0;
  AtlasWindowId session_id = 0;
  AtlasWindowId view_id = 0;
  std::string url;
  HWND hw = nullptr;
};

struct ViewState {
  AtlasWindowId view_id = 0;
  AtlasWindowId tab_id = 0;
  HWND hw = nullptr;
  HWND host_parent = nullptr;
  RECT rect{0, 0, 0, 0};
  DpiScale dpi{1.0f, 1.0f};
};

struct SessionState {
  AtlasWindowId session_id = 0;
  std::string profile_path;
  bool incognito = false;
  std::unordered_map<AtlasWindowId, TabState> tabs;
};

HINSTANCE g_instance = nullptr;
std::string g_download_dir;
std::string g_download_event_pipe_name;
void* g_download_event_pipe = nullptr;
std::atomic<uint32_t> g_next_download_id{1};
AtlasWindowId g_next_session_id = 1;
AtlasWindowId g_next_tab_id = 1;
AtlasWindowId g_next_view_id = 1;
std::unordered_map<AtlasWindowId, SessionState> g_sessions;
std::unordered_map<AtlasWindowId, ViewState> g_views;
std::mutex g_download_mutex;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
  return value;
}

void CloseDownloadPipe() {
  if (!g_download_event_pipe) return;
  CloseHandle((HANDLE)g_download_event_pipe);
  g_download_event_pipe = nullptr;
}

bool OpenDownloadPipe() {
  if (g_download_event_pipe) return true;
  if (g_download_event_pipe_name.empty()) return false;

  g_download_event_pipe = CreateFileA(g_download_event_pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
  if (g_download_event_pipe == nullptr || g_download_event_pipe == INVALID_HANDLE_VALUE) {
    g_download_event_pipe = nullptr;
    return false;
  }
  return true;
}

void EmitDownloadState(AtlasWindowId download_id, atlas::DownloadState state,
                      const std::string& file_path, int error_code) {
  std::lock_guard<std::mutex> lock(g_download_mutex);
  if (!OpenDownloadPipe()) return;

  std::string state_name = "UNKNOWN";
  switch (state) {
    case atlas::DownloadState::STARTED:
      state_name = "STARTED";
      break;
    case atlas::DownloadState::IN_PROGRESS:
      state_name = "IN_PROGRESS";
      break;
    case atlas::DownloadState::COMPLETE:
      state_name = "COMPLETE";
      break;
    case atlas::DownloadState::FAILED:
      state_name = "FAILED";
      break;
    case atlas::DownloadState::CANCELED:
      state_name = "CANCELED";
      break;
    default:
      break;
  }

  auto message = atlas::BuildMessage({"DOWNLOAD", std::to_string(download_id), state_name, file_path,
                                     std::to_string(error_code)});
  DWORD bytes_written = 0;
  if (!WriteFile((HANDLE)g_download_event_pipe, message.data(),
                 static_cast<DWORD>(message.size()), &bytes_written, nullptr)) {
    CloseDownloadPipe();
  }
}

void EmitDownloadSimulatedSequence(const std::string& raw_url) {
  std::string url = ToLower(raw_url);
  if (url.find(".zip") == std::string::npos && url.find(".pdf") == std::string::npos &&
      url.find(".msi") == std::string::npos && url.find(".exe") == std::string::npos &&
      url.find("download=") == std::string::npos &&
      url.find("?download") == std::string::npos) {
    return;
  }

  const uint32_t id = g_next_download_id.fetch_add(1);
  std::string file_name = "download.bin";
  auto slash = url.find_last_of("/");
  if (slash != std::string::npos && slash + 1 < url.size()) {
    file_name = url.substr(slash + 1);
    auto query = file_name.find('?');
    if (query != std::string::npos) file_name = file_name.substr(0, query);
    if (file_name.empty()) file_name = "download.bin";
  }
  std::string target = g_download_dir.empty()
                           ? ("C:\\tmp\\" + file_name)
                           : (g_download_dir.back() == '\\' ? g_download_dir + file_name
                                                           : g_download_dir + "\\" + file_name);

  std::thread([id, target] {
    EmitDownloadState(id, atlas::DownloadState::STARTED, target, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EmitDownloadState(id, atlas::DownloadState::IN_PROGRESS, target, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EmitDownloadState(id, atlas::DownloadState::COMPLETE, target, 0);
  }).detach();
}

inline bool ParseUint32(const std::string& value, AtlasWindowId& out) {
  if (value.empty()) return false;
  try {
    out = static_cast<AtlasWindowId>(std::stoull(value));
    return true;
  } catch (...) {
    return false;
  }
}

LRESULT CALLBACK ViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    wchar_t text[256];
    GetWindowTextW(hwnd, text, 256);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCDESTROY) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::string HandleCommand(const std::string& line) {
  const auto parts = atlas::SplitMessage(line);
  if (parts.empty()) return atlas::ParseError("empty request");

  const auto cmd = atlas::ParseCommand(parts[0]);
  switch (cmd) {
    case atlas::Command::CreateSession: {
      if (parts.size() < 2) return atlas::ParseError("missing session profile");
      SessionState session;
      session.session_id = g_next_session_id++;
      session.profile_path = atlas::UnescapeField(parts[1]);
      if (parts.size() >= 3) {
        session.incognito = atlas::UnescapeField(parts[2]) == "true";
      }
      g_sessions[session.session_id] = std::move(session);
      return atlas::BuildMessage({"SESSION", std::to_string(g_next_session_id - 1)});
    }
    case atlas::Command::CreateTab: {
      AtlasWindowId sid = 0;
      if (parts.size() < 2 || !ParseUint32(parts[1], sid)) return atlas::ParseError("bad session");
      auto it = g_sessions.find(sid);
      if (it == g_sessions.end()) return atlas::ParseError("session not found");
      TabState tab;
      tab.tab_id = g_next_tab_id++;
      tab.session_id = sid;
      tab.url = "about:blank";
      it->second.tabs[tab.tab_id] = tab;
      return atlas::BuildMessage({"TAB", std::to_string(tab.tab_id)});
    }
    case atlas::Command::Navigate: {
      AtlasWindowId tid = 0;
      if (parts.size() < 3 || !ParseUint32(parts[1], tid)) return atlas::ParseError("bad tab");
      std::string url = atlas::UnescapeField(parts[2]);
      for (auto& entry : g_sessions) {
        auto tit = entry.second.tabs.find(tid);
        if (tit == entry.second.tabs.end()) continue;
        tit->second.url = url;
        if (tit->second.view_id > 0) {
          auto vit = g_views.find(tit->second.view_id);
          if (vit != g_views.end() && vit->second.hw) {
            std::wstring wide(url.begin(), url.end());
            SetWindowTextW(vit->second.hw, wide.c_str());
            InvalidateRect(vit->second.hw, nullptr, TRUE);
          }
        }
        EmitDownloadSimulatedSequence(url);
        return atlas::ParseOk("navigated");
      }
      return atlas::ParseError("tab not found");
    }
    case atlas::Command::GoBack:
    case atlas::Command::GoForward:
    case atlas::Command::Reload:
      return atlas::ParseOk("noop");
    case atlas::Command::CloseTab: {
      AtlasWindowId tid = 0;
      if (parts.size() < 2 || !ParseUint32(parts[1], tid)) return atlas::ParseError("bad tab");
      for (auto& entry : g_sessions) {
        auto tit = entry.second.tabs.find(tid);
        if (tit == entry.second.tabs.end()) continue;
        if (tit->second.view_id) {
          auto vit = g_views.find(tit->second.view_id);
          if (vit != g_views.end()) {
            DestroyWindow(vit->second.hw);
            g_views.erase(vit);
          }
        }
        entry.second.tabs.erase(tit);
        return atlas::ParseOk("closed");
      }
      return atlas::ParseError("tab not found");
    }
    case atlas::Command::CreateEmbeddedView: {
      AtlasWindowId tid = 0;
      if (parts.size() < 2 || !ParseUint32(parts[1], tid)) return atlas::ParseError("bad tab");
      for (auto& entry : g_sessions) {
        auto tit = entry.second.tabs.find(tid);
        if (tit == entry.second.tabs.end()) continue;
        ViewState view;
        view.view_id = g_next_view_id++;
        view.tab_id = tid;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ViewWndProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = L"AtlasContentHostView";
        RegisterClassExW(&wc);
        view.hw = CreateWindowExW(0, L"AtlasContentHostView", L"about:blank", WS_CHILD | WS_VISIBLE,
                                  0, 0, 1, 1, nullptr, nullptr, g_instance, nullptr);
        if (!view.hw) return atlas::ParseError("could not create host child hwnd");
        tit->second.view_id = view.view_id;
        tit->second.hw = view.hw;
        g_views[view.view_id] = view;
        return atlas::BuildMessage(
            {"VIEW", std::to_string(view.view_id),
             std::to_string(reinterpret_cast<uint64_t>(view.hw))});
      }
      return atlas::ParseError("tab not found");
    }
    case atlas::Command::AttachToParentHwnd: {
      if (parts.size() < 7) return atlas::ParseError("missing attach args");
      AtlasWindowId vid = 0;
      uint64_t p_hwnd = 0;
      RectPx rect;
      DpiScale dpi;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad view id");
      try {
        p_hwnd = std::stoull(parts[2]);
      } catch (...) {
        return atlas::ParseError("bad parent hwnd");
      }
      try {
        rect.x = std::stoi(parts[3]);
        rect.y = std::stoi(parts[4]);
        rect.width = std::stoi(parts[5]);
        rect.height = std::stoi(parts[6]);
        if (parts.size() >= 9) {
          dpi.x = std::stof(parts[7]);
          dpi.y = std::stof(parts[8]);
        }
      } catch (...) {
        return atlas::ParseError("bad rect/dpi");
      }
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      auto& view = it->second;
      HWND parent = (HWND)(uintptr_t)p_hwnd;
      SetParent(view.hw, parent);
      SetWindowLongPtrW(view.hw, GWL_STYLE,
                        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE);
      SetWindowPos(view.hw, nullptr, rect.x, rect.y, rect.width, rect.height,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
      view.host_parent = parent;
      view.rect = {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
      view.dpi = dpi;
      return atlas::ParseOk("attached");
    }
    case atlas::Command::ResizeView: {
      if (parts.size() < 6) return atlas::ParseError("missing resize args");
      AtlasWindowId vid = 0;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad view id");
      RECT rect = {0, 0, 0, 0};
      try {
        rect.left = std::stoi(parts[2]);
        rect.top = std::stoi(parts[3]);
        rect.right = rect.left + std::stoi(parts[4]);
        rect.bottom = rect.top + std::stoi(parts[5]);
      } catch (...) {
        return atlas::ParseError("bad geometry");
      }
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      it->second.rect = rect;
      SetWindowPos(it->second.hw, nullptr, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top,
                   SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
      return atlas::ParseOk("resized");
    }
    case atlas::Command::SetViewVisibility: {
      AtlasWindowId vid = 0;
      if (parts.size() < 3 || !ParseUint32(parts[1], vid)) return atlas::ParseError("bad args");
      bool visible = parts[2] == "1" || parts[2] == "true";
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      ShowWindow(it->second.hw, visible ? SW_SHOW : SW_HIDE);
      return atlas::ParseOk("visibility");
    }
    case atlas::Command::DestroyView: {
      AtlasWindowId vid = 0;
      if (parts.size() < 2 || !ParseUint32(parts[1], vid)) return atlas::ParseError("bad view");
      auto vit = g_views.find(vid);
      if (vit == g_views.end()) return atlas::ParseError("view not found");
      for (auto& entry : g_sessions) {
        auto sit = std::find_if(entry.second.tabs.begin(), entry.second.tabs.end(),
                                [vid](const auto& pair) { return pair.second.view_id == vid; });
        if (sit != entry.second.tabs.end()) sit->second.view_id = 0;
      }
      DestroyWindow(vit->second.hw);
      g_views.erase(vit);
      return atlas::ParseOk("destroyed");
    }
    case atlas::Command::RouteMouse:
      return atlas::BuildMessage({"OK", "1"});
    case atlas::Command::RouteWheel:
      return atlas::BuildMessage({"OK", "1"});
    case atlas::Command::RouteKeyboard:
      return atlas::BuildMessage({"OK", "1"});
    case atlas::Command::SetFocus: {
      if (parts.size() < 3) return atlas::ParseError("missing focus args");
      AtlasWindowId vid = 0;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad args");
      bool focus = parts[2] == "1" || parts[2] == "true";
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      if (focus) SetFocus(it->second.hw);
      return atlas::ParseOk(focus ? "focused" : "unfocused");
    }
    case atlas::Command::SetDownloadDirectory:
      if (parts.size() >= 2) {
        g_download_dir = atlas::UnescapeField(parts[1]);
      }
      if (parts.size() >= 3) {
        {
          std::lock_guard<std::mutex> lock(g_download_mutex);
          g_download_event_pipe_name = atlas::UnescapeField(parts[2]);
          CloseDownloadPipe();
          OpenDownloadPipe();
        }
      }
      return atlas::ParseOk("downloads_ready");
    case atlas::Command::OnDownloadStateChanged:
      if (parts.size() < 2) return atlas::ParseOk("ack");
      if (!g_download_event_pipe && !OpenDownloadPipe()) {
        return atlas::ParseOk("ack_unconfigured");
      }
      return atlas::ParseOk("ack");
    case atlas::Command::Ping:
      return atlas::ParseOk("pong");
    case atlas::Command::Unknown:
    default:
      return atlas::ParseError("unknown command");
  }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
  g_instance = hInstance;
  SetProcessDPIAware();

  atlas::IpcServer server;
  server.start(atlas::kPipeName, [](const std::string& req) { return HandleCommand(req); });

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  server.stop();
  return 0;
}
