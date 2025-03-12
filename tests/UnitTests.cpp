#include <gtest/gtest.h>
#include "SocketCan.hpp"

TEST(SocketCan, BasicConstruction) {
    // Basic smoke test — doesn’t require kernel vcan0
    SocketCAN::Frame f;
    EXPECT_EQ(f.id, 0u);
    EXPECT_FALSE(f.extended);
    EXPECT_FALSE(f.rtr);
    EXPECT_EQ(f.dlc, 0);
}
