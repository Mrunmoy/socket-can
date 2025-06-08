cmake_minimum_required(VERSION 3.16)
project(SocketCan LANGUAGES CXX)

# ===============================
# Library (header-only)
# ===============================
# BasicCanbus<bool EnableFd> is a class template, so the library is header-only
# and exposed as an INTERFACE target.
set(HEADERS
    SocketCan.hpp
)

add_library(socketcan INTERFACE)
target_include_directories(socketcan INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(socketcan INTERFACE cxx_std_17)

# ===============================
# Install rules
# ===============================
install(FILES ${HEADERS} DESTINATION include)

# ===============================
# Tests (only if enabled)
# ===============================
set(BUILD_TESTING OFF CACHE BOOL "Build and enable tests")

if(BUILD_TESTING)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
