#pragma once

#include "common/protocol.h"

#include <string>

namespace atlas {
class IpcClient {
 public:
  IpcClient() = default;
  ~IpcClient() = default;
  bool connect(const char* pipe_name = kPipeName);
  bool sendMessage(const std::string& message, std::string& response);
  bool isConnected() const;
  void close();

 private:
  void* pipe_ = nullptr;
};
}  // namespace atlas
