#include <Windows.h>
#include <bcrypt.h>
#include <CommCtrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <string>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <fstream>
#include <vector>
#include <memory>
#include <winhttp.h>

#include "common/ipc_server.h"
#include "common/protocol.h"
#include "host/browser_backend.h"

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
  std::string cdp_target_id;
  std::string cdp_target_ws_url;
  bool accessibility_enabled = false;
};

struct ViewState {
  AtlasWindowId view_id = 0;
  AtlasWindowId tab_id = 0;
  HWND hw = nullptr;
  HWND host_parent = nullptr;
  RECT rect{0, 0, 0, 0};
  atlas::DpiScale dpi{1.0f, 1.0f};
  atlas::host::ViewBinding backend;
  bool visible = true;
  bool focused = false;
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
constexpr uint16_t kDefaultRemoteDebugPort = 9222;
std::string g_remote_debug_host = "127.0.0.1";
uint16_t g_remote_debug_port = kDefaultRemoteDebugPort;
HANDLE g_chrome_process = nullptr;
std::string g_chrome_user_data_dir;
std::string g_chrome_binary_path;
atlas::host::BackendKind g_backend_kind = atlas::host::BackendKind::Owl;
bool g_capture_dom_snapshot = false;
std::string g_owl_host_endpoint;
std::unique_ptr<atlas::host::BrowserBackend> g_browser_backend;
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

std::string EscapeJson(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '\"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string GetUtcIso8601Now() {
  SYSTEMTIME utc{};
  GetSystemTime(&utc);
  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(4) << utc.wYear << '-'
      << std::setw(2) << utc.wMonth << '-' << std::setw(2) << utc.wDay << 'T'
      << std::setw(2) << utc.wHour << ':' << std::setw(2) << utc.wMinute << ':'
      << std::setw(2) << utc.wSecond << 'Z';
  return oss.str();
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return L"";
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
  std::wstring out(needed > 0 ? (size_t)needed : 0, 0);
  if (needed > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), needed);
  return out;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr,
                                       nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<size_t>(needed), 0);
  WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), out.data(), needed, nullptr, nullptr);
  return out;
}

std::string EncodeUrlParam(const std::string& input) {
  std::ostringstream out;
  const char* kHex = "0123456789ABCDEF";
  for (unsigned char c : input) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') {
      out << c;
    } else if (c == ' ') {
      out << "%20";
    } else {
      out << '%' << kHex[(c >> 4) & 0xF] << kHex[c & 0xF];
    }
  }
  return out.str();
}

bool ParseHttpUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  if (url.rfind("http://", 0) != 0) return false;
  std::string rest = url.substr(7);
  auto slash = rest.find('/');
  std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
  path = slash == std::string::npos ? "/" : rest.substr(slash);

  auto colon = host_port.find(':');
  host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
  port = kDefaultRemoteDebugPort;
  if (colon != std::string::npos) {
    try {
      int parsed = std::stoi(host_port.substr(colon + 1));
      if (parsed > 0 && parsed < 65535) port = static_cast<uint16_t>(parsed);
    } catch (...) {
      return false;
    }
  }
  return true;
}

bool HttpGetText(const std::string& url, std::string& response) {
  std::string host, path;
  uint16_t port = 0;
  if (!ParseHttpUrl(url, host, port, path)) return false;
  HINTERNET session = WinHttpOpen(L"atlas-host-cdp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;

  HINTERNET connection = WinHttpConnect(session, Utf8ToWide(host).c_str(), port, 0);
  if (!connection) {
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(connection, L"GET", Utf8ToWide(path).c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!request) {
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
  if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD status = 0;
  DWORD status_len = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                      &status, &status_len, nullptr);
  if (status / 100 != 2) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  response.clear();
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
    response.append(chunk.data(), chunk.data() + read);
  }
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return true;
}

bool HttpPostJson(const std::string& url, const std::string& body, std::string& response) {
  std::string host, path;
  uint16_t port = 0;
  if (!ParseHttpUrl(url, host, port, path)) return false;
  HINTERNET session = WinHttpOpen(L"atlas-host-cdp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;

  HINTERNET connection = WinHttpConnect(session, Utf8ToWide(host).c_str(), port, 0);
  if (!connection) {
    WinHttpCloseHandle(session);
    return false;
  }

  std::wstring path_w = Utf8ToWide(path);
  HINTERNET request = WinHttpOpenRequest(connection, L"POST", path_w.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         0);
  if (!request) {
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  const wchar_t* headers = L"Content-Type: application/json\r\n";
  BOOL sent = WinHttpSendRequest(request, headers, (DWORD)-1, (PVOID)body.data(), body.size(), body.size(),
                                0);
  if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  response.clear();
  DWORD status = 0;
  DWORD status_len = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                      &status, &status_len, nullptr);
  if (status / 100 != 2) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
    response.append(chunk.data(), chunk.data() + read);
  }
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return true;
}

bool ParseWsUrl(const std::string& url, std::wstring& host, uint16_t& port, std::wstring& path) {
  if (url.rfind("ws://", 0) != 0) return false;
  std::string rest = url.substr(5);
  auto slash = rest.find('/');
  if (slash == std::string::npos) return false;
  std::string host_port = rest.substr(0, slash);
  std::string raw_path = rest.substr(slash);
  auto colon = host_port.find(':');
  host = Utf8ToWide(colon == std::string::npos ? host_port : host_port.substr(0, colon));
  if (colon != std::string::npos) {
    try {
      int parsed = std::stoi(host_port.substr(colon + 1));
      if (parsed > 0 && parsed < 65535) port = static_cast<uint16_t>(parsed);
      else return false;
    } catch (...) {
      return false;
    }
  } else {
    port = g_remote_debug_port;
  }
  path = Utf8ToWide(raw_path);
  return true;
}

bool WebSocketSendAndReceive(const std::string& ws_url, const std::string& request_json,
                            uint32_t command_id, std::string& response) {
  std::wstring host;
  uint16_t port = 0;
  std::wstring path;
  if (!ParseWsUrl(ws_url, host, port, path)) return false;

  HINTERNET session = WinHttpOpen(L"atlas-host-cdp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;

  HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
  if (!connection) {
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  if (!request) {
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  BOOL upgraded = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
  if (!upgraded) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request, NULL);
  if (!ws) {
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  BOOL ok = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 const_cast<PVOID>(static_cast<const void*>(request_json.data())),
                                 static_cast<DWORD>(request_json.size()));
  if (!ok) {
    WinHttpCloseHandle(ws);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return false;
  }

  auto extract_complete_json = [](const std::string& data, size_t& consumed, std::string& message) {
    size_t pos = 0;
    while (pos < data.size() && isspace(static_cast<unsigned char>(data[pos]))) ++pos;
    if (pos >= data.size() || data[pos] != '{') return false;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    size_t start = pos;
    for (size_t i = pos; i < data.size(); ++i) {
      char c = data[i];
      if (escape) {
        escape = false;
        continue;
      }
      if (in_string) {
        if (c == '\\') {
          escape = true;
          continue;
        }
        if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
        continue;
      }
      if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        if (depth == 0) return false;
        --depth;
        if (depth == 0) {
          consumed = i + 1;
          message.assign(data, start, consumed - start);
          return true;
        }
      }
    }
    return false;
  };

  auto parse_response_id = [](const std::string& message, uint32_t& id) {
    size_t pos = 0;
    while (pos < message.size() && isspace(static_cast<unsigned char>(message[pos]))) ++pos;
    if (pos >= message.size() || message[pos] != '{') return false;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = pos; i < message.size(); ++i) {
      char c = message[i];
      if (escape) {
        escape = false;
        continue;
      }
      if (in_string) {
        if (c == '\\') {
          escape = true;
          continue;
        }
        if (c == '"') in_string = false;
        continue;
      }
      if (c == '"') {
        size_t key_pos = i + 1;
        std::string key;
        bool key_escape = false;
        size_t j = key_pos;
        for (; j < message.size(); ++j) {
          char kc = message[j];
          if (key_escape) {
            key_escape = false;
            continue;
          }
          if (kc == '\\') {
            key_escape = true;
            continue;
          }
          if (kc == '"') break;
          key.push_back(kc);
        }
        if (j >= message.size()) return false;
        size_t colon = j + 1;
        while (colon < message.size() && isspace(static_cast<unsigned char>(message[colon]))) ++colon;
        if (colon >= message.size() || message[colon] != ':') {
          i = colon;
          continue;
        }
        if (depth == 1 && key == "id") {
          size_t value = colon + 1;
          while (value < message.size() &&
                 isspace(static_cast<unsigned char>(message[value]))) ++value;
          if (value >= message.size() || message[value] == '"') return false;
          size_t end = value;
          while (end < message.size() && isdigit(static_cast<unsigned char>(message[end]))) ++end;
          if (end == value) return false;
          try {
            unsigned long long parsed = std::stoull(message.substr(value, end - value));
            if (parsed > UINT32_MAX) return false;
            id = static_cast<uint32_t>(parsed);
            return true;
          } catch (...) {
            return false;
          }
        }
        i = colon;
        continue;
      }
      if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        --depth;
      }
      if (depth == 0) {
        break;
      }
    }
    return false;
  };

  response.clear();
  std::string incoming;
  for (int frame_count = 0; frame_count < 64; ++frame_count) {
    std::vector<unsigned char> buffer(32768);
    DWORD bytes = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    DWORD rv = WinHttpWebSocketReceive(ws, buffer.data(), (DWORD)buffer.size(), &bytes, &type);
    if (rv != ERROR_SUCCESS) break;
    if (bytes > 0) {
      if (type != WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        incoming.append(reinterpret_cast<const char*>(buffer.data()),
                        reinterpret_cast<const char*>(buffer.data()) + bytes);
        while (true) {
          size_t consumed = 0;
          std::string message;
          if (!extract_complete_json(incoming, consumed, message)) break;
          incoming.erase(0, consumed);
          uint32_t message_id = 0;
          if (parse_response_id(message, message_id) && message_id == command_id) {
            response = std::move(message);
            WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(ws);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return true;
          }
        }
      }
    }
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
  }

  WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
  WinHttpCloseHandle(ws);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return !response.empty();
}

