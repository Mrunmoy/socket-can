# Collect test sources (for now just one file)
set(TEST_SOURCES
    UnitTests.cpp
)

# Build test executable
add_executable(socketcan_tests
    ${TEST_SOURCES}
)

# Link against the main library + GTest
target_link_libraries(socketcan_tests
    PRIVATE
        socketcan
        gtest
        gtest_main
)

# Register test with CTest
add_test(NAME socketcan_tests
         COMMAND socketcan_tests)
