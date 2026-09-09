// Which package-module calls the daemon gives a real deadline.
//
// `package install blockchain_module` failed at the download step on any box
// slower than ~7 MB/s: package_downloader fetches the whole 148 MB inside ONE
// call, and that call ran on the transport default of 20 s. The client's wait
// on the daemon had been raised for exactly this (client.cpp); the hop below it
// had not. These pin the budget per method so the default cannot creep back.

#include <gtest/gtest.h>

#include "rpc_deadlines.h"

using rpc_deadlines::forPackageCall;

TEST(RpcDeadlines, TransferCallsGetTheTransferBudget) {
    for (const char* m : {"downloadResolvedDependencies", "downloadPinned",
                          "inspectPackage", "installPlugin"})
        EXPECT_EQ(forPackageCall(m).ms, rpc_deadlines::kTransferMs) << m;
}

TEST(RpcDeadlines, CatalogResolutionGetsTheCatalogBudget) {
    EXPECT_EQ(forPackageCall("resolveDependencies").ms, rpc_deadlines::kCatalogMs);
}

TEST(RpcDeadlines, LocalCallsKeepTheTransportDefault) {
    for (const char* m : {"getInstalledPackages", "requestInstall", "ackPendingAction",
                          "confirmInstall", "resolveFlatDependents", "notAPackageMethod"})
        EXPECT_EQ(forPackageCall(m).ms, Timeout().ms) << m;
}

// The budgets only mean something if they exceed the default they replace.
TEST(RpcDeadlines, BudgetsExceedTheTransportDefault) {
    EXPECT_GT(rpc_deadlines::kCatalogMs, Timeout().ms);
    EXPECT_GT(rpc_deadlines::kTransferMs, rpc_deadlines::kCatalogMs);
}