bool DecodeBase64(const std::string& input, std::vector<unsigned char>& output) {
  static const int decode[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1,
      -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
      21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
      37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};
  int val = 0;
  int bits = 0;
  for (unsigned char c : input) {
    if (c == '=') break;
    int d = (c < 128) ? decode[c] : -1;
    if (d < 0) continue;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
    }
  }
  return !output.empty();
}

bool SkipJsonWhitespace(const std::string& json, size_t& pos) {
  while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  return pos < json.size();
}

bool DecodeJsonString(const std::string& json, size_t value_pos, std::string& out) {
  if (value_pos >= json.size() || json[value_pos] != '"') return false;
  out.clear();
  bool escape = false;
  for (size_t i = value_pos + 1; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      if (c == 'u' && i + 4 < json.size()) {
        int code = 0;
        for (int j = 0; j < 4; ++j) {
          char h = json[i + 1 + j];
          int digit = 0;
          if ('0' <= h && h <= '9')
            digit = h - '0';
          else if ('a' <= h && h <= 'f')
            digit = h - 'a' + 10;
          else if ('A' <= h && h <= 'F')
            digit = h - 'A' + 10;
          else
            return false;
          code = (code << 4) | digit;
        }
        if (code < 0x20 || code > 0x7E) {
          out.push_back('?');
        } else {
          out.push_back(static_cast<char>(code));
        }
        i += 4;
      } else if (c == '"') {
        out.push_back('"');
      } else if (c == '\\') {
        out.push_back('\\');
      } else if (c == '/') {
        out.push_back('/');
      } else if (c == 'b') {
        out.push_back('\b');
      } else if (c == 'f') {
        out.push_back('\f');
      } else if (c == 'n') {
        out.push_back('\n');
      } else if (c == 'r') {
        out.push_back('\r');
      } else if (c == 't') {
        out.push_back('\t');
      } else {
        out.push_back(c);
      }
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') return true;
    out.push_back(c);
  }
  return false;
}

bool FindTopLevelFieldValuePos(const std::string& json, const std::string& key, size_t& value_pos) {
  size_t pos = 0;
  if (!SkipJsonWhitespace(json, pos) || pos >= json.size() || json[pos] != '{') return false;
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  ++pos;
  ++depth;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_string) {
      if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      size_t key_pos = pos + 1;
      std::string found_key;
      bool key_escape = false;
      size_t j = key_pos;
      for (; j < json.size(); ++j) {
        char kc = json[j];
        if (key_escape) {
          key_escape = false;
          continue;
        }
        if (kc == '\\') {
          key_escape = true;
          continue;
        }
        if (kc == '"') break;
        found_key.push_back(kc);
      }
      if (j >= json.size()) return false;
      size_t colon = j + 1;
      if (!SkipJsonWhitespace(json, colon) || colon >= json.size() || json[colon] != ':') {
        pos = colon;
        continue;
      }
      size_t value = colon + 1;
      SkipJsonWhitespace(json, value);
      if (depth == 1 && found_key == key) {
        value_pos = value;
        return true;
      }
      pos = value;
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (depth == 0) return false;
    }
  }
  return false;
}

bool ExtractJsonNumericField(const std::string& json, const std::string& key, uint64_t& out) {
  size_t value_pos = 0;
  if (!FindTopLevelFieldValuePos(json, key, value_pos) || value_pos >= json.size()) return false;
  size_t end = value_pos;
  if (end >= json.size() || !isdigit(static_cast<unsigned char>(json[end]))) return false;
  while (end < json.size() && isdigit(static_cast<unsigned char>(json[end]))) ++end;
  try {
    out = std::stoull(json.substr(value_pos, end - value_pos));
    return true;
  } catch (...) {
    return false;
  }
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
  size_t value_pos = 0;
  if (!FindTopLevelFieldValuePos(json, key, value_pos)) return "";
  std::string out;
  if (!DecodeJsonString(json, value_pos, out)) return "";
  return out;
}

bool ExtractJsonValueRaw(const std::string& json, const std::string& key, std::string& out,
                        char open_char = '{', char close_char = '}') {
  size_t value_pos = 0;
  if (!FindTopLevelFieldValuePos(json, key, value_pos) || value_pos >= json.size() ||
      json[value_pos] != open_char) {
    return false;
  }
  int depth = 0;
  bool escape = false;
  bool in_string = false;
  size_t pos = value_pos;
  const size_t start = value_pos;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (escape) {
      escape = false;
      continue;
    }
    if (c == '\\' && in_string) {
      escape = true;
      continue;
    }
    if (c == '\"') in_string = !in_string;
    if (in_string) continue;
    if (c == open_char) ++depth;
    if (c == close_char) {
      --depth;
      if (depth == 0) {
        out.assign(json.data() + start, json.data() + pos + 1);
        return true;
      }
    }
  }
  return false;
}

