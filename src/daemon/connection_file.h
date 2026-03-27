#ifndef CONNECTION_FILE_H
#define CONNECTION_FILE_H

#include <string>
#include <vector>
#include <cstdint>

struct ConnectionInfo {
    bool valid = false;
    std::string instanceId;
    std::string token;
    int64_t pid = -1;
    std::string startedAt;           // ISO 8601 UTC timestamp
    std::vector<std::string> modulesDirs;
};

class ConnectionFile {
public:
    static bool write(const std::string& instanceId, const std::string& token,
                      int64_t pid, const std::vector<std::string>& modulesDirs);
    static ConnectionInfo read();
    static bool remove();
    static bool isStale();
    static std::string filePath();

private:
    static bool isPidAlive(int64_t pid);
};

#endif // CONNECTION_FILE_H
