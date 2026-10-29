cmake_minimum_required(VERSION 3.20)
project(xport VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(xport STATIC
  src/crc32c.cpp
  src/cc.cpp
  src/packet.cpp
  src/sim.cpp
  src/transport.cpp
  src/udp.cpp
)
target_include_directories(xport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Winsock 不是 libc 的一部分，必须显式链接；POSIX 上 socket 就在 libc 里
if(WIN32)
  target_link_libraries(xport PUBLIC ws2_32)
endif()

add_subdirectory(apps)

enable_testing()
add_subdirectory(tests)
