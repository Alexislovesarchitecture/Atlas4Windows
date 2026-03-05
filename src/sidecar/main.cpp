#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/ipc_client.h"
#include "common/protocol.h"

namespace {

constexpr uint16_t kDefaultPort = 8765;
constexpr int64_t kDefaultTokenLifetimeMs = 30000;
constexpr int kListenBacklog = 8;

struct PendingAction {
  std::string token_id;
  std::string action_name;
  std::string action_hash;
  std::string top_level_origin;
  uint32_t tab_id = 0;
  uint32_t view_id = 0;
  int x = 0;
  int y = 0;
  int delta = 0;
  std::string button = "left";
  std::string url;
  int64_t issued_at_ms = 0;
  int64_t expires_at_ms = 0;
  bool single_use = true;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

uint16_t g_port = kDefaultPort;
std::mutex g_actions_mutex;
std::unordered_map<std::string, PendingAction> g_pending_actions;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 16);
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
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
      default:
        if (ch < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return out;
}

std::string MakeJsonRpcResult(std::string_view id_raw, std::string_view result_json) {
  return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::string(id_raw) + ",\"result\":" +
         std::string(result_json) + "}";
}

std::string MakeJsonRpcError(std::string_view id_raw,
                             int code,
                             std::string_view message,
                             std::string_view data_json = "null") {
  std::ostringstream out;
  out << "{\"jsonrpc\":\"2.0\",\"id\":" << id_raw
      << ",\"error\":{\"code\":" << code << ",\"message\":\"" << JsonEscape(message)
      << "\",\"data\":" << data_json << "}}";
  return out.str();
}

std::string Iso8601UtcFromMs(int64_t ms_since_epoch) {
  std::time_t seconds = static_cast<std::time_t>(ms_since_epoch / 1000);
  std::tm tm_utc{};
  gmtime_s(&tm_utc, &seconds);
  char buffer[32];
  std::snprintf(buffer,
                sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm_utc.tm_year + 1900,
                tm_utc.tm_mon + 1,
                tm_utc.tm_mday,
                tm_utc.tm_hour,
                tm_utc.tm_min,
                tm_utc.tm_sec);
  return buffer;
}

std::string Sha256Hex(std::string_view payload) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_length = 0;
  DWORD hash_length = 0;
  DWORD bytes = 0;
  std::vector<BYTE> object_buffer;
  std::vector<BYTE> hash_buffer;

  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    return "";
  }
  if (BCryptGetProperty(algorithm,
                        BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&object_length),
                        sizeof(object_length),
                        &bytes,
                        0) != 0 ||
      BCryptGetProperty(algorithm,
                        BCRYPT_HASH_LENGTH,
                        reinterpret_cast<PUCHAR>(&hash_length),
                        sizeof(hash_length),
                        &bytes,
                        0) != 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return "";
  }

  object_buffer.resize(object_length);
  hash_buffer.resize(hash_length);
  if (BCryptCreateHash(algorithm, &hash, object_buffer.data(), object_length, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return "";
  }
  if (BCryptHashData(hash,
                     reinterpret_cast<PUCHAR>(const_cast<char*>(payload.data())),
                     static_cast<ULONG>(payload.size()),
                     0) != 0 ||
      BCryptFinishHash(hash, hash_buffer.data(), hash_length, 0) != 0) {
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return "";
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.resize(hash_buffer.size() * 2);
  for (size_t i = 0; i < hash_buffer.size(); ++i) {
    hex[i * 2] = kHex[(hash_buffer[i] >> 4) & 0xF];
    hex[i * 2 + 1] = kHex[hash_buffer[i] & 0xF];
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}

std::string GenerateTokenId() {
  BYTE bytes[16]{};
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return Sha256Hex(std::to_string(NowMs()));
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.resize(sizeof(bytes) * 2);
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    token[i * 2] = kHex[(bytes[i] >> 4) & 0xF];
    token[i * 2 + 1] = kHex[bytes[i] & 0xF];
  }
  return token;
}

void SkipWhitespace(std::string_view json, size_t& index) {
  while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index])) != 0) {
    ++index;
  }
}

