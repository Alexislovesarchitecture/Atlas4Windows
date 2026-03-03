#pragma once

#include <functional>
#include <string>
#include <atomic>

namespace atlas {
class IpcServer {
 public:
  using Handler = std::function<std::string(const std::string&)>;

  IpcServer();
  ~IpcServer();

  bool start(const char* pipe_name, Handler handler);
  void stop();

 private:
  void run(const char* pipe_name, Handler handler);

  void* thread_handle_ = nullptr;
  void* stop_event_ = nullptr;
  std::atomic<bool> running_{false};
};
}  // namespace atlas
