#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

inline constexpr char kPipeName[] = R"(\\.\pipe\AtlasOwlHost)";
inline constexpr char kMessageTerminator = '\n';

enum class Command {
  Unknown,
  CreateSession,
  CreateTab,
  Navigate,
  GoBack,
  GoForward,
  Reload,
  CloseTab,
  CreateEmbeddedView,
  AttachToParentHwnd,
  ResizeView,
  SetViewVisibility,
  DestroyView,
  CaptureContext,
  RouteMouse,
  RouteWheel,
  RouteKeyboard,
  SetFocus,
  SetDownloadDirectory,
  OnDownloadStateChanged,
  Ping,
};

enum class DownloadState : uint32_t {
  STARTED = 0,
  IN_PROGRESS = 1,
  COMPLETE = 2,
  FAILED = 3,
  CANCELED = 4,
};

struct RectPx {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct DpiScale {
  float x = 1.0f;
  float y = 1.0f;
};

struct SessionConfig {
  std::string profile_path;
  bool incognito_enabled = false;
};

struct InputEventEnvelope {
  uint64_t timestamp_ms = 0;
  uint32_t modifiers = 0;
  uint32_t device_kind = 0;
  int x = 0;
  int y = 0;
  std::string raw_data;
};

std::vector<std::string> SplitMessage(std::string_view message);
std::string EscapeField(std::string_view value);
std::string UnescapeField(std::string_view value);
std::string BuildMessage(const std::vector<std::string>& fields);
Command ParseCommand(std::string_view token);
std::string ParseError(std::string_view message);
std::string ParseOk(std::string_view payload = "");

}  // namespace atlas
