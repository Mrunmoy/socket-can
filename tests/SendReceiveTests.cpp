#include <gtest/gtest.h>
#include "SocketCan.hpp"

using namespace SocketCAN;

TEST(CanbusIntegration, SendAndReceiveOnVcan0)
{
    // These will throw SocketCAN::CanError if something is wrong
    Canbus writer{"vcan0"};
    Canbus reader{"vcan0"};

    Frame tx;
    tx.id       = 0x123;
    tx.extended = false;
    tx.rtr      = false;
    tx.dlc      = 4;
    tx.data[0]  = 0xDE;
    tx.data[1]  = 0xAD;
    tx.data[2]  = 0xBE;
    tx.data[3]  = 0xEF;

    // Send – will throw on error
    writer.send(tx);

    // Receive with a reasonable timeout
    Frame rx;
    ASSERT_TRUE(reader.receive(rx, std::chrono::milliseconds{100}))
        << "Timed out waiting for CAN frame on vcan0";

    EXPECT_EQ(rx.id, tx.id);
    EXPECT_EQ(rx.extended, tx.extended);
    EXPECT_EQ(rx.rtr, tx.rtr);
    EXPECT_EQ(rx.dlc, tx.dlc);

    for (int i = 0; i < rx.dlc; ++i)
    {
        EXPECT_EQ(rx.data[i], tx.data[i]);
    }
}