bool ParseJsonString(std::string_view json, size_t& index, std::string& out) {
  if (index >= json.size() || json[index] != '"') return false;
  ++index;
  out.clear();
  while (index < json.size()) {
    char ch = json[index++];
    if (ch == '"') return true;
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (index >= json.size()) return false;
    char escape = json[index++];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escape);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        return false;
    }
  }
  return false;
}

size_t FindJsonValueEnd(std::string_view json, size_t index) {
  if (index >= json.size()) return std::string_view::npos;
  if (json[index] == '"') {
    std::string ignored;
    size_t cursor = index;
    return ParseJsonString(json, cursor, ignored) ? cursor : std::string_view::npos;
  }
  if (json[index] == '{' || json[index] == '[') {
    const char open = json[index];
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t cursor = index; cursor < json.size(); ++cursor) {
      const char ch = json[cursor];
      if (in_string) {
        if (escape) {
          escape = false;
        } else if (ch == '\\') {
          escape = true;
        } else if (ch == '"') {
          in_string = false;
        }
        continue;
      }
      if (ch == '"') {
        in_string = true;
        continue;
      }
      if (ch == open) ++depth;
      if (ch == close) {
        --depth;
        if (depth == 0) return cursor + 1;
      }
    }
    return std::string_view::npos;
  }

  size_t cursor = index;
  while (cursor < json.size() && json[cursor] != ',' && json[cursor] != '}') ++cursor;
  return cursor;
}

bool ExtractTopLevelRaw(std::string_view json, std::string_view key, std::string& out) {
  size_t index = 0;
  SkipWhitespace(json, index);
  if (index >= json.size() || json[index] != '{') return false;
  ++index;
  while (index < json.size()) {
    SkipWhitespace(json, index);
    if (index < json.size() && json[index] == '}') return false;
    std::string parsed_key;
    if (!ParseJsonString(json, index, parsed_key)) return false;
    SkipWhitespace(json, index);
    if (index >= json.size() || json[index] != ':') return false;
    ++index;
    SkipWhitespace(json, index);
    const size_t value_start = index;
    const size_t value_end = FindJsonValueEnd(json, value_start);
    if (value_end == std::string_view::npos) return false;
    if (parsed_key == key) {
      out.assign(json.substr(value_start, value_end - value_start));
      return true;
    }
    index = value_end;
    SkipWhitespace(json, index);
    if (index < json.size() && json[index] == ',') ++index;
  }
  return false;
}

bool ExtractObject(std::string_view json, std::string_view key, std::string& out) {
  if (!ExtractTopLevelRaw(json, key, out)) return false;
  return !out.empty() && out.front() == '{';
}

bool ExtractString(std::string_view json, std::string_view key, std::string& out) {
  std::string raw;
  if (!ExtractTopLevelRaw(json, key, raw)) return false;
  size_t index = 0;
  return ParseJsonString(raw, index, out);
}

bool ExtractInteger(std::string_view json, std::string_view key, int64_t& out) {
  std::string raw;
  if (!ExtractTopLevelRaw(json, key, raw)) return false;
  try {
    out = std::stoll(raw);
    return true;
  } catch (...) {
    return false;
  }
}

bool HostSend(const std::vector<std::string>& fields, std::string& response) {
  atlas::IpcClient client;
  if (!client.connect(atlas::kPipeName)) return false;
  return client.sendMessage(atlas::BuildMessage(fields), response);
}

void InvalidateTokensForView(uint32_t view_id) {
  if (view_id == 0) return;
  std::lock_guard<std::mutex> lock(g_actions_mutex);
  for (auto it = g_pending_actions.begin(); it != g_pending_actions.end();) {
    if (it->second.view_id == view_id) {
      it = g_pending_actions.erase(it);
    } else {
      ++it;
    }
  }
}

void PruneExpiredTokensLocked(int64_t now_ms) {
  for (auto it = g_pending_actions.begin(); it != g_pending_actions.end();) {
    if (it->second.expires_at_ms <= now_ms) {
      it = g_pending_actions.erase(it);
    } else {
      ++it;
    }
  }
}

