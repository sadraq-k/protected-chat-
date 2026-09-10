#ifndef DURABLE_ROUTING_H
#define DURABLE_ROUTING_H

#include <nlohmann/json.hpp>

#include <memory>

class ClientSession;
class Database;
class SessionRegistry;

enum class DurableRequestResult {
    NotHandled,
    Handled,
    Stop
};

DurableRequestResult handleDurableRequest(
    const nlohmann::json& request,
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry);

#endif
