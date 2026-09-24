#include "plain_rpc.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

// Detector: core_service serves calls concurrently, and two `watch` calls for one
// module pushed onto the same client's subscription list without a lock.
TEST(PlainRpcClient, ConcurrentSubscriptionsAreAllKept)
{
    logosctl::PlainRpcClient client("plain_rpc_fixture", "core_service");
    ASSERT_TRUE(client.valid());
    constexpr int kThreads = 8;
    constexpr int kEach = 200;
    std::atomic<int> subscribed{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kEach; ++i)
                if (client.subscribe("tick", [](const std::string&, const nlohmann::json&) {}))
                    ++subscribed;
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(subscribed.load(), kThreads * kEach);
    EXPECT_EQ(client.subscriptionCount(), static_cast<std::size_t>(kThreads * kEach));
}