std::string CanonicalizeAction(const PendingAction& action) {
  return action.action_name + "|" + std::to_string(action.tab_id) + "|" + std::to_string(action.view_id) +
         "|" + std::to_string(action.x) + "|" + std::to_string(action.y) + "|" +
         std::to_string(action.delta) + "|" + action.button + "|" + action.url + "|" +
         action.top_level_origin;
}

std::optional<PendingAction> BuildPendingAction(std::string_view params_json) {
  PendingAction action;
  if (!ExtractString(params_json, "action_name", action.action_name)) return std::nullopt;
  ExtractString(params_json, "top_level_origin", action.top_level_origin);
  int64_t value = 0;
  if (ExtractInteger(params_json, "tab_id", value)) action.tab_id = static_cast<uint32_t>(value);
  if (ExtractInteger(params_json, "view_id", value)) action.view_id = static_cast<uint32_t>(value);
  if (ExtractInteger(params_json, "x", value)) action.x = static_cast<int>(value);
  if (ExtractInteger(params_json, "y", value)) action.y = static_cast<int>(value);
  if (ExtractInteger(params_json, "delta", value)) action.delta = static_cast<int>(value);
  ExtractString(params_json, "button", action.button);
  ExtractString(params_json, "url", action.url);

  action.issued_at_ms = NowMs();
  action.expires_at_ms = action.issued_at_ms + kDefaultTokenLifetimeMs;
  action.token_id = GenerateTokenId();
  action.action_hash = Sha256Hex(CanonicalizeAction(action));
  return action;
}

std::optional<PendingAction> ValidatePendingAction(std::string_view params_json, std::string& error) {
  std::string token_id;
  if (!ExtractString(params_json, "token_id", token_id)) {
    error = "missing token_id";
    return std::nullopt;
  }

  std::lock_guard<std::mutex> lock(g_actions_mutex);
  const int64_t now_ms = NowMs();
  PruneExpiredTokensLocked(now_ms);
  auto it = g_pending_actions.find(token_id);
  if (it == g_pending_actions.end()) {
    error = "invalid or expired token";
    return std::nullopt;
  }

  PendingAction expected = it->second;
  if (expected.single_use) {
    g_pending_actions.erase(it);
  }

  auto actual = BuildPendingAction(params_json);
  if (!actual.has_value()) {
    error = "invalid action payload";
    return std::nullopt;
  }
  actual->token_id = expected.token_id;
  actual->issued_at_ms = expected.issued_at_ms;
  actual->expires_at_ms = expected.expires_at_ms;
  const std::string computed_hash = Sha256Hex(CanonicalizeAction(*actual));
  if (computed_hash != expected.action_hash) {
    error = "action hash mismatch";
    return std::nullopt;
  }
  if (!expected.top_level_origin.empty() && actual->top_level_origin != expected.top_level_origin) {
    error = "origin mismatch";
    return std::nullopt;
  }
  return actual;
}

HttpResponse HandleNavigate(std::string_view id_raw, std::string_view params_json) {
  int64_t tab_id = 0;
  std::string url;
  if (!ExtractInteger(params_json, "tab_id", tab_id) || !ExtractString(params_json, "url", url)) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32602, "tab_id and url are required")};
  }

  std::string response;
  if (!HostSend({"Navigate", std::to_string(tab_id), url}, response)) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32001, "host navigate failed")};
  }

  int64_t view_id = 0;
  if (ExtractInteger(params_json, "view_id", view_id)) {
    InvalidateTokensForView(static_cast<uint32_t>(view_id));
  }

  return {200,
          "application/json",
          MakeJsonRpcResult(id_raw, "{\"status\":\"ok\",\"response\":\"" + JsonEscape(response) + "\"}")};
}

HttpResponse HandleCaptureContext(std::string_view id_raw, std::string_view params_json) {
  int64_t tab_id = 0;
  if (!ExtractInteger(params_json, "tab_id", tab_id)) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32602, "tab_id is required")};
  }

  std::string response;
  if (!HostSend({"CaptureContext", std::to_string(tab_id)}, response)) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32001, "host capture failed")};
  }

  auto parts = atlas::SplitMessage(response);
  if (parts.size() < 2 || parts[0] != "CAPTURE_CONTEXT") {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32002, "unexpected host response")};
  }

  std::string payload = atlas::UnescapeField(parts[1]);
  return {200,
          "application/json",
          MakeJsonRpcResult(id_raw, "{\"context\":" + payload + "}")};
}