bool ExtractJsonArray(const std::string& json, const std::string& key, std::string& out) {
  return ExtractJsonValueRaw(json, key, out, '[', ']');
}

std::string ExtractFieldStringValue(const std::string& json, const std::string& key) {
  return ExtractJsonStringField(json, key);
}

bool ExtractTargetWs(const std::string& list_json, const std::string& target_id, std::string& ws_url) {
  const std::string target_key = "\"id\":\"" + target_id + "\"";
  auto target_pos = list_json.find(target_key);
  if (target_pos == std::string::npos) return false;
  auto ws_key_pos = list_json.find("\"webSocketDebuggerUrl\"", target_pos);
  if (ws_key_pos == std::string::npos) return false;
  ws_url = ExtractJsonStringField(list_json.substr(ws_key_pos), "webSocketDebuggerUrl");
  return !ws_url.empty();
}

bool ExtractTargetFromJsonList(const std::string& list_json, const std::string& target_id,
                              std::string& ws_url) {
  return ExtractTargetWs(list_json, target_id, ws_url);
}

std::string TrimSurroundingQuotes(std::string value) {
  if (value.size() >= 2) {
    if ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\'')) {
      value.erase(0, 1);
      value.pop_back();
    }
  }
  return value;
}

bool ResolveTargetId(const std::string& response, std::string& target_id) {
  uint64_t id = 0;
  if (ExtractJsonNumericField(response, "id", id)) {
    target_id = std::to_string(id);
    return true;
  }
  target_id = ExtractJsonStringField(response, "id");
  return !target_id.empty();
}

bool IsTargetUsableFromList(const std::string& list_json, const std::string& target_id,
                           std::string& ws_url) {
  ws_url.clear();
  return ExtractTargetFromJsonList(list_json, target_id, ws_url);
}

std::string DevToolsBaseUrl() {
  return "http://" + g_remote_debug_host + ":" + std::to_string(g_remote_debug_port);
}

bool EnsureChromeBinary(std::string& chrome_path) {
  if (!g_chrome_binary_path.empty()) {
    chrome_path = g_chrome_binary_path;
    return true;
  }
  const char* candidates[] = {
      "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
      "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  };
  for (const char* candidate : candidates) {
    if (candidate && GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
      chrome_path = candidate;
      return true;
    }
  }
  return false;
}

bool IsProcessAlive(HANDLE process) {
  if (!process) return false;
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process, &exit_code)) return false;
  return exit_code == STILL_ACTIVE;
}

bool EnsureChromeProcess() {
  if (g_chrome_process) {
    if (IsProcessAlive(g_chrome_process)) return true;
    CloseHandle(g_chrome_process);
    g_chrome_process = nullptr;
  }
  std::string chrome_path;
  if (!EnsureChromeBinary(chrome_path)) return false;
  char temp_path[MAX_PATH] = {};
  if (!GetTempPathA(MAX_PATH, temp_path)) return false;
  g_chrome_user_data_dir = std::string(temp_path) + "atlas-owl-chrome";
  CreateDirectoryA(g_chrome_user_data_dir.c_str(), nullptr);

  std::string command = "\"" + chrome_path + "\" --headless=new --remote-debugging-port=" +
                        std::to_string(g_remote_debug_port) + " --user-data-dir=\"" +
                        g_chrome_user_data_dir +
                        "\" --no-first-run --no-default-browser-check --disable-background-networking";
  std::wstring cmd = Utf8ToWide(command);

  STARTUPINFOW si{};
  PROCESS_INFORMATION pi{};
  si.cb = sizeof(si);
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                     &pi)) {
    return false;
  }
  g_chrome_process = pi.hProcess;
  if (pi.hThread) CloseHandle(pi.hThread);
  std::string version;
  const std::string version_url = DevToolsBaseUrl() + "/json/version";
  for (int i = 0; i < 100; ++i) {
    if (HttpGetText(version_url, version)) return true;
    Sleep(75);
  }
  return false;
}

bool EnsureTargetForTab(TabState& tab, const std::string& url) {
  if (!EnsureChromeProcess()) return false;
  const std::string base = DevToolsBaseUrl();
  const std::string target_url = url.empty() ? std::string("about:blank") : url;
  if (!tab.cdp_target_id.empty()) {
    std::string list_json;
    std::string ws_url;
    if (HttpGetText(base + "/json/list", list_json) && IsTargetUsableFromList(list_json, tab.cdp_target_id, ws_url)) {
      tab.cdp_target_ws_url = ws_url;
      return true;
    }
    tab.cdp_target_id.clear();
    tab.cdp_target_ws_url.clear();
  }

  if (target_url.empty()) return false;
  std::string response;
  const std::string create_url = base + "/json/new?url=" + EncodeUrlParam(target_url);
  if (!HttpGetText(create_url, response)) return false;
  std::string target;
  if (!ResolveTargetId(response, target)) return false;
  target = TrimSurroundingQuotes(target);
  if (target.empty()) return false;
  tab.cdp_target_id = target;

  std::string list_json;
  std::string ws_url;
  for (int i = 0; i < 40; ++i) {
    if (HttpGetText(base + "/json/list", list_json) && ExtractTargetFromJsonList(list_json, target, ws_url)) {
      tab.cdp_target_ws_url = ws_url;
      return true;
    }
    Sleep(50);
  }
  if (ws_url.empty()) {
    std::string fallback = "ws://" + g_remote_debug_host + ":" + std::to_string(g_remote_debug_port) +
                           "/devtools/page/" + target;
    tab.cdp_target_ws_url = fallback;
  }
  return true;
}

bool SendCdpCommand(TabState& tab, const std::string& method, const std::string& params,
                    std::string& response) {
  if (tab.cdp_target_ws_url.empty()) return false;
  static uint32_t command_id = 1;
  const uint32_t id = command_id++;
  std::ostringstream req;
  req << "{\"id\":" << id << ",\"method\":\"" << method << "\"";
  if (!params.empty()) req << ",\"params\":" << params;
  req << "}";
  if (!WebSocketSendAndReceive(tab.cdp_target_ws_url, req.str(), id, response)) return false;
  std::string response_id;
  if (!ResolveTargetId(response, response_id)) return false;
  if (response_id != std::to_string(id)) return false;
  return true;
}

