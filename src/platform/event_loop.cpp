#include "event_loop.h"

#include <QCoreApplication>
#include <QObject>

#include <cstdio>
#include <cstdlib>

static bool s_verbose = false;

static void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    QByteArray localMsg = msg.toLocal8Bit();
    const char* file = context.file ? context.file : "";
    const char* function = context.function ? context.function : "";

    switch (type) {
    case QtDebugMsg:
        if (!s_verbose) return;
        fprintf(stderr, "Debug: %s\n", localMsg.constData());
        break;
    case QtInfoMsg:
        if (!s_verbose) return;
        fprintf(stderr, "Info: %s\n", localMsg.constData());
        break;
    case QtWarningMsg:
        if (!s_verbose) return;
        fprintf(stderr, "Warning: %s\n", localMsg.constData());
        break;
    case QtCriticalMsg:
        fprintf(stderr, "Critical: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
        break;
    case QtFatalMsg:
        fprintf(stderr, "Fatal: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
        fflush(stderr);
        abort();
    }
    fflush(stderr);
}

void EventLoop::init(int argc, char* argv[],
                     const std::string& appName,
                     const std::string& appVersion)
{
    static QCoreApplication* app = nullptr;
    if (!app) {
        app = new QCoreApplication(argc, argv);
    }
    app->setApplicationName(QString::fromStdString(appName));
    app->setApplicationVersion(QString::fromStdString(appVersion));
}

int EventLoop::exec()
{
    return QCoreApplication::exec();
}

void EventLoop::quit()
{
    if (QCoreApplication::instance())
        QCoreApplication::quit();
}

void EventLoop::onAboutToQuit(std::function<void()> callback)
{
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     std::move(callback));
}

std::string EventLoop::applicationVersion()
{
    return QCoreApplication::applicationVersion().toStdString();
}

void EventLoop::installLogFilter(bool verbose)
{
    s_verbose = verbose;
    qInstallMessageHandler(messageHandler);
}
