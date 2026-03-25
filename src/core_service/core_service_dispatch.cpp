// Manual dispatch code for CoreServiceImpl.
// This is equivalent to what logos-cpp-generator would produce from
// the LOGOS_METHOD declarations in core_service_impl.h.

#include "core_service_impl.h"
#include <QJsonObject>
#include <QJsonArray>

QVariant CoreServiceImpl::callMethod(const QString& methodName, const QVariantList& args)
{
    if (methodName == "loadModule" && args.size() >= 1)
        return loadModule(args.at(0).toString());

    if (methodName == "unloadModule" && args.size() >= 1)
        return unloadModule(args.at(0).toString());

    if (methodName == "reloadModule" && args.size() >= 1)
        return reloadModule(args.at(0).toString());

    if (methodName == "listModules") {
        QString filter = args.size() >= 1 ? args.at(0).toString() : "all";
        return QVariant::fromValue(listModules(filter));
    }

    if (methodName == "getStatus")
        return QVariant::fromValue(getStatus());

    if (methodName == "getModuleInfo" && args.size() >= 1)
        return QVariant::fromValue(getModuleInfo(args.at(0).toString()));

    if (methodName == "getModuleStats")
        return QVariant::fromValue(getModuleStats());

    if (methodName == "callModuleMethod" && args.size() >= 3)
        return callModuleMethod(args.at(0).toString(),
                                args.at(1).toString(),
                                args.at(2).toList());

    if (methodName == "watchModuleEvents" && args.size() >= 2)
        return QVariant::fromValue(watchModuleEvents(args.at(0).toString(),
                                                      args.at(1).toString()));

    if (methodName == "shutdown")
        return QVariant::fromValue(shutdown());

    return QVariant();
}

QJsonArray CoreServiceImpl::getMethods()
{
    QJsonArray methods;

    auto mkParam = [](const QString& name, const QString& type) {
        QJsonObject p;
        p["name"] = name;
        p["type"] = type;
        return p;
    };

    auto mkMethod = [&](const QString& name, const QJsonArray& params, const QString& ret) {
        QJsonObject m;
        m["name"] = name;
        m["params"] = params;
        m["return_type"] = ret;
        methods.append(m);
    };

    mkMethod("loadModule",
             QJsonArray{mkParam("name", "QString")},
             "QVariant");

    mkMethod("unloadModule",
             QJsonArray{mkParam("name", "QString")},
             "QVariant");

    mkMethod("reloadModule",
             QJsonArray{mkParam("name", "QString")},
             "QVariant");

    mkMethod("listModules",
             QJsonArray{mkParam("filter", "QString")},
             "QJsonArray");

    mkMethod("getStatus",
             QJsonArray{},
             "QJsonObject");

    mkMethod("getModuleInfo",
             QJsonArray{mkParam("name", "QString")},
             "QJsonObject");

    mkMethod("getModuleStats",
             QJsonArray{},
             "QJsonArray");

    mkMethod("callModuleMethod",
             QJsonArray{mkParam("module", "QString"),
                        mkParam("method", "QString"),
                        mkParam("args", "QVariantList")},
             "QVariant");

    mkMethod("watchModuleEvents",
             QJsonArray{mkParam("module", "QString"),
                        mkParam("eventName", "QString")},
             "bool");

    mkMethod("shutdown",
             QJsonArray{},
             "QJsonObject");

    return methods;
}