bool CaptureTabWithCdp(TabState& tab, const ViewState* view, std::string& payload) {
  if (!EnsureTargetForTab(tab, tab.url.empty() ? "about:blank" : tab.url)) return false;

  std::string response;
  if (!SendCdpCommand(tab, "Accessibility.enable", "{}", response)) {
    // Non-fatal: continue to capture even when accessibility enable fails.
  } else {
    tab.accessibility_enabled = true;
  }
  if (!SendCdpCommand(tab, "Page.bringToFront", "{}", response)) {
    return false;
  }

  if (!SendCdpCommand(tab, "Page.captureScreenshot",
                      "{\"format\":\"png\",\"fromSurface\":true,\"captureBeyondViewport\":true}",
                      response)) {
    return false;
  }
  std::string screenshot_data = ExtractJsonStringField(response, "data");
  if (screenshot_data.empty()) return false;
  std::vector<unsigned char> png_bytes;
  if (!DecodeBase64(screenshot_data, png_bytes)) return false;

  char temp_path[MAX_PATH] = {};
  if (!GetTempPathA(MAX_PATH, temp_path)) temp_path[0] = 0;
  char file_name[MAX_PATH] = {};
  GetTempFileNameA(temp_path, "atc", 0, file_name);
  std::string screenshot_file = std::string(file_name) + ".png";
  if (!MoveFileA(file_name, screenshot_file.c_str())) {
    screenshot_file = file_name;
  }
  std::ofstream png_out(screenshot_file, std::ios::binary);
  if (!png_out) return false;
  png_out.write(reinterpret_cast<const char*>(png_bytes.data()), (std::streamsize)png_bytes.size());
  png_out.close();

  std::string ax_json;
  bool accessibility_enabled = false;
  if (!SendCdpCommand(tab, "Accessibility.getFullAXTree", "{}", response)) {
    response.clear();
  } else {
    ExtractJsonArray(response, "nodes", ax_json);
    accessibility_enabled = !ax_json.empty();
  }
  if (tab.accessibility_enabled) {
    SendCdpCommand(tab, "Accessibility.disable", "{}", response);
    tab.accessibility_enabled = false;
  }

  int width = 0;
  int height = 0;
  float scale = 1.0f;
  if (view) {
    int measured_width = view->rect.right - view->rect.left;
    int measured_height = view->rect.bottom - view->rect.top;
    width = measured_width > 0 ? measured_width : 0;
    height = measured_height > 0 ? measured_height : 0;
    scale = view->dpi.x > 1.0f ? view->dpi.x : 1.0f;
  }
  if (width == 0) width = 1280;
  if (height == 0) height = 800;

  std::string title = tab.url.empty() ? "about:blank" : tab.url;
  std::string session_id = "profile/" + std::to_string(tab.session_id) + "/window/1/tab/" +
                           std::to_string(tab.tab_id) + "/view/" + std::to_string(tab.view_id);

  std::ostringstream out;
  out << "{";
  out << "\"schema_version\":1,";
  out << "\"session_id\":\"" << EscapeJson(session_id) << "\",";
  out << "\"timestamp_utc\":\"" << GetUtcIso8601Now() << "\",";
  out << "\"url\":\"" << EscapeJson(tab.url.empty() ? std::string("about:blank") : tab.url) << "\",";
  out << "\"title\":\"" << EscapeJson(title) << "\",";
  out << "\"viewport_css\":{\"width\":" << width << ",\"height\":" << height << "},";
  out << "\"device_scale_factor\":" << scale << ",";
  out << "\"screenshot\":{\"mime\":\"image/png\",\"sha256\":\"\",\"temp_file\":\""
      << EscapeJson(screenshot_file) << "\"},";
  out << "\"ax\":{\"source\":\"cdp.Accessibility.getFullAXTree\",\"enabled\":"
      << (accessibility_enabled ? "true" : "false") << ",\"nodes\":"
      << (ax_json.empty() ? "[]" : ax_json) << "},";
  out << "\"selected_text\":\"\",";
  out << "\"focused_element\":{\"role\":\"\",\"name\":\"\"}";
  out << "}";
  payload = out.str();
  return true;
}

std::string BuildCapturePayload(TabState& tab, const ViewState* view) {
  std::string payload;
  if (CaptureTabWithCdp(tab, view, payload)) {
    return payload;
  }
  return {};
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

bool ParseHostArgs() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  constexpr wchar_t kRemoteDebuggingPortPrefix[] = L"--remote_debugging_port=";
  constexpr size_t kPrefixLen = _countof(kRemoteDebuggingPortPrefix) - 1;
  constexpr wchar_t kChromeBinaryPrefix[] = L"--chrome_binary=";
  constexpr size_t kBinaryPrefixLen = _countof(kChromeBinaryPrefix) - 1;
  constexpr wchar_t kBackendPrefix[] = L"--backend=";
  constexpr size_t kBackendPrefixLen = _countof(kBackendPrefix) - 1;
  constexpr wchar_t kCaptureDomPrefix[] = L"--capture_dom_snapshot=";
  constexpr size_t kCaptureDomPrefixLen = _countof(kCaptureDomPrefix) - 1;
  constexpr wchar_t kOwlHostEndpointPrefix[] = L"--owl_host_endpoint=";
  constexpr size_t kOwlHostEndpointPrefixLen = _countof(kOwlHostEndpointPrefix) - 1;
  for (int i = 1; i < argc; ++i) {
    std::wstring arg(argv[i]);
    if (arg.rfind(kRemoteDebuggingPortPrefix, 0) == 0) {
      try {
        const std::string value = WideToUtf8(arg.substr(kPrefixLen));
        int parsed = std::stoi(value);
        if (parsed > 0 && parsed <= 65535) g_remote_debug_port = static_cast<uint16_t>(parsed);
      } catch (...) {
      }
      continue;
    }
    if (arg.rfind(kChromeBinaryPrefix, 0) == 0) {
      g_chrome_binary_path = WideToUtf8(arg.substr(kBinaryPrefixLen));
      g_chrome_binary_path = TrimSurroundingQuotes(g_chrome_binary_path);
      continue;
    }
    if (arg.rfind(kBackendPrefix, 0) == 0) {
      g_backend_kind = atlas::host::ParseBackendKind(WideToUtf8(arg.substr(kBackendPrefixLen)));
      continue;
    }
    if (arg.rfind(kCaptureDomPrefix, 0) == 0) {
      std::string value = WideToUtf8(arg.substr(kCaptureDomPrefixLen));
      g_capture_dom_snapshot = value == "1" || value == "true";
      continue;
    }
    if (arg.rfind(kOwlHostEndpointPrefix, 0) == 0) {
      g_owl_host_endpoint = WideToUtf8(arg.substr(kOwlHostEndpointPrefixLen));
    }
  }
  LocalFree(argv);
  return true;
}

std::string DevToolsBaseUrl(uint16_t port) {
  return "http://" + g_remote_debug_host + ":" + std::to_string(port);
}

bool WaitForDevToolsReady(uint16_t port) {
  std::string response;
  const std::string version_url = DevToolsBaseUrl(port) + "/json/version";
  for (int i = 0; i < 100; ++i) {
    if (HttpGetText(version_url, response)) return true;
    Sleep(50);
  }
  return false;
}

bool ExtractFirstTargetFromJsonList(const std::string& list_json, std::string& target_id,
                                    std::string& ws_url) {
  auto ws_pos = list_json.find("\"webSocketDebuggerUrl\"");
  if (ws_pos == std::string::npos) return false;
  auto id_pos = list_json.rfind("\"id\"", ws_pos);
  if (id_pos == std::string::npos) return false;
  target_id = ExtractJsonStringField(list_json.substr(id_pos), "id");
  ws_url = ExtractJsonStringField(list_json.substr(ws_pos), "webSocketDebuggerUrl");
  return !target_id.empty() && !ws_url.empty();
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  BCRYPT_ALG_HANDLE provider = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD hash_object_len = 0;
  DWORD digest_len = 0;
  DWORD result_len = 0;
  if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  if (BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_object_len),
                        sizeof(hash_object_len), &result_len, 0) != 0 ||
      BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digest_len),
                        sizeof(digest_len), &result_len, 0) != 0) {
    BCryptCloseAlgorithmProvider(provider, 0);
    return {};
  }
  std::vector<unsigned char> hash_object(hash_object_len);
  std::vector<unsigned char> digest(digest_len);
  if (BCryptCreateHash(provider, &hash, hash_object.data(), hash_object_len, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(provider, 0);
    return {};
  }
  if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) !=
          0 ||
      BCryptFinishHash(hash, digest.data(), digest_len, 0) != 0) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(provider, 0);
    return {};
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(provider, 0);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

