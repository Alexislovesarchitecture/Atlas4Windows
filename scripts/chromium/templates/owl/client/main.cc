#include <windows.h>

#include <iostream>
#include <string>

int wmain(int argc, wchar_t* argv[]) {
  std::wstring profile_root;
  std::wstring chrome_binary;
  std::wstring remote_debugging_port;
  bool smoke_test = false;

  constexpr wchar_t kProfilePrefix[] = L"--profile_root=";
  constexpr size_t kProfilePrefixLen = _countof(kProfilePrefix) - 1;
  constexpr wchar_t kChromeBinaryPrefix[] = L"--chrome_binary=";
  constexpr size_t kChromeBinaryPrefixLen = _countof(kChromeBinaryPrefix) - 1;
  constexpr wchar_t kRemoteDebuggingPortPrefix[] = L"--remote_debugging_port=";
  constexpr size_t kRemoteDebuggingPortPrefixLen =
      _countof(kRemoteDebuggingPortPrefix) - 1;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg(argv[i]);
    if (arg.rfind(kProfilePrefix, 0) == 0) {
      profile_root = arg.substr(kProfilePrefixLen);
      continue;
    }
    if (arg.rfind(kChromeBinaryPrefix, 0) == 0) {
      chrome_binary = arg.substr(kChromeBinaryPrefixLen);
      continue;
    }
    if (arg.rfind(kRemoteDebuggingPortPrefix, 0) == 0) {
      remote_debugging_port = arg.substr(kRemoteDebuggingPortPrefixLen);
      continue;
    }
    if (arg == L"--smoke-test") {
      smoke_test = true;
    }
  }

  std::wcout << L"owl_client scaffold started";
  if (!profile_root.empty()) {
    std::wcout << L" profile_root=" << profile_root;
  }
  if (!chrome_binary.empty()) {
    std::wcout << L" chrome_binary=" << chrome_binary;
  }
  if (!remote_debugging_port.empty()) {
    std::wcout << L" remote_debugging_port=" << remote_debugging_port;
  }
  std::wcout << std::endl;

  if (smoke_test) {
    std::wcout << L"owl_client smoke test OK" << std::endl;
    return 0;
  }

  // Keep the process alive long enough for run-script/process smoke checks.
  Sleep(30000);
  return 0;
}
