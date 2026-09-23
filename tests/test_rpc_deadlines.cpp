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
        EXPECT_EQ(forPackageCall(m), rpc_deadlines::kTransferMs) << m;
}

TEST(RpcDeadlines, CatalogResolutionGetsTheCatalogBudget) {
    EXPECT_EQ(forPackageCall("resolveDependencies"), rpc_deadlines::kCatalogMs);
}

TEST(RpcDeadlines, LocalCallsKeepTheTransportDefault) {
    for (const char* m : {"getInstalledPackages", "requestInstall", "ackPendingAction",
                          "confirmInstall", "resolveFlatDependents", "notAPackageMethod"})
        EXPECT_EQ(forPackageCall(m), 0) << m;
}

// Detector: both legs of a module call ran on the same 20 s default, so the
// client stopped waiting when the daemon did and a hung module read RPC_FAILED.
TEST(RpcDeadlines, TheClientOutwaitsTheDaemonsModuleCall) {
    EXPECT_GT(rpc_deadlines::kModuleCallReplyMs, rpc_deadlines::kModuleCallMs);
}

// The budgets only mean something if they exceed the default they replace.
TEST(RpcDeadlines, BudgetsExceedTheTransportDefault) {
    EXPECT_GT(rpc_deadlines::kCatalogMs, 20 * 1000);
    EXPECT_GT(rpc_deadlines::kTransferMs, rpc_deadlines::kCatalogMs);
}
