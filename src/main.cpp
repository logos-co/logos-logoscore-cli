#include <QCoreApplication>
#include <QDebug>
#include <cstdio>
#include "logos_core.h"
#include "command_line_parser.h"
#include "call_executor.h"

// Custom message handler that ensures immediate flushing of output
// We do this due to github actions timing out
void flushingMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    QByteArray localMsg = msg.toLocal8Bit();
    const char *file = context.file ? context.file : "";
    const char *function = context.function ? context.function : "";
    
    switch (type) {
    case QtDebugMsg:
        fprintf(stderr, "Debug: %s\n", localMsg.constData());
        break;
    case QtInfoMsg:
        fprintf(stderr, "Info: %s\n", localMsg.constData());
        break;
    case QtWarningMsg:
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
    fflush(stderr);  // Flush after every message
}

int main(int argc, char *argv[]) {
    // Install custom message handler before any Qt operations
    qInstallMessageHandler(flushingMessageHandler);

    QCoreApplication app(argc, argv);
    app.setApplicationName("logoscore");
    app.setApplicationVersion("1.0");

    CoreArgs args = parseCommandLineArgs(app);
    if (!args.valid) {
        return 1;
    }

    logos_core_init(argc, argv);
    
    for (const QString& dir : args.modulesDirs) {
        logos_core_add_plugins_dir(dir.toUtf8().constData());
    }
    
    logos_core_start();
    
    if (!args.loadModules.isEmpty()) {
        for (const QString& moduleName : args.loadModules) {
            QString trimmed = moduleName.trimmed();
            if (trimmed.isEmpty())
                continue;
            if (!logos_core_load_plugin_with_dependencies(trimmed.toUtf8().constData())) {
                qWarning() << "Failed to load module:" << trimmed;
            }
        }
    }
    
    if (!args.calls.isEmpty()) {
        int callResult = CallExecutor::executeCalls(args.calls);
        if (args.quitOnFinish || callResult != 0) {
            return callResult;
        }
    }

    return logos_core_exec();
}
