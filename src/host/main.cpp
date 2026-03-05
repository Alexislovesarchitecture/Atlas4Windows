#include <Windows.h>
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
#include <winhttp.h>

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
    }
  }
  LocalFree(argv);
  return true;
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
        if (!EnsureTargetForTab(tit->second, tit->second.url)) {
          return atlas::ParseError("target unavailable");
        }
        std::string response;
        std::string nav_params = "{\"url\":\"" + EscapeJson(url.empty() ? "about:blank" : url) + "\"}";
        if (!SendCdpCommand(tit->second, "Page.navigate", nav_params, response)) {
          return atlas::ParseError("navigation failed");
        }
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
        std::string payload = BuildCapturePayload(tit->second, view);
        if (payload.empty()) return atlas::ParseError("capture failed");
        return atlas::BuildMessage({"CAPTURE_CONTEXT", payload});
      }
      return atlas::ParseError("tab not found");
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
  ParseHostArgs();

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
