// Unit tests for ListenerRegistry, the fixed-capacity (zero heap) observer
// list that BasicCanbus uses for both Classic and CAN FD listeners.

#include <gtest/gtest.h>
#include "SocketCan.hpp"

#include <vector>

using namespace SocketCAN;

namespace
{
    struct RecordingListener : IClassicFrameListener
    {
        std::vector<uint32_t> ids;
        void onClassicFrame(const Frame& f) override { ids.push_back(f.id); }
    };

    using Registry = ListenerRegistry<IClassicFrameListener, 2>;
}

TEST(ListenerRegistry, StartsEmpty)
{
    Registry r;
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(Registry::capacity(), 2u);
}

TEST(ListenerRegistry, RejectsNull)
{
    Registry r;
    EXPECT_THROW(r.add(nullptr), CanError);
}

TEST(ListenerRegistry, RejectsWhenFull)
{
    Registry r;
    RecordingListener a, b, c;
    r.add(&a);
    r.add(&b);
    EXPECT_THROW(r.add(&c), CanError);
    EXPECT_EQ(r.size(), 2u);
}

TEST(ListenerRegistry, NotifiesEveryListenerInOrder)
{
    Registry r;
    RecordingListener a, b;
    r.add(&a);
    r.add(&b);

    Frame f;
    f.id = 0x42;
    r.forEach([&](IClassicFrameListener& l) { l.onClassicFrame(f); });

    EXPECT_EQ(a.ids, std::vector<uint32_t>{0x42});
    EXPECT_EQ(b.ids, std::vector<uint32_t>{0x42});
}

TEST(ListenerRegistry, ClearRemovesAll)
{
    Registry r;
    RecordingListener a;
    r.add(&a);
    r.clear();
    EXPECT_EQ(r.size(), 0u);
}
