#include "command_line_parser.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>

static QStringList parseParams(const QString& paramsStr) {
    QStringList result;
    QString current;
    QChar quoteChar;
    bool inQuote = false;
    
    for (int i = 0; i < paramsStr.length(); ++i) {
        QChar c = paramsStr[i];
        
        if (!inQuote) {
            if (c == '\'' || c == '"') {
                inQuote = true;
                quoteChar = c;
            } else if (c == ',') {
                QString trimmed = current.trimmed();
                if (!trimmed.isEmpty()) {
                    result.append(trimmed);
                }
                current.clear();
            } else {
                current += c;
            }
        } else {
            if (c == quoteChar) {
                inQuote = false;
            } else {
                current += c;
            }
        }
    }
    
    QString trimmed = current.trimmed();
    if (!trimmed.isEmpty()) {
        result.append(trimmed);
    }
    
    return result;
}

static ModuleCall parseCallString(const QString& callStr) {
    ModuleCall call;
    
    int dotIndex = callStr.indexOf('.');
    if (dotIndex == -1) {
        qWarning() << "Invalid call syntax (no dot found):" << callStr;
        return call;
    }
    
    call.moduleName = callStr.left(dotIndex).trimmed();
    
    int parenStart = callStr.indexOf('(', dotIndex);
    if (parenStart == -1) {
        call.methodName = callStr.mid(dotIndex + 1).trimmed();
        return call;
    }
    
    call.methodName = callStr.mid(dotIndex + 1, parenStart - dotIndex - 1).trimmed();
    
    int parenEnd = callStr.lastIndexOf(')');
    if (parenEnd == -1 || parenEnd <= parenStart) {
        qWarning() << "Invalid call syntax (mismatched parentheses):" << callStr;
        return call;
    }
    
    QString paramsStr = callStr.mid(parenStart + 1, parenEnd - parenStart - 1);
    call.params = parseParams(paramsStr);
    
    return call;
}

CoreArgs parseCommandLineArgs(QCoreApplication& app) {
    CoreArgs args;
    args.valid = false;

    QCommandLineParser parser;
    parser.setApplicationDescription("Logos Core - Plugin-based application framework");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption modulesDirOption(
        QStringList() << "modules-dir" << "m",
        "Directory to scan for modules (repeatable)",
        "path"
    );
    parser.addOption(modulesDirOption);

    QCommandLineOption loadModulesOption(
        QStringList() << "load-modules" << "l",
        "Comma-separated list of modules to load in order",
        "modules"
    );
    parser.addOption(loadModulesOption);

    QCommandLineOption callOption(
        QStringList() << "call" << "c",
        "Call a module method: module.method(param1, param2). Use @file to read param from file. Can be repeated.",
        "call"
    );
    parser.addOption(callOption);

    QCommandLineOption quitOnFinishOption(
        QStringList() << "quit-on-finish",
        "Exit after all -c calls complete (exit 0 on success, 1 on failure)"
    );
    parser.addOption(quitOnFinishOption);

    parser.process(app);

    if (parser.isSet(modulesDirOption)) {
        args.modulesDirs = parser.values(modulesDirOption);
    }

    if (parser.isSet(loadModulesOption)) {
        QString modulesList = parser.value(loadModulesOption);
        args.loadModules = modulesList.split(',', Qt::SkipEmptyParts);
    }

    if (parser.isSet(callOption)) {
        QStringList callStrings = parser.values(callOption);
        for (const QString& callStr : callStrings) {
            ModuleCall call = parseCallString(callStr);
            if (!call.moduleName.isEmpty() && !call.methodName.isEmpty()) {
                args.calls.append(call);
            } else {
                qWarning() << "Skipping invalid call:" << callStr;
            }
        }
    }

    args.quitOnFinish = parser.isSet(quitOnFinishOption);
    args.valid = true;
    return args;
}