BOOL CALLBACK EnumChromeWindowProc(HWND hwnd, LPARAM lparam) {
  auto* state = reinterpret_cast<std::pair<DWORD, HWND*>*>(lparam);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != state->first) return TRUE;
  if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  wchar_t class_name[256] = {};
  GetClassNameW(hwnd, class_name, 256);
  if (std::wstring(class_name).rfind(L"Chrome_WidgetWin", 0) != 0) return TRUE;
  *state->second = hwnd;
  return FALSE;
}

HWND WaitForChromeWindow(HANDLE process) {
  if (!process) return nullptr;
  const DWORD pid = GetProcessId(process);
  for (int i = 0; i < 200; ++i) {
    HWND found = nullptr;
    std::pair<DWORD, HWND*> state(pid, &found);
    EnumWindows(EnumChromeWindowProc, reinterpret_cast<LPARAM>(&state));
    if (found) return found;
    Sleep(50);
  }
  return nullptr;
}

bool IsBlockedAutomationShortcut(const atlas::host::KeyDispatchParams& params, std::string& error) {
  const bool ctrl = (params.modifiers & 2U) != 0;
  const bool alt = (params.modifiers & 1U) != 0;
  const uint32_t key = static_cast<uint32_t>(params.wparam);
  if (ctrl && (key == 'L' || key == 'T' || key == 'W')) {
    error = "blocked automation browser shortcut";
    return true;
  }
  if (alt && key == 'F') {
    error = "blocked automation browser shortcut";
    return true;
  }
  return false;
}

class CdpBackendBase : public atlas::host::BrowserBackend {
 protected:
  explicit CdpBackendBase(const atlas::host::BackendConfig& config) : config_(config) {}

  bool SendCdpCommand(atlas::host::ViewBinding& binding, const std::string& method,
                      const std::string& params, std::string& response, std::string& error) {
    if (binding.cdp_target_ws_url.empty()) {
      error = "devtools target unavailable";
      return false;
    }
    static std::atomic<uint32_t> next_command_id{1};
    const uint32_t id = next_command_id.fetch_add(1);
    std::ostringstream req;
    req << "{\"id\":" << id << ",\"method\":\"" << method << "\"";
    if (!params.empty()) req << ",\"params\":" << params;
    req << "}";
    if (!WebSocketSendAndReceive(binding.cdp_target_ws_url, req.str(), id, response)) {
      error = "devtools command failed";
      return false;
    }
    uint64_t response_id = 0;
    if (!ExtractJsonNumericField(response, "id", response_id) || response_id != id) {
      error = "devtools response id mismatch";
      return false;
    }
    return true;
  }

  bool CaptureContextWithCdp(atlas::host::ViewBinding& binding,
                             const atlas::host::CaptureRequest& request, std::string& payload,
                             std::string& error) {
    std::string response;
    if (!SendCdpCommand(binding, "Accessibility.enable", "{}", response, error)) {
      response.clear();
      error.clear();
    } else {
      binding.accessibility_enabled = true;
    }
    if (!SendCdpCommand(binding, "Page.bringToFront", "{}", response, error)) return false;
    if (!SendCdpCommand(
            binding, "Page.captureScreenshot",
            "{\"format\":\"png\",\"fromSurface\":true,\"captureBeyondViewport\":true}", response,
            error)) {
      return false;
    }

    std::string screenshot_data = ExtractJsonStringField(response, "data");
    if (screenshot_data.empty()) {
      error = "capture screenshot data missing";
      return false;
    }
    std::vector<unsigned char> png_bytes;
    if (!DecodeBase64(screenshot_data, png_bytes)) {
      error = "capture screenshot decode failed";
      return false;
    }

    char temp_path[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, temp_path)) temp_path[0] = 0;
    char temp_file[MAX_PATH] = {};
    GetTempFileNameA(temp_path, "atc", 0, temp_file);
    std::string screenshot_file = std::string(temp_file) + ".png";
    if (!MoveFileA(temp_file, screenshot_file.c_str())) screenshot_file = temp_file;
    std::ofstream png_out(screenshot_file, std::ios::binary);
    if (!png_out) {
      error = "capture screenshot file open failed";
      return false;
    }
    png_out.write(reinterpret_cast<const char*>(png_bytes.data()),
                  static_cast<std::streamsize>(png_bytes.size()));
    png_out.close();
    const std::string screenshot_sha = Sha256Hex(png_bytes);

    std::string ax_json = "[]";
    if (SendCdpCommand(binding, "Accessibility.getFullAXTree", "{}", response, error)) {
      std::string nodes;
      if (ExtractJsonArray(response, "nodes", nodes) && !nodes.empty()) ax_json = nodes;
    } else {
      error.clear();
    }
    if (binding.accessibility_enabled) {
      std::string disable_response;
      std::string disable_error;
      SendCdpCommand(binding, "Accessibility.disable", "{}", disable_response, disable_error);
      binding.accessibility_enabled = false;
    }

    std::string dom_json = "{}";
    if (config_.capture_dom_snapshot) {
      std::string dom_response;
      if (SendCdpCommand(
              binding, "DOMSnapshot.captureSnapshot",
              "{\"computedStyles\":[],\"includeDOMRects\":true,\"includePaintOrder\":true}",
              dom_response, error)) {
        std::string raw_result;
        if (ExtractJsonValueRaw(dom_response, "result", raw_result)) dom_json = raw_result;
      } else {
        error.clear();
      }
    }

    int width = request.rect.right - request.rect.left;
    int height = request.rect.bottom - request.rect.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 800;
    const float scale = request.dpi.x > 1.0f ? request.dpi.x : 1.0f;

    std::ostringstream out;
    out << "{";
    out << "\"schema_version\":1,";
    out << "\"session_id\":\"profile/" << request.session_id << "/window/1/tab/" << request.tab_id
        << "/view/" << request.view_id << "\",";
    out << "\"timestamp_utc\":\"" << GetUtcIso8601Now() << "\",";
    out << "\"url\":\""
        << EscapeJson(request.url.empty() ? std::string("about:blank") : request.url) << "\",";
    out << "\"title\":\""
        << EscapeJson(request.title.empty() ? request.url : request.title) << "\",";
    out << "\"viewport_css\":{\"width\":" << width << ",\"height\":" << height << "},";
    out << "\"device_scale_factor\":" << scale << ",";
    out << "\"screenshot\":{\"mime\":\"image/png\",\"sha256\":\"" << screenshot_sha
        << "\",\"temp_file\":\"" << EscapeJson(screenshot_file) << "\"},";
    out << "\"ax\":{\"source\":\"cdp.Accessibility.getFullAXTree\",\"nodes\":" << ax_json << "},";
    if (config_.capture_dom_snapshot) {
      out << "\"dom_snapshot\":{\"source\":\"cdp.DOMSnapshot.captureSnapshot\",\"result\":"
          << dom_json << "},";
    }
    out << "\"selected_text\":\"\",";
    out << "\"focused_element\":{\"role\":\"\",\"name\":\"\"}";
    out << "}";
    payload = out.str();
    return true;
  }

  bool DispatchMouseWithCdp(atlas::host::ViewBinding& binding,
                            const atlas::host::MouseDispatchParams& params,
                            std::string& error) {
    std::ostringstream req;
    req << "{";
    req << "\"type\":\"" << params.type << "\",";
    req << "\"x\":" << params.x << ",";
    req << "\"y\":" << params.y << ",";
    req << "\"button\":\"" << params.button << "\",";
    req << "\"clickCount\":" << params.click_count << ",";
    req << "\"modifiers\":" << params.modifiers;
    if (params.type == "mouseWheel") {
      req << ",\"deltaX\":" << params.delta_x << ",\"deltaY\":" << params.delta_y;
    }
    req << "}";
    std::string response;
    return SendCdpCommand(binding, "Input.dispatchMouseEvent", req.str(), response, error);
  }

  bool DispatchKeyboardWithCdp(atlas::host::ViewBinding& binding,
                               const atlas::host::KeyDispatchParams& params, std::string& error) {
    if (IsBlockedAutomationShortcut(params, error)) return false;
    const std::string type =
        (params.message == WM_KEYUP || params.message == WM_SYSKEYUP) ? "keyUp" : "rawKeyDown";
    const UINT vk = static_cast<UINT>(params.wparam);
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    std::ostringstream req;
    req << "{";
    req << "\"type\":\"" << type << "\",";
    req << "\"windowsVirtualKeyCode\":" << vk << ",";
    req << "\"nativeVirtualKeyCode\":" << vk << ",";
    req << "\"modifiers\":" << params.modifiers << ",";
    req << "\"code\":\"" << EscapeJson(std::to_string(scan)) << "\",";
    req << "\"key\":\"" << EscapeJson(std::string(1, static_cast<char>(vk))) << "\"";
    req << "}";
    std::string response;
    return SendCdpCommand(binding, "Input.dispatchKeyEvent", req.str(), response, error);
  }

  atlas::host::BackendConfig config_;
};

