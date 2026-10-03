#ifndef LOGOSCTL_REMOVED_TRANSPORTS_H
#define LOGOSCTL_REMOVED_TRANSPORTS_H

#include "json_schema.h"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string>

// The one refusal for a setting that went with the tcp and tcp_ssl transports:
// config keys, their values and logoscore's flags alike.
inline std::string removedWithTcpTransports(const std::string& key)
{
    return "`" + key + "` was removed with the tcp and tcp_ssl transports: the daemon "
           "listens on its local socket only. To operate it from another computer use "
           "Remote Runtime Control (`logosctl peer invite --runtime-control`, "
           "`logosctl remote pair`, `--remote`); to call its modules from another "
           "runtime, peer the two (developer guide §9.6).";
}

// A transport entry written before tcp and tcp_ssl went: `<protocolKey>: local`
// still loads. tcp, tcp_ssl or one of their `tcpFields` is refused, naming it.
inline bool isLocalTransportEntry(const nlohmann::json& entry, const std::string& protocolKey,
                                  std::initializer_list<const char*> tcpFields,
                                  json_schema::Errors& errs, const std::string& path)
{
    if (!entry.is_object()) {
        errs.mismatch(path, "a mapping describing one transport", entry);
        return false;
    }
    json_schema::Reader r(entry, errs, path + ".");
    const std::string protocol = r.str(protocolKey);
    if (!errs.ok()) return false;
    if (protocol == "tcp" || protocol == "tcp_ssl") {
        errs.note(removedWithTcpTransports(r.path(protocolKey) + ": " + protocol));
        return false;
    }
    if (protocol != "local") {
        errs.note(r.path(protocolKey) + R"(: expected "local")" +
                  (protocol.empty() ? std::string(", but it is missing.")
                                    : ", but got \"" + protocol + "\"."));
        return false;
    }
    for (const char* field : tcpFields) {
        const auto it = entry.find(field);
        if (it != entry.end() && !it->is_null()) {
            errs.note(removedWithTcpTransports(r.path(field)));
            return false;
        }
    }
    return true;
}

#endif // LOGOSCTL_REMOVED_TRANSPORTS_H
