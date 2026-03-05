#include "common/protocol.h"

#include <sstream>

namespace atlas {

std::vector<std::string> SplitMessage(std::string_view message) {
  std::vector<std::string> parts;
  std::string token;
  for (char ch : message) {
    if (ch == '|') {
      parts.push_back(token);
      token.clear();
      continue;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      break;
    }
    token.push_back(ch);
  }
  parts.push_back(token);
  return parts;
}

std::string EscapeField(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 4);
  for (char c : value) {
    switch (c) {
      case '%':
        out += "%25";
        break;
      case '|':
        out += "%7C";
        break;
      case '\n':
        out += "%0A";
        break;
      case '\r':
        out += "%0D";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string UnescapeField(std::string_view value) {
  std::string out;
  for (size_t i = 0; i < value.size();) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const char h1 = value[i + 1];
      const char h2 = value[i + 2];
      if ((h1 == '2' && h2 == '5') || (h1 == '7' && h2 == 'C') ||
          (h1 == '0' && (h2 == 'A' || h2 == 'D'))) {
        if (h1 == '2') out.push_back('%');
        else if (h1 == '7') out.push_back('|');
        else if (h2 == 'A') out.push_back('\n');
        else if (h2 == 'D') out.push_back('\r');
        i += 3;
        continue;
      }
    }
    out.push_back(value[i]);
    ++i;
  }
  return out;
}

std::string BuildMessage(const std::vector<std::string>& fields) {
  std::ostringstream oss;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) oss << '|';
    oss << EscapeField(fields[i]);
  }
  oss << '\n';
  return oss.str();
}

Command ParseCommand(std::string_view token) {
  if (token == "CreateSession") return Command::CreateSession;
  if (token == "CreateTab") return Command::CreateTab;
  if (token == "Navigate") return Command::Navigate;
  if (token == "GoBack") return Command::GoBack;
  if (token == "GoForward") return Command::GoForward;
  if (token == "Reload") return Command::Reload;
  if (token == "CloseTab") return Command::CloseTab;
  if (token == "CreateEmbeddedView") return Command::CreateEmbeddedView;
  if (token == "AttachToParentHwnd") return Command::AttachToParentHwnd;
  if (token == "ResizeView") return Command::ResizeView;
  if (token == "SetViewVisibility") return Command::SetViewVisibility;
  if (token == "DestroyView") return Command::DestroyView;
  if (token == "CaptureContext") return Command::CaptureContext;
  if (token == "RouteMouse") return Command::RouteMouse;
  if (token == "RouteWheel") return Command::RouteWheel;
  if (token == "RouteKeyboard") return Command::RouteKeyboard;
  if (token == "SetFocus") return Command::SetFocus;
  if (token == "SetDownloadDirectory") return Command::SetDownloadDirectory;
  if (token == "OnDownloadStateChanged") return Command::OnDownloadStateChanged;
  if (token == "Ping") return Command::Ping;
  return Command::Unknown;
}

std::string ParseError(std::string_view message) {
  return BuildMessage({"ERR", std::string(message)});
}

std::string ParseOk(std::string_view payload) {
  if (payload.empty()) return std::string("OK\n");
  return BuildMessage({"OK", std::string(payload)});
}

}  // namespace atlas