HttpResponse HandleProposeAction(std::string_view id_raw, std::string_view params_json) {
  auto action = BuildPendingAction(params_json);
  if (!action.has_value()) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32602, "invalid action proposal")};
  }

  {
    std::lock_guard<std::mutex> lock(g_actions_mutex);
    PruneExpiredTokensLocked(NowMs());
    g_pending_actions[action->token_id] = *action;
  }

  std::ostringstream result;
  result << "{\"token_id\":\"" << action->token_id << "\",\"view_id\":" << action->view_id
         << ",\"top_level_origin\":\"" << JsonEscape(action->top_level_origin)
         << "\",\"action_hash\":\"" << action->action_hash << "\",\"issued_at\":\""
         << Iso8601UtcFromMs(action->issued_at_ms) << "\",\"expires_at\":\""
         << Iso8601UtcFromMs(action->expires_at_ms) << "\",\"single_use\":true}";
  return {200, "application/json", MakeJsonRpcResult(id_raw, result.str())};
}

HttpResponse HandleExecuteAction(std::string_view id_raw, std::string_view params_json) {
  std::string error;
  auto action = ValidatePendingAction(params_json, error);
  if (!action.has_value()) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32003, error)};
  }

  std::string response;
  if (action->action_name == "click") {
    if (action->view_id == 0) {
      return {200, "application/json", MakeJsonRpcError(id_raw, -32602, "view_id is required for click")};
    }
    if (!HostSend({"RouteMouse", std::to_string(action->view_id), "mousePressed", action->button,
                   std::to_string(action->x), std::to_string(action->y), "1", "0"},
                  response) ||
        !HostSend({"RouteMouse", std::to_string(action->view_id), "mouseReleased", action->button,
                   std::to_string(action->x), std::to_string(action->y), "1", "0"},
                  response)) {
      return {200, "application/json", MakeJsonRpcError(id_raw, -32001, "host click failed")};
    }
    InvalidateTokensForView(action->view_id);
  } else if (action->action_name == "scroll") {
    if (action->view_id == 0) {
      return {200, "application/json", MakeJsonRpcError(id_raw, -32602, "view_id is required for scroll")};
    }
    if (!HostSend({"RouteWheel", std::to_string(action->view_id), "mouseWheel",
                   std::to_string(action->x), std::to_string(action->y), std::to_string(action->delta),
                   "0"},
                  response)) {
      return {200, "application/json", MakeJsonRpcError(id_raw, -32001, "host scroll failed")};
    }
    InvalidateTokensForView(action->view_id);
  } else if (action->action_name == "navigate") {
    if (action->tab_id == 0 || action->url.empty()) {
      return {200, "application/json", MakeJsonRpcError(id_raw, -32602, "tab_id and url are required")};
    }
    if (!HostSend({"Navigate", std::to_string(action->tab_id), action->url}, response)) {
      return {200, "application/json", MakeJsonRpcError(id_raw, -32001, "host navigate failed")};
    }
    InvalidateTokensForView(action->view_id);
  } else {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32004, "action not implemented")};
  }

  return {200,
          "application/json",
          MakeJsonRpcResult(id_raw,
                            "{\"status\":\"executed\",\"action_name\":\"" +
                                JsonEscape(action->action_name) + "\"}")};
}

