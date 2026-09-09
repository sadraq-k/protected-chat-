#ifndef SESSION_REGISTRY_H
#define SESSION_REGISTRY_H

#include "client_session.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class SessionRegistry {
public:
    bool reserve(const std::string& username, const std::shared_ptr<ClientSession>& session);
    bool publishReady(const std::string& username, const std::shared_ptr<ClientSession>& session);
    std::shared_ptr<ClientSession> findReady(const std::string& username);
    std::vector<std::pair<std::string, std::shared_ptr<ClientSession>>>
    snapshotReadyExcept(const std::string& excludedUsername);
    bool removeIfSame(const std::string& username, const std::shared_ptr<ClientSession>& session);

private:
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<ClientSession>> sessions;
};

#endif
