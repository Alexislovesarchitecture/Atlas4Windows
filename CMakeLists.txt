cmake_minimum_required(VERSION 3.20)

project(AtlasWindowsMVP LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "AtlasWindowsMVP is Windows-only")
endif()

add_library(atlas_common
  src/common/protocol.cpp
  src/common/ipc_client.cpp
  src/common/ipc_server.cpp)

target_include_directories(atlas_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(atlas_common PRIVATE User32)

add_executable(atlas-host WIN32
  src/host/main.cpp
  src/host/atlas-host.rc)
target_link_libraries(atlas-host PRIVATE atlas_common)
target_link_libraries(atlas-host PRIVATE User32 Gdi32 Winhttp Bcrypt)

add_executable(atlas-client WIN32
  src/client/main.cpp
  src/client/atlas-client.rc)
target_link_libraries(atlas-client PRIVATE atlas_common)
target_link_libraries(atlas-client PRIVATE User32 Gdi32 Comctl32)

add_executable(atlas-sidecar src/sidecar/main.cpp)
target_link_libraries(atlas-sidecar PRIVATE atlas_common)
target_link_libraries(atlas-sidecar PRIVATE Ws2_32 Bcrypt)
