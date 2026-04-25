#include "daemon.h"
#include "connection_file.h"
#include "../config.h"
#include "../paths.h"
#include "logos_core.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <logos_transport_config.h>
#include <token_manager.h>
#include "../core_service/core_service_impl.h"

#include <QCoreApplication>
#include <QDebug>
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

int Daemon::start(int argc, char* argv[], const std::vector<std::string>& modulesDirs,
                  const std::string& persistencePath,
                  const std::vector<TransportInfo>& transports)
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
        const char* resolved = ec ? dir.c_str() : absDir.c_str();
        qDebug() << "Added plugins directory:" << resolved;
        logos_core_add_plugins_dir(resolved);
    }

    std::string bundledDir = paths::bundledModulesDir();
    if (!bundledDir.empty()) {
        logos_core_add_plugins_dir(bundledDir.c_str());
        qDebug() << "Added bundled modules directory:" << bundledDir.c_str();
    }

    // 4. Set persistence base path for module instance data
    std::string persistenceBase = persistencePath.empty()
        ? Config::configDir().toStdString() + "/data"
        : persistencePath;
    logos_core_set_persistence_base_path(persistenceBase.c_str());

    // 5. Start core (discover plugins, launch logos_host in remote mode)
    logos_core_start();

    // 6. Register core_service as an in-process module via the C++ SDK.
    //    core_service can publish on multiple transports simultaneously:
    //    a local QLocalSocket (back-compat) + any TCP / TCP+SSL listeners
    //    specified via --transport. Modules continue to use the process-
    //    global LocalSocket default — only core_service is exposed
    //    externally.
    LogosTransportSet coreTransports;
    std::vector<TransportInfo> advertisedTransports;
    auto pushAdvertised = [&](const TransportInfo& t) { advertisedTransports.push_back(t); };

    // Always include the local-socket transport so on-host CLI keeps working.
    {
        LogosTransportConfig c;
        c.protocol = LogosProtocol::LocalSocket;
        coreTransports.push_back(std::move(c));
        // `local` advertises just the protocol — no host/port/cert/key/ca.
        // Construct field-by-field rather than via brace-init so adding new
        // fields to TransportInfo doesn't silently corrupt the ordering.
        TransportInfo localInfo;
        localInfo.protocol = "local";
        pushAdvertised(localInfo);
    }

    for (const auto& t : transports) {
        if (t.protocol == "local") continue;  // already added
        LogosTransportConfig c;
        c.host = t.host;
        c.port = t.port;
        c.certFile   = t.certFile;
        c.keyFile    = t.keyFile;
        c.caFile     = t.caFile;
        c.verifyPeer = t.verifyPeer;
        // codec: plain transports honor the user's pick; "json" is the
        // only implementation today but the plumbing is end-to-end so
        // adding "cbor" (or future codecs) is a single enum arm away.
        if (t.codec == "cbor")      c.codec = LogosWireCodec::Cbor;
        else                        c.codec = LogosWireCodec::Json;
        if (t.protocol == "tcp")     c.protocol = LogosProtocol::Tcp;
        else if (t.protocol == "tcp_ssl") c.protocol = LogosProtocol::TcpSsl;
        else continue;
        coreTransports.push_back(std::move(c));
        pushAdvertised(t);
    }

    auto* coreServiceApi = new LogosAPI(QString("core_service"), coreTransports);
    auto* coreServiceImpl = new CoreServiceImpl();

    coreServiceImpl->init(coreServiceApi);
    auto* provider = coreServiceApi->getProvider();
    provider->registerObject("core_service", static_cast<LogosProviderObject*>(coreServiceImpl));

    TokenManager::instance().saveToken("cli_client", QString::fromStdString(token));

    // 7. Write connection file — includes the advertised transports so
    //    remote clients know how to reach us.
    if (!ConnectionFile::write(instanceId, token, pid, modulesDirs, advertisedTransports)) {
        fprintf(stderr, "Failed to write connection file: %s\n", ConnectionFile::filePath().c_str());
        return 1;
    }

    fprintf(stdout, "Logoscore daemon started (pid %lld, instance %s)\n",
            static_cast<long long>(pid), instanceId.c_str());
    fprintf(stdout, "Connection file: %s\n", ConnectionFile::filePath().c_str());
    fflush(stdout);

    // 8. Set up signal handlers for clean shutdown
    setupSignalHandlers();

    // 9. Run Qt event loop (blocks)
    int result = QCoreApplication::exec();

    // 10. Cleanup
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