class HeadlessCdpBackend final : public CdpBackendBase {
 public:
  explicit HeadlessCdpBackend(const atlas::host::BackendConfig& config) : CdpBackendBase(config) {}

  atlas::host::BackendKind kind() const override {
    return atlas::host::BackendKind::HeadlessCdp;
  }

  bool CreateView(uint32_t view_id, HWND placeholder_hwnd, const RECT&, const atlas::DpiScale&,
                  atlas::host::ViewBinding& binding, std::string&) override {
    binding.kind = kind();
    binding.backend_view_id = view_id;
    binding.placeholder_hwnd = placeholder_hwnd;
    return true;
  }

  bool DestroyView(atlas::host::ViewBinding& binding, std::string&) override {
    binding = {};
    return true;
  }

  bool Navigate(atlas::host::ViewBinding& binding, const std::string& url,
                std::string& error) override {
    if (!EnsureTarget(binding, url, error)) return false;
    std::string response;
    return SendCdpCommand(binding, "Page.navigate",
                          "{\"url\":\"" + EscapeJson(url.empty() ? "about:blank" : url) + "\"}",
                          response, error);
  }

  bool ResizeView(atlas::host::ViewBinding&, const RECT&, const atlas::DpiScale&,
                  std::string&) override {
    return true;
  }

  bool SetVisible(atlas::host::ViewBinding& binding, bool visible, std::string&) override {
    binding.visible = visible;
    return true;
  }

  bool SetFocus(atlas::host::ViewBinding&, bool, std::string&) override { return true; }

  bool CaptureContext(atlas::host::ViewBinding& binding,
                      const atlas::host::CaptureRequest& request, std::string& payload,
                      std::string& error) override {
    if (!EnsureTarget(binding, request.url, error)) return false;
    return CaptureContextWithCdp(binding, request, payload, error);
  }

  bool DispatchMouse(atlas::host::ViewBinding& binding,
                     const atlas::host::MouseDispatchParams& params,
                     std::string& error) override {
    if (!EnsureTarget(binding, "about:blank", error)) return false;
    return DispatchMouseWithCdp(binding, params, error);
  }

  bool DispatchWheel(atlas::host::ViewBinding& binding,
                     const atlas::host::MouseDispatchParams& params,
                     std::string& error) override {
    if (!EnsureTarget(binding, "about:blank", error)) return false;
    return DispatchMouseWithCdp(binding, params, error);
  }

  bool DispatchKeyboard(atlas::host::ViewBinding& binding,
                        const atlas::host::KeyDispatchParams& params,
                        std::string& error) override {
    if (!EnsureTarget(binding, "about:blank", error)) return false;
    return DispatchKeyboardWithCdp(binding, params, error);
  }

  std::string GetDevToolsEndpoint(const atlas::host::ViewBinding& binding) const override {
    return binding.devtools_endpoint.empty() ? binding.cdp_target_ws_url : binding.devtools_endpoint;
  }

 private:
  bool EnsureTarget(atlas::host::ViewBinding& binding, const std::string& url,
                    std::string& error) {
    if (!EnsureChromeProcess()) {
      error = "chrome process unavailable";
      return false;
    }
    const std::string base = DevToolsBaseUrl(g_remote_debug_port);
    if (!binding.cdp_target_id.empty()) {
      std::string list_json;
      std::string ws_url;
      if (HttpGetText(base + "/json/list", list_json) &&
          ExtractTargetFromJsonList(list_json, binding.cdp_target_id, ws_url)) {
        binding.cdp_target_ws_url = ws_url;
        binding.devtools_endpoint = ws_url;
        return true;
      }
      binding.cdp_target_id.clear();
      binding.cdp_target_ws_url.clear();
      binding.devtools_endpoint.clear();
    }

    std::string response;
    const std::string create_url =
        base + "/json/new?url=" + EncodeUrlParam(url.empty() ? "about:blank" : url);
    if (!HttpGetText(create_url, response)) {
      error = "headless devtools target creation failed";
      return false;
    }
    std::string target_id;
    if (!ResolveTargetId(response, target_id)) {
      error = "headless devtools target id missing";
      return false;
    }
    binding.cdp_target_id = TrimSurroundingQuotes(target_id);
    for (int i = 0; i < 80; ++i) {
      std::string list_json;
      std::string ws_url;
      if (HttpGetText(base + "/json/list", list_json) &&
          ExtractTargetFromJsonList(list_json, binding.cdp_target_id, ws_url)) {
        binding.cdp_target_ws_url = ws_url;
        binding.devtools_endpoint = ws_url;
        return true;
      }
      Sleep(50);
    }
    binding.cdp_target_ws_url = "ws://" + g_remote_debug_host + ":" +
                                std::to_string(g_remote_debug_port) + "/devtools/page/" +
                                binding.cdp_target_id;
    binding.devtools_endpoint = binding.cdp_target_ws_url;
    return true;
  }
};

class OwlBackend final : public CdpBackendBase {
 public:
  explicit OwlBackend(const atlas::host::BackendConfig& config) : CdpBackendBase(config) {}

  atlas::host::BackendKind kind() const override { return atlas::host::BackendKind::Owl; }

  bool CreateView(uint32_t view_id, HWND placeholder_hwnd, const RECT& bounds,
                  const atlas::DpiScale& dpi, atlas::host::ViewBinding& binding,
                  std::string& error) override {
    binding.kind = kind();
    binding.backend_view_id = view_id;
    binding.placeholder_hwnd = placeholder_hwnd;
    binding.remote_debug_port = static_cast<uint16_t>(10000 + view_id);

    std::string chrome_path;
    if (!EnsureChromeBinary(chrome_path)) {
      error = "chrome binary not found";
      return false;
    }

    char temp_path[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, temp_path)) {
      error = "temp path unavailable";
      return false;
    }
    binding.user_data_dir = std::string(temp_path) + "atlas-owl-live-" + std::to_string(view_id);
    CreateDirectoryA(binding.user_data_dir.c_str(), nullptr);