HttpResponse HandleMcp(std::string_view body) {
  std::string id_raw = "null";
  std::string method;
  std::string params_json = "{}";
  ExtractTopLevelRaw(body, "id", id_raw);
  if (!ExtractString(body, "method", method)) {
    return {200, "application/json", MakeJsonRpcError(id_raw, -32600, "missing method")};
  }
  ExtractObject(body, "params", params_json);

  if (method == "navigate") {
    return HandleNavigate(id_raw, params_json);
  }
  if (method == "capture_context") {
    return HandleCaptureContext(id_raw, params_json);
  }
  if (method == "get_interactive_elements") {
    return {200,
            "application/json",
            MakeJsonRpcResult(id_raw, "{\"elements\":[],\"status\":\"not_implemented\"}")};
  }
  if (method == "list_tabs") {
    return {200,
            "application/json",
            MakeJsonRpcError(id_raw, -32004, "list_tabs is not implemented on the current host protocol")};
  }
  if (method == "propose_action") {
    return HandleProposeAction(id_raw, params_json);
  }
  if (method == "execute_action") {
    return HandleExecuteAction(id_raw, params_json);
  }

  return {200, "application/json", MakeJsonRpcError(id_raw, -32601, "method not found")};
}

bool ReadHttpRequest(SOCKET socket, HttpRequest& request) {
  std::string raw;
  char buffer[4096];
  size_t header_end = std::string::npos;
  size_t content_length = 0;

  while (true) {
    const int received = recv(socket, buffer, sizeof(buffer), 0);
    if (received <= 0) return false;
    raw.append(buffer, received);
    header_end = raw.find("\r\n\r\n");
    if (header_end != std::string::npos) break;
    if (raw.size() > 65536) return false;
  }

  std::istringstream header_stream(raw.substr(0, header_end));
  std::string request_line;
  if (!std::getline(header_stream, request_line)) return false;
  if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
  std::istringstream request_line_stream(request_line);
  request_line_stream >> request.method >> request.path;
  if (request.method.empty() || request.path.empty()) return false;

  std::string header;
  while (std::getline(header_stream, header)) {
    if (!header.empty() && header.back() == '\r') header.pop_back();
    const auto colon = header.find(':');
    if (colon == std::string::npos) continue;
    std::string key = header.substr(0, colon);
    std::string value = header.substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.erase(value.begin());
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (key == "content-length") {
      content_length = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }
  }

  const size_t body_start = header_end + 4;
  while (raw.size() < body_start + content_length) {
    const int received = recv(socket, buffer, sizeof(buffer), 0);
    if (received <= 0) return false;
    raw.append(buffer, received);
  }
  request.body.assign(raw.data() + body_start, content_length);
  return true;
}

void SendHttpResponse(SOCKET socket, const HttpResponse& response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << " "
      << (response.status == 200 ? "OK" : "ERROR") << "\r\n"
      << "Content-Type: " << response.content_type << "\r\n"
      << "Content-Length: " << response.body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << response.body;
  const std::string payload = out.str();
  size_t sent = 0;
  while (sent < payload.size()) {
    const int wrote =
        send(socket, payload.data() + sent, static_cast<int>(payload.size() - sent), 0);
    if (wrote <= 0) break;
    sent += static_cast<size_t>(wrote);
  }
}

void ParseArgs(int argc, wchar_t* argv[]) {
  constexpr wchar_t kPortPrefix[] = L"--port=";
  constexpr size_t kPortPrefixLen = _countof(kPortPrefix) - 1;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg(argv[i]);
    if (arg.rfind(kPortPrefix, 0) == 0) {
      g_port = static_cast<uint16_t>(_wtoi(arg.substr(kPortPrefixLen).c_str()));
    }
  }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  ParseArgs(argc, argv);

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return 1;
  }

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    WSACleanup();
    return 1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(g_port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
      listen(listener, kListenBacklog) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    return 1;
  }

  while (true) {
    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;

    HttpRequest request;
    if (!ReadHttpRequest(client, request)) {
      SendHttpResponse(client, {400, "application/json", "{\"error\":\"bad request\"}"});
      closesocket(client);
      continue;
    }

    HttpResponse response;
    if (request.method == "GET" && request.path == "/health") {
      response.body = "{\"ok\":true,\"mcp_url\":\"http://127.0.0.1:" + std::to_string(g_port) +
                      "/mcp\"}";
    } else if (request.method == "POST" && request.path == "/mcp") {
      response = HandleMcp(request.body);
    } else {
      response.status = 404;
      response.body = "{\"error\":\"not found\"}";
    }

    SendHttpResponse(client, response);
    closesocket(client);
  }

  closesocket(listener);
  WSACleanup();
  return 0;
}
