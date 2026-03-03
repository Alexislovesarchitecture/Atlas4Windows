#include "common/ipc_server.h"

#include <Windows.h>
#include <atomic>
#include <thread>

#include "common/protocol.h"

namespace atlas {

IpcServer::IpcServer() {
  stop_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
}

IpcServer::~IpcServer() {
  stop();
  if (stop_event_) {
    CloseHandle((HANDLE)stop_event_);
    stop_event_ = nullptr;
  }
}

void IpcServer::run(const char* pipe_name, Handler handler) {
  while (running_) {
    HANDLE pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                     PIPE_WAIT,
                                 1, 4096, 4096, 0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) break;

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe);
      if (!running_) {
        break;
      }
      continue;
    }

    char buffer[4096] = {};
    DWORD bytes_read = 0;
    while (running_ &&
           ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytes_read, nullptr) && bytes_read > 0) {
      std::string request(buffer, buffer + bytes_read);
      size_t start = 0;
      while (start < request.size()) {
        size_t end = request.find('\n', start);
        std::string line = request.substr(start, end == std::string::npos ? std::string::npos
                                                                          : end - start);
        if (line.empty()) {
          if (end == std::string::npos) break;
          start = end + 1;
          continue;
        }
        std::string response = handler(line);
        DWORD bytes_written = 0;
        WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &bytes_written,
                  nullptr);
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      memset(buffer, 0, sizeof(buffer));
    }

    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

bool IpcServer::start(const char* pipe_name, Handler handler) {
  if (running_) return true;
  if (thread_handle_) return false;

  running_ = true;
  using RunContext = std::pair<IpcServer*, std::pair<std::string, Handler>>;
  HANDLE thread = CreateThread(
      nullptr, 0,
      [](LPVOID param) -> DWORD {
        auto* ctx = static_cast<RunContext*>(param);
        auto* server = ctx->first;
        auto named_pipe = std::move(ctx->second.first);
        auto handler = std::move(ctx->second.second);
        delete ctx;
        server->run(named_pipe.c_str(), handler);
        return 0;
      },
      new RunContext(this, std::pair<std::string, Handler>(pipe_name ? pipe_name : atlas::kPipeName, handler)),
      0, nullptr);

  if (!thread) {
    running_ = false;
    return false;
  }

  thread_handle_ = thread;
  return true;
}

void IpcServer::stop() {
  if (!running_) return;
  running_ = false;
  SetEvent((HANDLE)stop_event_);
  if (thread_handle_) {
    WaitForSingleObject((HANDLE)thread_handle_, INFINITE);
    CloseHandle((HANDLE)thread_handle_);
    thread_handle_ = nullptr;
  }
}

}  // namespace atlas
