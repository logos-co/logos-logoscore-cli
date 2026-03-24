#include "call_command.h"
#include <QFile>
#include <QTextStream>

QString CallCommand::resolveFileParam(const QString& param)
{
    if (!param.startsWith('@'))
        return param;

    QString filePath = param.mid(1);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QTextStream in(&file);
    QString content = in.readAll();
    file.close();
    return content;
}

int CallCommand::execute(const QStringList& args)
{
    // Parse args: either "call <module> <method> [args...]"
    // or "module <name> method <method> [args...]"
    QString moduleName;
    QString methodName;
    QStringList methodArgs;

    if (args.isEmpty()) {
        output().printError("INVALID_ARGS",
                           "Usage: logoscore call <module> <method> [args...]");
        return 1;
    }

    // Check for verbose "module <name> method <method>" syntax
    // args[0] is the first arg after the subcommand keyword
    if (args.size() >= 1) {
        // For "logoscore call <module> <method> [args...]"
        // args = [<module>, <method>, args...]
        // For "logoscore module <name> method <method> [args...]"
        // main.cpp passes the subcommand as "module", and args = [<name>, method, <method>, args...]
        int i = 0;

        // Check if this is the verbose form: first arg after "module" subcommand
        // In that case args = [<name>, "method", <method>, ...]
        if (args.size() >= 3 && args.at(1) == "method") {
            moduleName = args.at(0);
            methodName = args.at(2);
            for (int j = 3; j < args.size(); ++j)
                methodArgs.append(args.at(j));
        } else {
            // Short form: <module> <method> [args...]
            if (args.size() < 2) {
                output().printError("INVALID_ARGS",
                                   "Usage: logoscore call <module> <method> [args...]");
                return 1;
            }
            moduleName = args.at(0);
            methodName = args.at(1);
            for (int j = 2; j < args.size(); ++j)
                methodArgs.append(args.at(j));
        }
    }

    if (moduleName.isEmpty() || methodName.isEmpty()) {
        output().printError("INVALID_ARGS",
                           "Usage: logoscore call <module> <method> [args...]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    // Resolve @file parameters
    QVariantList resolvedArgs;
    for (const QString& arg : methodArgs) {
        QString resolved = resolveFileParam(arg);
        if (arg.startsWith('@') && resolved.isEmpty()) {
            output().printError("INVALID_ARGS",
                               QString("Failed to read file: %1").arg(arg.mid(1)));
            return 1;
        }
        resolvedArgs.append(resolved);
    }

    QJsonObject result = client().callModuleMethod(moduleName, methodName, resolvedArgs);

    QString status = result.value("status").toString();
    if (status == "error") {
        QString code = result.value("code").toString();
        int exitCode = 4; // method error
        if (code == "MODULE_NOT_LOADED" || code == "MODULE_NOT_FOUND")
            exitCode = 3;
        output().printError(code, result.value("message").toString(), result);
        return exitCode;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        // Print the result value in human mode
        QJsonValue resultValue = result.value("result");
        if (resultValue.isString()) {
            output().printRaw(resultValue.toString());
        } else {
            // Print as JSON for structured results
            QJsonDocument doc;
            if (resultValue.isArray())
                doc = QJsonDocument(resultValue.toArray());
            else if (resultValue.isObject())
                doc = QJsonDocument(resultValue.toObject());
            else
                doc = QJsonDocument(result);
            output().printRaw(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
        }
    }

    return 0;
}
