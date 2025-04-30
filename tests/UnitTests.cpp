#include <gtest/gtest.h>
#include "SocketCan.hpp"

TEST(SocketCan, ClassicFrameDefaultConstruction) {
    SocketCAN::Frame f;
    EXPECT_EQ(f.id, 0u);
    EXPECT_FALSE(f.extended);
    EXPECT_FALSE(f.rtr);
    EXPECT_EQ(f.dlc, 0);
    for (auto b : f.data)
        EXPECT_EQ(b, 0);
}

TEST(SocketCan, FdFrameDefaultConstruction) {
    SocketCAN::FdFrame f;
    EXPECT_EQ(f.id, 0u);
    EXPECT_FALSE(f.extended);
    EXPECT_FALSE(f.brs);
    EXPECT_FALSE(f.esi);
    EXPECT_EQ(f.len, 0);
    for (auto b : f.data)
        EXPECT_EQ(b, 0);
}

TEST(SocketCan, FdFrameMaxDataSize) {
    // Compile-time check: FD payload must hold 64 bytes
    static_assert(std::tuple_size<decltype(SocketCAN::FdFrame::data)>::value == 64,
                  "FdFrame data must be 64 bytes");
    EXPECT_EQ(SocketCAN::FdFrame{}.data.size(), 64u);
}
