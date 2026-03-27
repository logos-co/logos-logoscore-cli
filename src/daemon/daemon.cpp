#include "daemon.h"
#include "connection_file.h"
#include "../config.h"
#include "../paths.h"
#include "logos_core.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <token_manager.h>
#include "../core_service/core_service_impl.h"

#include <QCoreApplication>
#include <QString>

#include <uuid.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

static volatile sig_atomic_t g_shutdownRequested = 0;

void Daemon::signalHandler(int signal)
{
    (void)signal;
    g_shutdownRequested = 1;

    // Post a quit event to the Qt event loop
    if (QCoreApplication::instance())
        QCoreApplication::quit();
}

void Daemon::setupSignalHandlers()
{
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int Daemon::start(int argc, char* argv[], const std::vector<std::string>& modulesDirs)
{
    // 1. Generate instance ID BEFORE core init, so logos_host inherits it
    std::random_device rd;
    std::mt19937 gen(rd());
    uuids::uuid_random_generator uuidGen(gen);

    // instanceId: 12 hex chars (no dashes), like QUuid::Id128.left(12)
    std::string fullUuid = uuids::to_string(uuidGen());
    std::string instanceId;
    for (char c : fullUuid)
        if (c != '-') instanceId += c;
    instanceId = instanceId.substr(0, 12);

    std::string token = uuids::to_string(uuidGen());
    int64_t pid = getpid();

    setenv("LOGOS_INSTANCE_ID", instanceId.c_str(), 1);

    // 2. Initialize logos core
    logos_core_init(argc, argv);

    // 3. Add plugin directories — user-specified and bundled
    //    Resolve to absolute paths: logos_core cannot load plugin metadata from relative paths.
    for (const std::string& dir : modulesDirs) {
        std::error_code ec;
        std::string absDir = std::filesystem::absolute(dir, ec).string();
        logos_core_add_plugins_dir(ec ? dir.c_str() : absDir.c_str());
    }

    std::string bundledDir = paths::bundledModulesDir();
    if (!bundledDir.empty()) {
        logos_core_add_plugins_dir(bundledDir.c_str());
    }

    // 4. Start core (discover plugins, launch logos_host in remote mode)
    logos_core_start();

    // 5. Register core_service as an in-process module via the C++ SDK.
    //    This publishes it via Qt RemoteObjects so CLI clients can connect.
    auto* coreServiceApi = new LogosAPI("core_service");
    auto* coreServiceImpl = new CoreServiceImpl();

    // Initialize core_service with its LogosAPI so it can proxy calls
    coreServiceImpl->init(coreServiceApi);

    // Publish core_service as a remote object
    auto* provider = coreServiceApi->getProvider();
    provider->registerObject("core_service", static_cast<LogosProviderObject*>(coreServiceImpl));

    // Save the client token so core_service can authenticate CLI clients
    TokenManager::instance().saveToken("cli_client", QString::fromStdString(token));

    // 6. Write connection file
    if (!ConnectionFile::write(instanceId, token, pid, modulesDirs)) {
        fprintf(stderr, "Failed to write connection file: %s\n", ConnectionFile::filePath().c_str());
        return 1;
    }

    fprintf(stdout, "Logoscore daemon started (pid %lld, instance %s)\n",
            static_cast<long long>(pid), instanceId.c_str());
    fprintf(stdout, "Connection file: %s\n", ConnectionFile::filePath().c_str());
    fflush(stdout);

    // 7. Set up signal handlers for clean shutdown
    setupSignalHandlers();

    // 8. Run Qt event loop (blocks)
    int result = logos_core_exec();

    // 9. Cleanup
    fprintf(stdout, "Shutting down logoscore daemon...\n");
    fflush(stdout);

    logos_core_cleanup();
    ConnectionFile::remove();

    delete coreServiceImpl;
    delete coreServiceApi;

    fprintf(stdout, "Logoscore daemon stopped.\n");
    fflush(stdout);

    return result;
}
