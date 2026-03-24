#ifndef CALL_EXECUTOR_H
#define CALL_EXECUTOR_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonValue>
#include "command_line_parser.h"

class CallExecutor {
public:
    static int executeCalls(const QList<ModuleCall>& calls);

private:
    static QString resolveParam(const QString& param);
    
    static QJsonValue convertParam(const QString& param);
    
    static QString buildParamsJson(const QStringList& params);
    
    static bool executeCall(const ModuleCall& call);
};

#endif // CALL_EXECUTOR_H
