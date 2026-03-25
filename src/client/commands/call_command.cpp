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

    // Resolve @file parameters and coerce types
    QVariantList resolvedArgs;
    for (const QString& arg : methodArgs) {
        QString resolved = resolveFileParam(arg);
        if (arg.startsWith('@') && resolved.isEmpty()) {
            output().printError("INVALID_ARGS",
                               QString("Failed to read file: %1").arg(arg.mid(1)));
            return 1;
        }

        // Try to coerce to native types so the RPC target can match signatures
        if (resolved == "true") {
            resolvedArgs.append(true);
        } else if (resolved == "false") {
            resolvedArgs.append(false);
        } else {
            bool isInt = false;
            int intVal = resolved.toInt(&isInt);
            if (isInt) {
                resolvedArgs.append(intVal);
            } else {
                bool isDouble = false;
                double dblVal = resolved.toDouble(&isDouble);
                if (isDouble) {
                    resolvedArgs.append(dblVal);
                } else {
                    resolvedArgs.append(resolved);
                }
            }
        }
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
        } else if (resultValue.isDouble()) {
            // Handles both integers and floating point
            double d = resultValue.toDouble();
            if (d == static_cast<int>(d))
                output().printRaw(QString::number(static_cast<int>(d)));
            else
                output().printRaw(QString::number(d));
        } else if (resultValue.isBool()) {
            output().printRaw(resultValue.toBool() ? "true" : "false");
        } else if (resultValue.isNull() || resultValue.isUndefined()) {
            // Nothing useful to print
        } else if (resultValue.isArray()) {
            QJsonDocument doc(resultValue.toArray());
            output().printRaw(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
        } else if (resultValue.isObject()) {
            QJsonDocument doc(resultValue.toObject());
            output().printRaw(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
        }
    }

    return 0;
}