    std::ostringstream command;
    command << "\"" << chrome_path << "\"";
    command << " --app=about:blank";
    command << " --remote-debugging-port=" << binding.remote_debug_port;
    command << " --user-data-dir=\"" << binding.user_data_dir << "\"";
    command << " --no-first-run --no-default-browser-check --disable-background-networking";
    command << " --force-device-scale-factor=" << std::fixed << std::setprecision(2)
            << (dpi.x > 0.0f ? dpi.x : 1.0f);
    std::wstring cmd = Utf8ToWide(command.str());

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
      error = "failed to launch live chrome";
      return false;
    }
    binding.process_handle = pi.hProcess;
    if (pi.hThread) CloseHandle(pi.hThread);
    binding.child_hwnd = WaitForChromeWindow(binding.process_handle);
    if (!binding.child_hwnd) {
      error = "live chrome window not found";
      return false;
    }
    SetParent(binding.child_hwnd, placeholder_hwnd);
    LONG_PTR style = GetWindowLongPtrW(binding.child_hwnd, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE);
    style |= WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    SetWindowLongPtrW(binding.child_hwnd, GWL_STYLE, style);
    LONG_PTR exstyle = GetWindowLongPtrW(binding.child_hwnd, GWL_EXSTYLE);
    exstyle &= ~(WS_EX_APPWINDOW | WS_EX_TOPMOST);
    SetWindowLongPtrW(binding.child_hwnd, GWL_EXSTYLE, exstyle);
    SetWindowPos(binding.child_hwnd, nullptr, 0, 0, std::max(1, bounds.right - bounds.left),
                 std::max(1, bounds.bottom - bounds.top),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    if (!WaitForDevToolsReady(binding.remote_debug_port)) {
      error = "live chrome devtools unavailable";
      return false;
    }
    return EnsureTarget(binding, error);
  }

  bool DestroyView(atlas::host::ViewBinding& binding, std::string&) override {
    if (binding.child_hwnd && IsWindow(binding.child_hwnd)) {
      PostMessageW(binding.child_hwnd, WM_CLOSE, 0, 0);
    }
    if (binding.process_handle) {
      if (WaitForSingleObject(binding.process_handle, 1500) == WAIT_TIMEOUT) {
        TerminateProcess(binding.process_handle, 0);
      }
      CloseHandle(binding.process_handle);
    }
    binding = {};
    return true;
  }

  bool Navigate(atlas::host::ViewBinding& binding, const std::string& url,
                std::string& error) override {
    if (!EnsureTarget(binding, error)) return false;
    std::string response;
    return SendCdpCommand(binding, "Page.navigate",
                          "{\"url\":\"" + EscapeJson(url.empty() ? "about:blank" : url) + "\"}",
                          response, error);
  }

  bool ResizeView(atlas::host::ViewBinding& binding, const RECT& bounds, const atlas::DpiScale&,
                  std::string&) override {
    if (binding.child_hwnd && IsWindow(binding.child_hwnd)) {
      SetWindowPos(binding.child_hwnd, nullptr, 0, 0, std::max(1, bounds.right - bounds.left),
                   std::max(1, bounds.bottom - bounds.top),
                   SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    return true;
  }

  bool SetVisible(atlas::host::ViewBinding& binding, bool visible, std::string&) override {
    binding.visible = visible;
    if (binding.child_hwnd && IsWindow(binding.child_hwnd)) {
      ShowWindow(binding.child_hwnd, visible ? SW_SHOW : SW_HIDE);
    }
    return true;
  }

  bool SetFocus(atlas::host::ViewBinding& binding, bool focused, std::string&) override {
    binding.focused = focused;
    if (focused && binding.child_hwnd && IsWindow(binding.child_hwnd)) {
      ::SetFocus(binding.child_hwnd);
    }
    return true;
  }

  bool CaptureContext(atlas::host::ViewBinding& binding,
                      const atlas::host::CaptureRequest& request, std::string& payload,
                      std::string& error) override {
    if (!EnsureTarget(binding, error)) return false;
    return CaptureContextWithCdp(binding, request, payload, error);
  }

  bool DispatchMouse(atlas::host::ViewBinding& binding,
                     const atlas::host::MouseDispatchParams& params,
                     std::string& error) override {
    if (!EnsureTarget(binding, error)) return false;
    return DispatchMouseWithCdp(binding, params, error);
  }

  bool DispatchWheel(atlas::host::ViewBinding& binding,
                     const atlas::host::MouseDispatchParams& params,
                     std::string& error) override {
    if (!EnsureTarget(binding, error)) return false;
    return DispatchMouseWithCdp(binding, params, error);
  }

  bool DispatchKeyboard(atlas::host::ViewBinding& binding,
                        const atlas::host::KeyDispatchParams& params,
                        std::string& error) override {
    if (!EnsureTarget(binding, error)) return false;
    return DispatchKeyboardWithCdp(binding, params, error);
  }

  std::string GetDevToolsEndpoint(const atlas::host::ViewBinding& binding) const override {
    return binding.devtools_endpoint.empty() ? binding.cdp_target_ws_url : binding.devtools_endpoint;
  }

 private:
  bool EnsureTarget(atlas::host::ViewBinding& binding, std::string& error) {
    const std::string base = DevToolsBaseUrl(binding.remote_debug_port);
    if (!binding.cdp_target_id.empty()) {
      std::string list_json;
      std::string ws_url;
      if (HttpGetText(base + "/json/list", list_json) &&
          ExtractTargetFromJsonList(list_json, binding.cdp_target_id, ws_url)) {
        binding.cdp_target_ws_url = ws_url;
        binding.devtools_endpoint = ws_url;
        return true;
      }
      binding.cdp_target_id.clear();
      binding.cdp_target_ws_url.clear();
      binding.devtools_endpoint.clear();
    }
    for (int i = 0; i < 80; ++i) {
      std::string list_json;
      std::string target_id;
      std::string ws_url;
      if (HttpGetText(base + "/json/list", list_json) &&
          ExtractFirstTargetFromJsonList(list_json, target_id, ws_url)) {
        binding.cdp_target_id = target_id;
        binding.cdp_target_ws_url = ws_url;
        binding.devtools_endpoint = ws_url;
        return true;
      }
      Sleep(50);
    }
    error = "live chrome devtools target unavailable";
    return false;
  }
};

std::unique_ptr<atlas::host::BrowserBackend> CreateBackend() {
  atlas::host::BackendConfig config;
  config.kind = g_backend_kind;
  config.capture_dom_snapshot = g_capture_dom_snapshot;
  config.owl_host_endpoint = g_owl_host_endpoint;
  config.remote_debug_host = g_remote_debug_host;
  config.remote_debug_port = g_remote_debug_port;
  config.chrome_binary_path = g_chrome_binary_path;
  if (config.kind == atlas::host::BackendKind::Owl) {
    return std::make_unique<OwlBackend>(config);
  }
  return std::make_unique<HeadlessCdpBackend>(config);
}

}  // namespace

namespace atlas::host {

BackendKind ParseBackendKind(const std::string& token) {
  std::string lower = token;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(tolower(c)); });
  if (lower == "owl") return BackendKind::Owl;
  return BackendKind::HeadlessCdp;
}

const char* BackendKindName(BackendKind kind) {
  switch (kind) {
    case BackendKind::Owl:
      return "owl";
    case BackendKind::HeadlessCdp:
    default:
      return "headless_cdp";
  }
}

