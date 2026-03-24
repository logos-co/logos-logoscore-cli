#ifndef CONNECTION_FILE_H
#define CONNECTION_FILE_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QDateTime>

struct ConnectionInfo {
    bool valid = false;
    QString instanceId;
    QString token;
    qint64 pid = -1;
    QDateTime startedAt;
    QStringList modulesDirs;
};

class ConnectionFile {
public:
    static bool write(const QString& instanceId, const QString& token,
                      qint64 pid, const QStringList& modulesDirs);
    static ConnectionInfo read();
    static bool remove();
    static bool isStale();
    static QString filePath();

private:
    static bool isPidAlive(qint64 pid);
};

#endif // CONNECTION_FILE_H
