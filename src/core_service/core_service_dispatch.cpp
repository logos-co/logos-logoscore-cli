// Qt <-> Universal bridge for CoreServiceImpl.
// Implements the LogosProviderObject interface and delegates to the
// universal-typed business methods on CoreServiceImpl.

#include "core_service_impl.h"
#include <logos_api.h>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

// ---------------------------------------------------------------------------
// Helpers: convert nlohmann::json values to QVariant / QJsonValue
// ---------------------------------------------------------------------------

static QJsonValue jsonToQJsonValue(const nlohmann::json& j)
{
    if (j.is_null())
        return QJsonValue(QJsonValue::Null);
    if (j.is_boolean())
        return QJsonValue(j.get<bool>());
    if (j.is_number_integer())
        return QJsonValue(static_cast<qint64>(j.get<int64_t>()));
    if (j.is_number_float())
        return QJsonValue(j.get<double>());
    if (j.is_string())
        return QJsonValue(QString::fromStdString(j.get<std::string>()));
    if (j.is_object() || j.is_array()) {
        QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(j.dump()));
        if (j.is_object())
            return QJsonValue(doc.object());
        return QJsonValue(doc.array());
    }
    return QJsonValue();
}

static QJsonObject logosMapToQJsonObject(const LogosMap& m)
{
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(m.dump()));
    return doc.object();
}

static QJsonArray logosListToQJsonArray(const LogosList& l)
{
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(l.dump()));
    return doc.array();
}

static QVariant stdLogosResultToQVariant(const StdLogosResult& r)
{
    return QVariant::fromValue(logosMapToQJsonObject(r.value));
}

// Convert incoming QVariantList args to a LogosList (nlohmann::json array)
static LogosList qArgsToLogosList(const QVariantList& args)
{
    LogosList result = LogosList::array();
    for (const QVariant& arg : args) {
        QJsonValue jv = QJsonValue::fromVariant(arg);
        if (jv.isString())
            result.push_back(jv.toString().toStdString());
        else if (jv.isBool())
            result.push_back(jv.toBool());
        else if (jv.isDouble())
            result.push_back(jv.toDouble());
        else if (jv.isObject() || jv.isArray()) {
            QJsonDocument doc;
            if (jv.isObject()) doc = QJsonDocument(jv.toObject());
            else doc = QJsonDocument(jv.toArray());
            result.push_back(nlohmann::json::parse(
                doc.toJson(QJsonDocument::Compact).toStdString()));
        } else {
            result.push_back(nullptr);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// LogosProviderObject overrides
// ---------------------------------------------------------------------------

QString CoreServiceImpl::providerName() const
{
    return QString::fromStdString(name());
}

QString CoreServiceImpl::providerVersion() const
{
    return QString::fromStdString(version());
}

void CoreServiceImpl::setEventListener(EventCallback callback)
{
    m_eventCallback = callback;

    // Wire the universal emitEvent to the Qt EventCallback.
    // The data string is a JSON array; unpack it into a QVariantList
    // so the client receives individual elements (the transport/client
    // expects [sourceModule, eventName, ...eventData]).
    emitEvent = [this](const std::string& eventName, const std::string& data) {
        if (m_eventCallback) {
            QVariantList qData;
            QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(data));
            if (doc.isArray()) {
                for (const QJsonValue& v : doc.array())
                    qData.append(v.toVariant());
            } else {
                qData.append(QString::fromStdString(data));
            }
            m_eventCallback(QString::fromStdString(eventName), qData);
        }
    };
}

bool CoreServiceImpl::informModuleToken(const QString& /*moduleName*/, const QString& /*token*/)
{
    return false;
}

void CoreServiceImpl::init(void* apiInstance)
{
    onInit(static_cast<LogosAPI*>(apiInstance));
}

// ---------------------------------------------------------------------------
// Method dispatch — converts Qt args -> universal, calls impl, converts back
// ---------------------------------------------------------------------------

QVariant CoreServiceImpl::callMethod(const QString& methodName, const QVariantList& args)
{
    if (methodName == "loadModule" && args.size() >= 1)
        return stdLogosResultToQVariant(loadModule(args.at(0).toString().toStdString()));

    if (methodName == "unloadModule" && args.size() >= 1)
        return stdLogosResultToQVariant(unloadModule(args.at(0).toString().toStdString()));

    if (methodName == "reloadModule" && args.size() >= 1)
        return stdLogosResultToQVariant(reloadModule(args.at(0).toString().toStdString()));

    if (methodName == "listModules") {
        std::string filter = args.size() >= 1 ? args.at(0).toString().toStdString() : "all";
        return QVariant::fromValue(logosListToQJsonArray(listModules(filter)));
    }

    if (methodName == "getStatus")
        return QVariant::fromValue(logosMapToQJsonObject(getStatus()));

    if (methodName == "getModuleInfo" && args.size() >= 1)
        return QVariant::fromValue(logosMapToQJsonObject(getModuleInfo(args.at(0).toString().toStdString())));

    if (methodName == "getModuleStats")
        return QVariant::fromValue(logosListToQJsonArray(getModuleStats()));

    if (methodName == "callModuleMethod" && args.size() >= 3)
        return stdLogosResultToQVariant(
            callModuleMethod(args.at(0).toString().toStdString(),
                             args.at(1).toString().toStdString(),
                             qArgsToLogosList(args.at(2).toList())));

    if (methodName == "watchModuleEvents" && args.size() >= 2)
        return QVariant::fromValue(watchModuleEvents(args.at(0).toString().toStdString(),
                                                     args.at(1).toString().toStdString()));

    if (methodName == "shutdown")
        return QVariant::fromValue(logosMapToQJsonObject(shutdown()));

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
             QJsonArray{mkParam("name", "string")},
             "StdLogosResult");

    mkMethod("unloadModule",
             QJsonArray{mkParam("name", "string")},
             "StdLogosResult");

    mkMethod("reloadModule",
             QJsonArray{mkParam("name", "string")},
             "StdLogosResult");

    mkMethod("listModules",
             QJsonArray{mkParam("filter", "string")},
             "LogosList");

    mkMethod("getStatus",
             QJsonArray{},
             "LogosMap");

    mkMethod("getModuleInfo",
             QJsonArray{mkParam("name", "string")},
             "LogosMap");

    mkMethod("getModuleStats",
             QJsonArray{},
             "LogosList");

    mkMethod("callModuleMethod",
             QJsonArray{mkParam("module", "string"),
                        mkParam("method", "string"),
                        mkParam("args", "LogosList")},
             "StdLogosResult");

    mkMethod("watchModuleEvents",
             QJsonArray{mkParam("module", "string"),
                        mkParam("eventName", "string")},
             "bool");

    mkMethod("shutdown",
             QJsonArray{},
             "LogosMap");

    return methods;
}
