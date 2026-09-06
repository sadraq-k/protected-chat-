#include "session_registry.h"

bool SessionRegistry::reserve(
    const std::string& username,
    const std::shared_ptr<ClientSession>& session) {
    std::lock_guard<std::mutex> lock(mutex);
    if (sessions.find(username) != sessions.end()) {
        return false;
    }
    sessions.emplace(username, session);
    return true;
}

bool SessionRegistry::publishReady(
    const std::string& username,
    const std::shared_ptr<ClientSession>& session) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = sessions.find(username);
    if (found == sessions.end() || found->second != session || session->isStopping()) {
        return false;
    }
    session->markReady();
    return true;
}

std::shared_ptr<ClientSession> SessionRegistry::findReady(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = sessions.find(username);
    if (found == sessions.end() || !found->second->isReady()) {
        return nullptr;
    }
    if (found->second->isStopping()) {
        sessions.erase(found);
        return nullptr;
    }
    return found->second;
}

std::vector<std::pair<std::string, std::shared_ptr<ClientSession>>>
SessionRegistry::snapshotReadyExcept(const std::string& excludedUsername) {
    std::vector<std::pair<std::string, std::shared_ptr<ClientSession>>> snapshot;
    std::lock_guard<std::mutex> lock(mutex);

    for (auto entry = sessions.begin(); entry != sessions.end();) {
        const std::shared_ptr<ClientSession>& session = entry->second;
        if (session->isReady() && session->isStopping()) {
            entry = sessions.erase(entry);
            continue;
        }
        if (entry->first != excludedUsername && session->isReady()) {
            snapshot.emplace_back(entry->first, session);
        }
        ++entry;
    }
    return snapshot;
}

bool SessionRegistry::removeIfSame(
    const std::string& username,
    const std::shared_ptr<ClientSession>& session) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = sessions.find(username);
    if (found == sessions.end() || found->second != session) {
        return false;
    }
    sessions.erase(found);
    return true;
}
