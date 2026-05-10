cmake_minimum_required(VERSION 3.22)

project(mydrive LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_path(BOOST_ASIO_INCLUDE_DIR boost/asio.hpp
  HINTS
    /opt/homebrew/opt/boost/include
    /usr/local/include
)
if(NOT BOOST_ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "Boost.Asio headers were not found")
endif()

add_library(mydrive_common
  src/common/checksum.cpp
  src/common/config.cpp
  src/common/protocol.cpp
)

target_include_directories(mydrive_common PUBLIC src ${BOOST_ASIO_INCLUDE_DIR})
target_compile_definitions(mydrive_common PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
target_link_libraries(mydrive_common
  PUBLIC
    OpenSSL::Crypto
    Threads::Threads
)

if(APPLE)
  target_compile_definitions(mydrive_common PUBLIC MYDRIVE_PLATFORM_MACOS=1)
elseif(UNIX)
  target_compile_definitions(mydrive_common PUBLIC MYDRIVE_PLATFORM_LINUX=1)
endif()

add_executable(mydrive_server src/server/main.cpp)
target_link_libraries(mydrive_server PRIVATE mydrive_common)

add_executable(mydrive_client src/client/main.cpp)
target_link_libraries(mydrive_client PRIVATE mydrive_common)

target_compile_options(mydrive_common PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(mydrive_server PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(mydrive_client PRIVATE -Wall -Wextra -Wpedantic)
