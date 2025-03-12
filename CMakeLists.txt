cmake_minimum_required(VERSION 3.16)
project(SocketCan LANGUAGES CXX)

# ===============================
# Library sources
# ===============================
set(SOURCES
    SocketCan.cpp
)

set(HEADERS
    SocketCan.hpp
)

# ===============================
# Shared library
# ===============================
add_library(socketcan SHARED ${SOURCES})
target_include_directories(socketcan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(socketcan PUBLIC cxx_std_17)
set_target_properties(socketcan PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ===============================
# Static library
# ===============================
add_library(socketcan_static STATIC ${SOURCES})
target_include_directories(socketcan_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(socketcan_static PUBLIC cxx_std_17)
set_target_properties(socketcan_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ===============================
# Install rules
# ===============================
install(TARGETS socketcan socketcan_static
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

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
