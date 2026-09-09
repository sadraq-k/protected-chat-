#include "serverf/client_session.h"
#include "database.h"
#include "serverf/discovery_protocol.h"
#include "serverf/durable_routing.h"
#include "serverf/session_registry.h"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;
using boost::asio::ip::tcp;

#ifndef PROTECTED_CHAT_SERVER_PORT
#define PROTECTED_CHAT_SERVER_PORT 1403
#endif

namespace {

enum class RequestReadResult {
    Request,
    InvalidCompleteFrame,
    TooLarge,
    Closed
};

constexpr std::size_t DefaultGroupListLimit = 50;
constexpr std::size_t MaxGroupListLimit = 100;
constexpr std::size_t MaxClientResponseJsonBytes = 1'048'576;

class HandlerTracker {
public:
    bool add(const std::shared_ptr<ClientSession>& session) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return false;
        }
        sessions.insert(session);
        return true;
    }

    void complete(const std::shared_ptr<ClientSession>& session) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        sessions.erase(session);
        if (sessions.empty()) {
            completed.notify_all();
        }
    }

    void stopAllAndWait() noexcept {
        std::vector<std::shared_ptr<ClientSession>> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            snapshot.assign(sessions.begin(), sessions.end());
        }

        for (const std::shared_ptr<ClientSession>& session : snapshot) {
            session->requestStop();
        }

        std::unique_lock<std::mutex> lock(mutex);
        completed.wait(lock, [this] { return sessions.empty(); });
    }

private:
    std::mutex mutex;
    std::condition_variable completed;
    std::unordered_set<std::shared_ptr<ClientSession>> sessions;
    bool stopping = false;
};

bool isWhitespaceOnly(const std::string& text) {
    for (const unsigned char character : text) {
        if (!std::isspace(character)) {
            return false;
        }
    }
    return true;
}

bool readNonEmptyString(
    const json& request,
    const char* field,
    std::string& value) {
    const auto found = request.find(field);
    if (found == request.end() || !found->is_string()) {
        return false;
    }
    value = found->get<std::string>();
    return !value.empty();
}

