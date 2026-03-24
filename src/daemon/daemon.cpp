#include "daemon.h"
#include "connection_file.h"
#include "../config.h"
#include "logos_core.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <token_manager.h>
#include "../core_service/core_service_impl.h"

#include <QCoreApplication>
#include <QUuid>
#include <QDebug>

#include <csignal>
#include <unistd.h>

static volatile sig_atomic_t g_shutdownRequested = 0;

void Daemon::signalHandler(int signal)
{
    Q_UNUSED(signal);
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

int Daemon::start(int argc, char* argv[], const QStringList& modulesDirs)
{
    // 1. Generate instance ID BEFORE core init, so logos_host inherits it
    QString instanceId = QUuid::createUuid().toString(QUuid::Id128).left(12);
    QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qint64 pid = QCoreApplication::applicationPid();

    qputenv("LOGOS_INSTANCE_ID", instanceId.toUtf8());

    // 2. Initialize logos core
    logos_core_init(argc, argv);

    // 3. Add plugin directories — user-specified and bundled
    for (const QString& dir : modulesDirs) {
        logos_core_add_plugins_dir(dir.toUtf8().constData());
    }
    QByteArray bundledDir = qgetenv("LOGOS_BUNDLED_MODULES_DIR");
    if (!bundledDir.isEmpty()) {
        logos_core_add_plugins_dir(bundledDir.constData());
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
    TokenManager::instance().saveToken("cli_client", token);

    // 6. Write connection file
    if (!ConnectionFile::write(instanceId, token, pid, modulesDirs)) {
        qCritical() << "Failed to write connection file:" << ConnectionFile::filePath();
        return 1;
    }

    fprintf(stderr, "Logoscore daemon started (pid %lld, instance %s)\n",
            static_cast<long long>(pid), instanceId.toUtf8().constData());
    fprintf(stderr, "Connection file: %s\n", ConnectionFile::filePath().toUtf8().constData());
    fflush(stderr);

    // 7. Set up signal handlers for clean shutdown
    setupSignalHandlers();

    // 8. Run Qt event loop (blocks)
    int result = logos_core_exec();

    // 9. Cleanup
    fprintf(stderr, "Shutting down logoscore daemon...\n");
    fflush(stderr);

    logos_core_cleanup();
    ConnectionFile::remove();

    delete coreServiceImpl;
    delete coreServiceApi;

    fprintf(stderr, "Logoscore daemon stopped.\n");
    fflush(stderr);

    return result;
}
