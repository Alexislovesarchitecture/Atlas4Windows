#include "common/ipc_client.h"

#include <Windows.h>
#include <string>

namespace atlas {

bool IpcClient::connect(const char* pipe_name) {
  if (pipe_) return true;
  pipe_ = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) {
    pipe_ = nullptr;
    return false;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState((HANDLE)pipe_, &mode, nullptr, nullptr);
  return true;
}

bool IpcClient::sendMessage(const std::string& message, std::string& response) {
  if (!pipe_) return false;

  DWORD written = 0;
  if (!WriteFile((HANDLE)pipe_, message.data(), static_cast<DWORD>(message.size()), &written,
                 nullptr)) {
    return false;
  }

  char buffer[4096] = {};
  DWORD read = 0;
  if (!ReadFile((HANDLE)pipe_, buffer, sizeof(buffer) - 1, &read, nullptr)) {
    return false;
  }
  response.assign(buffer, buffer + read);
  if (!response.empty() && response.back() == '\n') response.pop_back();
  return true;
}

bool IpcClient::isConnected() const {
  return pipe_ != nullptr;
}

void IpcClient::close() {
  if (!pipe_) return;
  CloseHandle((HANDLE)pipe_);
  pipe_ = nullptr;
}

}  // namespace atlas