std::unique_ptr<BrowserBackend> CreateBrowserBackend(const BackendConfig& config) {
  if (config.kind == BackendKind::Owl) {
    return std::make_unique<OwlBackend>(config);
  }
  return std::make_unique<HeadlessCdpBackend>(config);
}

bool EnablePerMonitorV2DpiAwareness() {
  using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) user32 = LoadLibraryW(L"user32.dll");
  auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
      GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (set_context && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    return true;
  }
  return SetProcessDPIAware() == TRUE;
}

}  // namespace atlas::host

namespace {

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
        auto vit = g_views.find(tit->second.view_id);
        if (vit == g_views.end()) return atlas::ParseError("view not found");
        std::string backend_error;
        if (!g_browser_backend->Navigate(vit->second.backend, url, backend_error)) {
          return atlas::ParseError(backend_error.empty() ? "navigation failed" : backend_error);
        }
        if (tit->second.view_id > 0) {
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
            std::string backend_error;
            g_browser_backend->DestroyView(vit->second.backend, backend_error);
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
        RECT initial_rect{0, 0, 1280, 800};
        std::string backend_error;
        if (!g_browser_backend->CreateView(view.view_id, view.hw, initial_rect, view.dpi,
                                           view.backend, backend_error)) {
          DestroyWindow(view.hw);
          return atlas::ParseError(backend_error.empty() ? "backend create view failed"
                                                         : backend_error);
        }
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
      atlas::RectPx rect;
      atlas::DpiScale dpi;
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
      view.backend.parent_hwnd = parent;
      std::string backend_error;
      if (!g_browser_backend->ResizeView(view.backend, view.rect, view.dpi, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "backend resize failed" : backend_error);
      }
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
      std::string backend_error;
      if (!g_browser_backend->ResizeView(it->second.backend, rect, it->second.dpi, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "backend resize failed" : backend_error);
      }
      return atlas::ParseOk("resized");
    }
    case atlas::Command::SetViewVisibility: {
      AtlasWindowId vid = 0;
      if (parts.size() < 3 || !ParseUint32(parts[1], vid)) return atlas::ParseError("bad args");
      bool visible = parts[2] == "1" || parts[2] == "true";
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      it->second.visible = visible;
      ShowWindow(it->second.hw, visible ? SW_SHOW : SW_HIDE);
      std::string backend_error;
      if (!g_browser_backend->SetVisible(it->second.backend, visible, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "backend visibility failed"
                                                       : backend_error);
      }
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
      std::string backend_error;
      g_browser_backend->DestroyView(vit->second.backend, backend_error);
      DestroyWindow(vit->second.hw);
      g_views.erase(vit);
      return atlas::ParseOk("destroyed");
    }
    case atlas::Command::CaptureContext: {
      if (parts.size() < 2) return atlas::ParseError("missing capture args");
      AtlasWindowId tid = 0;
      if (!ParseUint32(parts[1], tid)) return atlas::ParseError("bad tab");
      for (auto& entry : g_sessions) {
        auto tit = entry.second.tabs.find(tid);
        if (tit == entry.second.tabs.end()) continue;
        ViewState* view = nullptr;
        if (tit->second.view_id > 0) {
          auto vit = g_views.find(tit->second.view_id);
          if (vit != g_views.end()) view = &vit->second;
        }
        if (!view) return atlas::ParseError("view not found");
        atlas::host::CaptureRequest request;
        request.session_id = tit->second.session_id;
        request.tab_id = tit->second.tab_id;
        request.view_id = tit->second.view_id;
        request.url = tit->second.url.empty() ? "about:blank" : tit->second.url;
        request.title = request.url;
        request.rect = view->rect;
        request.dpi = view->dpi;
        std::string payload;
        std::string backend_error;
        if (!g_browser_backend->CaptureContext(view->backend, request, payload, backend_error)) {
          return atlas::ParseError(backend_error.empty() ? "capture failed" : backend_error);
        }
        return atlas::BuildMessage({"CAPTURE_CONTEXT", payload});
      }
      return atlas::ParseError("tab not found");
    }
    case atlas::Command::RouteMouse: {
      if (parts.size() < 6) return atlas::ParseError("missing mouse args");
      AtlasWindowId vid = 0;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad view id");
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      atlas::host::MouseDispatchParams params;
      params.type = atlas::UnescapeField(parts[2]);
      try {
        params.x = std::stoi(parts[3]);
        params.y = std::stoi(parts[4]);
        params.button = parts.size() >= 6 ? atlas::UnescapeField(parts[5]) : "left";
        params.click_count = parts.size() >= 7 ? std::stoi(parts[6]) : 1;
        params.modifiers = parts.size() >= 8 ? static_cast<uint32_t>(std::stoul(parts[7])) : 0;
      } catch (...) {
        return atlas::ParseError("bad mouse args");
      }
      std::string backend_error;
      if (!g_browser_backend->DispatchMouse(it->second.backend, params, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "mouse dispatch failed" : backend_error);
      }
      return atlas::BuildMessage({"OK", "1"});
    }
    case atlas::Command::RouteWheel: {
      if (parts.size() < 6) return atlas::ParseError("missing wheel args");
      AtlasWindowId vid = 0;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad view id");
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      atlas::host::MouseDispatchParams params;
      params.type = "mouseWheel";
      try {
        params.x = std::stoi(parts[3]);
        params.y = std::stoi(parts[4]);
        params.delta_y = std::stoi(parts[5]);
        params.modifiers = parts.size() >= 7 ? static_cast<uint32_t>(std::stoul(parts[6])) : 0;
      } catch (...) {
        return atlas::ParseError("bad wheel args");
      }
      std::string backend_error;
      if (!g_browser_backend->DispatchWheel(it->second.backend, params, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "wheel dispatch failed" : backend_error);
      }
      return atlas::BuildMessage({"OK", "1"});
    }
    case atlas::Command::RouteKeyboard: {
      if (parts.size() < 5) return atlas::ParseError("missing key args");
      AtlasWindowId vid = 0;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad view id");
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      atlas::host::KeyDispatchParams params;
      try {
        params.message = static_cast<UINT>(std::stoul(parts[2]));
        params.wparam = static_cast<WPARAM>(std::stoull(parts[3]));
        params.lparam = static_cast<LPARAM>(std::stoll(parts[4]));
        params.modifiers = parts.size() >= 6 ? static_cast<uint32_t>(std::stoul(parts[5])) : 0;
      } catch (...) {
        return atlas::ParseError("bad key args");
      }
      std::string backend_error;
      if (!g_browser_backend->DispatchKeyboard(it->second.backend, params, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "key dispatch failed" : backend_error);
      }
      return atlas::BuildMessage({"OK", "1"});
    }
    case atlas::Command::SetFocus: {
      if (parts.size() < 3) return atlas::ParseError("missing focus args");
      AtlasWindowId vid = 0;
      if (!ParseUint32(parts[1], vid)) return atlas::ParseError("bad args");
      bool focus = parts[2] == "1" || parts[2] == "true";
      auto it = g_views.find(vid);
      if (it == g_views.end()) return atlas::ParseError("view not found");
      if (focus) SetFocus(it->second.hw);
      it->second.focused = focus;
      std::string backend_error;
      if (!g_browser_backend->SetFocus(it->second.backend, focus, backend_error)) {
        return atlas::ParseError(backend_error.empty() ? "backend focus failed" : backend_error);
      }
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
  atlas::host::EnablePerMonitorV2DpiAwareness();
  ParseHostArgs();
  g_browser_backend = CreateBackend();

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