bool containsOnlyFields(
    const json& request,
    std::initializer_list<const char*> allowedFields) {
    for (const auto& field : request.items()) {
        bool allowed = false;
        for (const char* allowedField : allowedFields) {
            if (field.key() == allowedField) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool readSignedInteger(
    const json& request,
    const char* field,
    std::int64_t& value) {
    const auto found = request.find(field);
    if (found == request.end()) {
        return false;
    }
    if (found->is_number_unsigned()) {
        const std::uint64_t unsignedValue = found->get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        value = static_cast<std::int64_t>(unsignedValue);
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    value = found->get<std::int64_t>();
    return true;
}

bool isAsciiWhitespaceOnly(const std::string& text) {
    for (const unsigned char character : text) {
        if (character != ' ' && character != '\t' && character != '\r' &&
            character != '\n' && character != '\f' && character != '\v') {
            return false;
        }
    }
    return true;
}

RequestReadResult readJsonRequest(
    const std::shared_ptr<ClientSession>& session,
    json& request) {
    std::string frame;
    const FrameReadResult frameResult = session->readFrame(frame);
    if (frameResult == FrameReadResult::TooLarge) {
        return RequestReadResult::TooLarge;
    }
    if (frameResult != FrameReadResult::Frame) {
        if (frameResult == FrameReadResult::IncompleteFrame) {
            std::cerr << "[CONNECTION] EOF during an incomplete frame" << std::endl;
        } else if (frameResult == FrameReadResult::TransportFailure &&
                   !session->isStopping()) {
            std::cerr << "[CONNECTION] Read failure" << std::endl;
        }
        return RequestReadResult::Closed;
    }

    if (frame.empty() || isWhitespaceOnly(frame)) {
        return RequestReadResult::InvalidCompleteFrame;
    }

    request = json::parse(frame, nullptr, false);
    if (request.is_discarded()) {
        return RequestReadResult::InvalidCompleteFrame;
    }
    return RequestReadResult::Request;
}

bool sendFailure(
    const std::shared_ptr<ClientSession>& session,
    const std::string& message) {
    return session->sendJson({{"status", "FAIL"}, {"message", message}}) ==
        SendResult::Written;
}

bool authenticate(
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry) {
    json request;
    const RequestReadResult readResult = readJsonRequest(session, request);
    if (readResult == RequestReadResult::TooLarge) {
        sendFailure(session, "Frame too large");
        session->requestStop();
        return false;
    }
    if (readResult == RequestReadResult::InvalidCompleteFrame) {
        std::cout << "[AUTH] Invalid authentication frame" << std::endl;
        sendFailure(session, "Invalid authentication request");
        return false;
    }
    if (readResult == RequestReadResult::Closed) {
        return false;
    }
    if (!request.is_object()) {
        sendFailure(session, "Invalid authentication request");
        return false;
    }

    std::string action;
    if (!readNonEmptyString(request, "action", action)) {
        sendFailure(session, "Invalid authentication request");
        return false;
    }

    std::string username;
    std::string password;
    if (action == "SIGN_IN") {
        std::string name;
        if (!readNonEmptyString(request, "name", name) ||
            !readNonEmptyString(request, "username", username) ||
            !readNonEmptyString(request, "password", password)) {
            sendFailure(session, "All fields are required");
            return false;
        }

        std::cout << "[AUTH] Registration attempted for: " << username << std::endl;
        try {
            const RegistrationResult result =
                database.insertUser(name, username, password);
            if (result == RegistrationResult::DuplicateUsername) {
                std::cout << "[AUTH] Registration rejected: duplicate username" << std::endl;
                sendFailure(session, "Username exists");
                return false;
            }
        } catch (const std::runtime_error& error) {
            std::cerr << "[AUTH] Registration database failure: "
                      << error.what() << std::endl;
            sendFailure(session, "Database error");
            return false;
        }
    } else if (action == "LOG_IN") {
        if (!readNonEmptyString(request, "username", username) ||
            !readNonEmptyString(request, "password", password)) {
            sendFailure(session, "Username and password required");
            return false;
        }

        std::cout << "[AUTH] Login attempted for: " << username << std::endl;
        try {
            if (!database.verifyLogin(username, password)) {
                std::cout << "[AUTH] Login rejected: invalid credentials" << std::endl;
                sendFailure(session, "Invalid credentials");
                return false;
            }
        } catch (const std::runtime_error& error) {
            std::cerr << "[AUTH] Login database failure: "
                      << error.what() << std::endl;
            sendFailure(session, "Database error");
            return false;
        }
    } else {
        sendFailure(session, "Invalid action");
        return false;
    }

    session->setAuthenticatedUsername(username);
    if (!registry.reserve(username, session)) {
        std::cout << "[AUTH] Admission rejected: user already logged in" << std::endl;
        sendFailure(session, "User already logged in");
        return false;
    }

    const json success = {
        {"status", "SUCCESS"},
        {"message", "Your ID: " + username}
    };
    if (session->sendJson(success) != SendResult::Written) {
        registry.removeIfSame(username, session);
        return false;
    }
    if (!registry.publishReady(username, session)) {
        registry.removeIfSame(username, session);
        return false;
    }

    std::cout << "[AUTH] Session admitted for: " << username << std::endl;
    return true;
}

bool processGroupCreate(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    const std::string& trustedUsername,
    Database& database) {
    std::string name;
    if (!containsOnlyFields(request, {"type", "name"}) ||
        !readNonEmptyString(request, "name", name) ||
        name.size() > 128 || isAsciiWhitespaceOnly(name)) {
        return sendFailure(session, "Invalid group name");
    }

    std::optional<GroupCreationResult> result;
    try {
        result.emplace(database.createGroup(trustedUsername, name));
    } catch (const std::invalid_argument&) {
        return sendFailure(session, "Invalid group name");
    } catch (const std::runtime_error& error) {
        std::cerr << "[GROUP] Creation database failure: "
                  << error.what() << std::endl;
        return sendFailure(session, "Database error");
    }

    if (result->status() == GroupCreationStatus::DuplicateName) {
        return session->sendJson({
            {"status", "FAIL"},
            {"message", "Group name exists"},
            {"operation", "GROUP_CREATE"},
            {"outcome", "DUPLICATE_NAME"}
        }) == SendResult::Written;
    }
    if (result->status() == GroupCreationStatus::CreatorNotFound) {
        return session->sendJson({
            {"status", "FAIL"},
            {"message", "Authenticated account not found"},
            {"operation", "GROUP_CREATE"},
            {"outcome", "CREATOR_NOT_FOUND"}
        }) == SendResult::Written;
    }
    if (!result->group()) {
        std::cerr << "[GROUP] Creation returned no group identity" << std::endl;
        return sendFailure(session, "Database error");
    }

    const GroupSummary& group = *result->group();
    return session->sendJson({
        {"status", "SUCCESS"},
        {"message", "Group created"},
        {"operation", "GROUP_CREATE"},
        {"outcome", "CREATED"},
        {"group", {{"id", group.id()}, {"name", group.name()}}}
    }) == SendResult::Written;
}

bool processGroupJoin(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    const std::string& trustedUsername,
    Database& database) {
    std::int64_t groupId = 0;
    if (!containsOnlyFields(request, {"type", "group_id"}) ||
        !readSignedInteger(request, "group_id", groupId) || groupId <= 0) {
        return sendFailure(session, "Invalid group ID");
    }

    std::optional<GroupJoinResult> result;
    try {
        result = database.joinGroup(trustedUsername, groupId);
    } catch (const std::invalid_argument&) {
        return sendFailure(session, "Invalid group ID");
    } catch (const std::runtime_error& error) {
        std::cerr << "[GROUP] Join database failure: "
                  << error.what() << std::endl;
        return sendFailure(session, "Database error");
    }

    if (*result == GroupJoinResult::GroupNotFound) {
        return session->sendJson({
            {"status", "FAIL"},
            {"message", "Group not found"},
            {"operation", "GROUP_JOIN"},
            {"outcome", "GROUP_NOT_FOUND"}
        }) == SendResult::Written;
    }
    if (*result == GroupJoinResult::UserNotFound) {
        return session->sendJson({
            {"status", "FAIL"},
            {"message", "Authenticated account not found"},
            {"operation", "GROUP_JOIN"},
            {"outcome", "USER_NOT_FOUND"}
        }) == SendResult::Written;
    }

    const bool alreadyMember = *result == GroupJoinResult::AlreadyMember;
    return session->sendJson({
        {"status", "SUCCESS"},
        {"message", alreadyMember ? "Already a member" : "Group joined"},
        {"operation", "GROUP_JOIN"},
        {"outcome", alreadyMember ? "ALREADY_MEMBER" : "JOINED"},
        {"group_id", groupId}
    }) == SendResult::Written;
}

bool buildGroupListResponse(
    const GroupListResult& result,
    std::int64_t afterGroupId,
    json& response,
    std::string& failureMessage) {
    response = {
        {"status", "SUCCESS"},
        {"message", "Groups listed"},
        {"operation", "GROUP_LIST"},
        {"groups", json::array()},
        {"has_more", result.hasMore()},
        {"next_after_group_id", afterGroupId}
    };

    const std::vector<GroupSummary>& groups = result.groups();
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const GroupSummary& group = groups[index];
        json candidate = response;
        candidate["groups"].push_back({
            {"id", group.id()},
            {"name", group.name()}
        });
        candidate["next_after_group_id"] = group.id();
        candidate["has_more"] =
            index + 1 < groups.size() || result.hasMore();

        if (candidate.dump().size() > MaxClientResponseJsonBytes) {
            if (response["groups"].empty()) {
                failureMessage = "Group record exceeds response limit";
                return false;
            }
            response["has_more"] = true;
            return true;
        }
        response = std::move(candidate);
    }
    return true;
}

bool processGroupList(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    const std::string& trustedUsername,
    Database& database) {
    if (!containsOnlyFields(
            request, {"type", "after_group_id", "limit"})) {
        return sendFailure(session, "Invalid group list request");
    }

    std::int64_t afterGroupId = 0;
    const auto cursor = request.find("after_group_id");
    if (cursor != request.end() &&
        (!readSignedInteger(request, "after_group_id", afterGroupId) ||
         afterGroupId < 0)) {
        return sendFailure(session, "Invalid group list cursor");
    }

    std::size_t limit = DefaultGroupListLimit;
    const auto requestedLimit = request.find("limit");
    if (requestedLimit != request.end()) {
        std::int64_t parsedLimit = 0;
        if (!readSignedInteger(request, "limit", parsedLimit) ||
            parsedLimit <= 0 ||
            parsedLimit > static_cast<std::int64_t>(MaxGroupListLimit)) {
            return sendFailure(session, "Invalid group list limit");
        }
        limit = static_cast<std::size_t>(parsedLimit);
    }

    std::optional<GroupListResult> result;
    try {
        result.emplace(
            database.listGroupsForUser(
                trustedUsername, afterGroupId, limit));
    } catch (const std::invalid_argument&) {
        return sendFailure(session, "Invalid group list request");
    } catch (const std::runtime_error& error) {
        std::cerr << "[GROUP] List database failure: "
                  << error.what() << std::endl;
        return sendFailure(session, "Database error");
    }

    if (result->status() == GroupListStatus::UserNotFound) {
        return session->sendJson({
            {"status", "FAIL"},
            {"message", "Authenticated account not found"},
            {"operation", "GROUP_LIST"},
            {"outcome", "USER_NOT_FOUND"}
        }) == SendResult::Written;
    }

    json response;
    std::string failureMessage;
    try {
        if (!buildGroupListResponse(
                *result, afterGroupId, response, failureMessage)) {
            return sendFailure(session, failureMessage);
        }
    } catch (const json::exception&) {
        return sendFailure(session, "Group data cannot be encoded");
    }
    return session->sendJson(response) == SendResult::Written;
}

void processMessages(
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry) {
    const std::string& sender = session->authenticatedUsername();

    while (!session->isStopping()) {
        json request;
        const RequestReadResult readResult = readJsonRequest(session, request);
        if (readResult == RequestReadResult::TooLarge) {
            sendFailure(session, "Frame too large");
            session->requestStop();
            return;
        }
        if (readResult == RequestReadResult::InvalidCompleteFrame) {
            if (!sendFailure(session, "Invalid request")) {
                return;
            }
            continue;
        }
        if (readResult == RequestReadResult::Closed) {
            return;
        }
        if (!request.is_object()) {
            if (!sendFailure(session, "Invalid request")) {
                return;
            }
            continue;
        }

        std::string type;
        if (!readNonEmptyString(request, "type", type)) {
            if (!sendFailure(session, "Invalid message type")) {
                return;
            }
            continue;
        }

        if (type == "EXIT") {
            return;
        }

        const DurableRequestResult durableResult =
            handleDurableRequest(request, session, database, registry);
        if (durableResult == DurableRequestResult::Stop) {
            return;
        }
        if (durableResult == DurableRequestResult::Handled) {
            continue;
        }
        const DiscoveryRequestResult discoveryResult =
            handleDiscoveryRequest(request, session, database);
        if (discoveryResult == DiscoveryRequestResult::Stop) {
            return;
        }
        if (discoveryResult == DiscoveryRequestResult::Handled) {
            continue;
        }
        if (type == "GROUP_CREATE") {
            if (!processGroupCreate(request, session, sender, database)) {
                return;
            }
            continue;
        }
        if (type == "GROUP_JOIN") {
            if (!processGroupJoin(request, session, sender, database)) {
                return;
            }
            continue;
        }
        if (type == "GROUP_LIST") {
            if (!processGroupList(request, session, sender, database)) {
                return;
            }
            continue;
        }
        if (!sendFailure(session, "Invalid message type")) {
            return;
        }
    }
}

void cleanupSession(
    const std::shared_ptr<ClientSession>& session,
    SessionRegistry& registry,
    HandlerTracker& handlers) noexcept {
    session->requestStop();
    const std::string& username = session->authenticatedUsername();
    if (!username.empty()) {
        registry.removeIfSame(username, session);
    }
    session->close();
    handlers.complete(session);
    std::cout << "[CONNECTION] Session closed"
              << (username.empty() ? "" : ": " + username) << std::endl;
}

void handleClient(
    const std::shared_ptr<ClientSession>& session,
    Database& database,
    SessionRegistry& registry,
    HandlerTracker& handlers) noexcept {
    try {
        std::cout << "[CONNECTION] Client connected" << std::endl;
        if (authenticate(session, database, registry)) {
            processMessages(session, database, registry);
        }
    } catch (...) {
        std::cerr << "[CONNECTION] Unexpected session failure" << std::endl;
    }
    cleanupSession(session, registry, handlers);
}

} // namespace

void runServer(const std::string& ip, int port) {
    boost::asio::io_context ioContext;
    Database database("chat.db");
    const std::int64_t imported = database.importLegacyMessages();
    if (imported > 0) {
        std::cout << "[DB] Imported " << imported
                  << " legacy deliveries as durable Private messages; "
                  << "original timestamps and routing context were unavailable"
                  << std::endl;
    }
    SessionRegistry registry;
    HandlerTracker handlers;

    try {
        tcp::acceptor acceptor(
            ioContext,
            tcp::endpoint(boost::asio::ip::make_address(ip), port));
        std::cout << "[SERVER] Running on " << ip << ":" << port << std::endl;

        while (true) {
            const std::shared_ptr<ClientSession> session =
                std::make_shared<ClientSession>(ioContext);
            acceptor.accept(session->socketForAccept());

            if (!handlers.add(session)) {
                session->close();
                break;
            }

            try {
                std::thread(
                    handleClient,
                    session,
                    std::ref(database),
                    std::ref(registry),
                    std::ref(handlers))
                    .detach();
            } catch (const std::system_error& error) {
                std::cerr << "[SERVER] Failed to start handler: "
                          << error.what() << std::endl;
                session->close();
                handlers.complete(session);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[SERVER] Accept loop stopped: " << error.what() << std::endl;
    }

    handlers.stopAllAndWait();
}

int main() {
    try {
        runServer("127.0.0.1", PROTECTED_CHAT_SERVER_PORT);
    } catch (const std::exception& error) {
        std::cerr << "[SERVER] Startup failure: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
