#ifndef CALL_COMMAND_H
#define CALL_COMMAND_H

#include "command.h"

class CallCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "call"; }
    QString description() const override { return "Call a method on a loaded module"; }

private:
    QString resolveFileParam(const QString& param);
};

#endif // CALL_COMMAND_H
