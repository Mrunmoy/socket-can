# Collect test sources
set(TEST_SOURCES
    CanbusIntegrationTests.cpp
    CanbusUnitTests.cpp
)

# Locate GTest — prefer the CMake package, fall back to manual paths
find_package(GTest QUIET)

if(TARGET GTest::gtest AND NOT TARGET GTest::GTest)
    add_library(GTest::GTest ALIAS GTest::gtest)
endif()
if(TARGET GTest::gtest_main AND NOT TARGET GTest::Main)
    add_library(GTest::Main ALIAS GTest::gtest_main)
endif()

if(NOT TARGET GTest::GTest OR NOT TARGET GTest::Main)
    # libgtest-dev installs static archives but no CMake config on some distros.
    find_path(GTEST_INCLUDE_DIR gtest/gtest.h)
    find_library(GTEST_LIBRARY NAMES gtest)
    find_library(GTEST_MAIN_LIBRARY NAMES gtest_main)

    if(NOT GTEST_INCLUDE_DIR OR NOT GTEST_LIBRARY OR NOT GTEST_MAIN_LIBRARY)
        message(FATAL_ERROR "GTest not found. Install GoogleTest development package.")
    endif()

    add_library(GTest::GTest STATIC IMPORTED)
    set_target_properties(GTest::GTest PROPERTIES
        IMPORTED_LOCATION "${GTEST_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GTEST_INCLUDE_DIR}"
    )
    add_library(GTest::Main STATIC IMPORTED)
    set_target_properties(GTest::Main PROPERTIES
        IMPORTED_LOCATION "${GTEST_MAIN_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GTEST_INCLUDE_DIR}"
    )
endif()

find_package(Threads REQUIRED)

# Build test executable
add_executable(socketcan_tests
    ${TEST_SOURCES}
)

# Link against the (header-only) library + GTest
target_link_libraries(socketcan_tests PRIVATE socketcan GTest::GTest GTest::Main Threads::Threads)

# --------------------------------------------------------------
# Register main test with CTest
# --------------------------------------------------------------
# The integration tests need vcan0, which the dev container's entrypoint brings
# up on start (see .devcontainer/). When running outside the container, create
# it once with:
#   sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
add_test(NAME socketcan_tests
         COMMAND socketcan_tests)

# Register GTest tests individually
include(GoogleTest)
gtest_discover_tests(socketcan_tests)
