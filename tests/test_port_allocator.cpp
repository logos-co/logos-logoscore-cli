#include <gtest/gtest.h>

#include "daemon/port_allocator.h"

#include <set>

TEST(PortAllocator, ReturnsNonZeroPort)
{
    uint16_t p = PortAllocator::allocateEphemeralTcp("127.0.0.1");
    EXPECT_GT(p, 0u);
}

TEST(PortAllocator, EmptyHostTreatedAsAnyAddress)
{
    // Empty host = "0.0.0.0". Allocator should still succeed.
    uint16_t p = PortAllocator::allocateEphemeralTcp("");
    EXPECT_GT(p, 0u);
}

TEST(PortAllocator, BadHostReturnsZero)
{
    // Allocator only accepts numeric IPv4 — hostname resolution is the
    // caller's responsibility. A garbage string should fail closed.
    uint16_t p = PortAllocator::allocateEphemeralTcp("not-an-ip");
    EXPECT_EQ(p, 0u);
}

TEST(PortAllocator, ConsecutiveAllocationsAreDistinct)
{
    // Two ephemeral allocations in a row should pick different ports
    // (the kernel rotates through the ephemeral range). This is the
    // critical property for the multi-module use case: the daemon
    // pre-allocates one port per module before spawning the children.
    std::set<uint16_t> seen;
    for (int i = 0; i < 10; ++i) {
        uint16_t p = PortAllocator::allocateEphemeralTcp("127.0.0.1");
        ASSERT_GT(p, 0u);
        seen.insert(p);
    }
    // Don't require *all* 10 distinct (kernels can reuse a just-freed
    // port immediately under SO_REUSEADDR), but expect at least a few
    // distinct values — a single-port answer would mean the rotation
    // is broken.
    EXPECT_GT(seen.size(), 1u);
}
