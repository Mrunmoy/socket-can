# Collect test sources
set(TEST_SOURCES
    UnitTests.cpp
    CanbusIntegrationTests.cpp
    CanbusUnitTests.cpp
)

# Locate GTest — prefer the CMake package, fall back to manual paths
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    # libgtest-dev installs static archives but no CMake config on some distros;
    # build a minimal imported target manually.
    add_library(GTest::GTest STATIC IMPORTED)
    set_target_properties(GTest::GTest PROPERTIES
        IMPORTED_LOCATION /usr/lib/x86_64-linux-gnu/libgtest.a
        INTERFACE_INCLUDE_DIRECTORIES /usr/include
    )
    add_library(GTest::Main STATIC IMPORTED)
    set_target_properties(GTest::Main PROPERTIES
        IMPORTED_LOCATION /usr/lib/x86_64-linux-gnu/libgtest_main.a
        INTERFACE_INCLUDE_DIRECTORIES /usr/include
    )
endif()

# Build test executable
add_executable(socketcan_tests
    ${TEST_SOURCES}
)

# Link against the main library + GTest
# Use static library for test isolation
target_link_libraries(socketcan_tests PRIVATE socketcan_static GTest::GTest GTest::Main pthread)

# --------------------------------------------------------------
# Register main test with CTest
# --------------------------------------------------------------
add_test(NAME socketcan_tests
         COMMAND socketcan_tests)

# --------------------------------------------------------------
# vcan0 setup fixture
# --------------------------------------------------------------

# 1. Add setup_vcan test that runs the script
add_test(
    NAME setup_vcan
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/setup_vcan.sh
)

# 2. Mark this test as a fixture setup step
set_tests_properties(setup_vcan PROPERTIES
    FIXTURES_SETUP VcanSetup
)

# 3. Require the fixture for the main test
set_tests_properties(socketcan_tests PROPERTIES
    FIXTURES_REQUIRED VcanSetup
)


# Register GTest tests individually
include(GoogleTest)
gtest_discover_tests(socketcan_tests)
