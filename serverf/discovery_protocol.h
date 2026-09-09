#ifndef DISCOVERY_PROTOCOL_H
#define DISCOVERY_PROTOCOL_H

#include <memory>
#include <nlohmann/json.hpp>

class ClientSession;
class Database;

enum class DiscoveryRequestResult {
    NotHandled,
    Handled,
    Stop
};

DiscoveryRequestResult handleDiscoveryRequest(
    const nlohmann::json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database);

#endif
