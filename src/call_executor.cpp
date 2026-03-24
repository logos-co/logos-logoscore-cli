#include "call_executor.h"
#include "logos_core.h"
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>
#include <iostream>

struct CallResult {
    bool completed = false;
    bool success = false;
    QString message;
};

static void methodCallCallback(int result, const char* message, void* user_data) {
    CallResult* callResult = static_cast<CallResult*>(user_data);
    callResult->completed = true;
    callResult->success = (result == 1);
    callResult->message = message ? QString::fromUtf8(message) : QString();
}

QString CallExecutor::resolveParam(const QString& param) {
    if (param.startsWith('@')) {
        QString filePath = param.mid(1);
        QFile file(filePath);
        
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Failed to open file:" << filePath;
            return QString();
        }
        
        QTextStream in(&file);
        QString content = in.readAll();
        file.close();
        
        return content;
    }
    
    return param;
}

QJsonValue CallExecutor::convertParam(const QString& param) {
    if (param.toLower() == "true") {
        return QJsonValue(true);
    }
    if (param.toLower() == "false") {
        return QJsonValue(false);
    }
    
    bool ok;
    int intVal = param.toInt(&ok);
    if (ok) {
        return QJsonValue(intVal);
    }
    
    double doubleVal = param.toDouble(&ok);
    if (ok) {
        return QJsonValue(doubleVal);
    }
    
    return QJsonValue(param);
}

QString CallExecutor::buildParamsJson(const QStringList& params) {
    QJsonArray paramsArray;
    
    for (int i = 0; i < params.size(); ++i) {
        QString rawParam = params[i];
        QString resolvedParam = resolveParam(rawParam);
        
        if (rawParam.startsWith('@') && resolvedParam.isEmpty()) {
            qWarning() << "Failed to resolve file parameter:" << rawParam;
            return QString();
        }
        
        QJsonObject paramObj;
        paramObj["name"] = QString("arg%1").arg(i);
        paramObj["value"] = resolvedParam;
        
        QJsonValue converted = convertParam(resolvedParam);
        if (converted.isBool()) {
            paramObj["type"] = "bool";
            paramObj["value"] = resolvedParam;
        } else if (converted.isDouble()) {
            bool ok;
            resolvedParam.toInt(&ok);
            if (ok) {
                paramObj["type"] = "int";
            } else {
                paramObj["type"] = "double";
            }
            paramObj["value"] = resolvedParam;
        } else {
            paramObj["type"] = "QString";
            paramObj["value"] = resolvedParam;
        }
        
        paramsArray.append(paramObj);
    }
    
    QJsonDocument doc(paramsArray);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

bool CallExecutor::executeCall(const ModuleCall& call) {
    qDebug() << "Executing call:" << call.moduleName << "." << call.methodName 
             << "with" << call.params.size() << "params";
    
    QString paramsJson = buildParamsJson(call.params);
    if (call.params.size() > 0 && paramsJson.isEmpty()) {
        std::cerr << "Error: Failed to build parameters for " 
                  << call.moduleName.toStdString() << "." 
                  << call.methodName.toStdString() << std::endl;
        return false;
    }
    
    CallResult result;
    
    logos_core_call_plugin_method_async(
        call.moduleName.toUtf8().constData(),
        call.methodName.toUtf8().constData(),
        paramsJson.isEmpty() ? "[]" : paramsJson.toUtf8().constData(),
        methodCallCallback,
        &result
    );
    
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(30000);
    
    QTimer pollTimer;
    pollTimer.setInterval(100);
    
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&pollTimer, &QTimer::timeout, [&]() {
        if (result.completed) {
            loop.quit();
        }
    });
    
    timeoutTimer.start();
    pollTimer.start();
    loop.exec();
    
    pollTimer.stop();
    timeoutTimer.stop();
    
    if (!result.completed) {
        std::cerr << "Error: Timeout waiting for " 
                  << call.moduleName.toStdString() << "." 
                  << call.methodName.toStdString() << std::endl;
        return false;
    }
    
    if (!result.success) {
        std::cerr << "Error: " << result.message.toStdString() << std::endl;
        return false;
    }
    
    std::cout << result.message.toStdString() << std::endl;
    
    return true;
}

int CallExecutor::executeCalls(const QList<ModuleCall>& calls) {
    for (const ModuleCall& call : calls) {
        if (!executeCall(call)) {
            return 1;
        }
    }
    return 0;
}
