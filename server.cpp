#include "client_session.h"
#include "database.h"
#include "session_registry.h"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
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

enum class DeliveryResult {
    Delivered,
    StoredOffline,
    Failed
};

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

DeliveryResult sendMessageToUser(
    const std::string& sender,
    const std::string& receiver,
    const std::string& content,
    Database& database,
    SessionRegistry& registry) {
    const std::shared_ptr<ClientSession> target = registry.findReady(receiver);
    if (!target) {
        database.storeOfflineMessages(sender, receiver, content);
        return DeliveryResult::StoredOffline;
    }

    const json response = {
        {"type", "MESSAGE"},
        {"sender", sender},
        {"content", content}
    };
    if (target->sendJson(response) == SendResult::Written) {
        return DeliveryResult::Delivered;
    }

    target->requestStop();
    registry.removeIfSame(receiver, target);
    return DeliveryResult::Failed;
}

bool broadcastMessage(
    const std::string& sender,
    const std::string& content,
    SessionRegistry& registry) {
    const auto targets = registry.snapshotReadyExcept(sender);
    const json response = {
        {"type", "MESSAGE"},
        {"sender", sender},
        {"content", content}
    };

    bool allWritten = true;
    for (const auto& target : targets) {
        if (target.second->sendJson(response) == SendResult::Written) {
            continue;
        }
        allWritten = false;
        target.second->requestStop();
        registry.removeIfSame(target.first, target.second);
    }
    return allWritten;
}

bool replayOfflineMessages(
    const std::shared_ptr<ClientSession>& session,
    Database& database) {
    std::vector<Message> messages;
    try {
        messages = database.getOfflineMessages(session->authenticatedUsername());
    } catch (const std::runtime_error& error) {
        std::cerr << "[OFFLINE] Fetch database failure: "
                  << error.what() << std::endl;
        sendFailure(session, "Database error");
        return false;
    }

    for (const Message& message : messages) {
        const json response = {
            {"type", "MESSAGE"},
            {"sender", message.sender},
            {"content", message.message}
        };
        if (session->sendJson(response) != SendResult::Written) {
            return false;
        }
    }

    try {
        database.clearOfflineMessages(session->authenticatedUsername());
    } catch (const std::runtime_error& error) {
        std::cerr << "[OFFLINE] Clear database failure: "
                  << error.what() << std::endl;
        sendFailure(session, "Database error");
        return false;
    }
    return true;
}

bool sendOperationResult(
    const std::shared_ptr<ClientSession>& session,
    bool success,
    const std::string& successMessage,
    const std::string& failureMessage) {
    const json response = {
        {"status", success ? "SUCCESS" : "FAIL"},
        {"message", success ? successMessage : failureMessage}
    };
    return session->sendJson(response) == SendResult::Written;
}

bool processPrivateMessage(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    const std::string& sender,
    Database& database,
    SessionRegistry& registry) {
    std::string receiver;
    std::string content;
    if (!readNonEmptyString(request, "receiver", receiver) ||
        !readNonEmptyString(request, "content", content)) {
        return sendFailure(session, "Invalid private message format");
    }

    try {
        const DeliveryResult result =
            sendMessageToUser(sender, receiver, content, database, registry);
        return sendOperationResult(
            session,
            result != DeliveryResult::Failed,
            "Private message sent",
            "Message delivery failed");
    } catch (const std::runtime_error& error) {
        std::cerr << "[MESSAGE] Private database failure: "
                  << error.what() << std::endl;
        return sendFailure(session, "Database error");
    }
}

bool processGroupMessage(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    const std::string& sender,
    Database& database,
    SessionRegistry& registry) {
    std::string content;
    const auto receivers = request.find("receivers");
    if (!readNonEmptyString(request, "content", content) ||
        receivers == request.end() ||
        !receivers->is_array() ||
        receivers->empty()) {
        return sendFailure(session, "Invalid group message format");
    }

    std::vector<std::string> validatedReceivers;
    for (const json& receiver : *receivers) {
        if (!receiver.is_string()) {
            return sendFailure(session, "Invalid group message format");
        }
        std::string value = receiver.get<std::string>();
        if (value.empty()) {
            return sendFailure(session, "Invalid group message format");
        }
        validatedReceivers.push_back(std::move(value));
    }

    bool allDelivered = true;
    try {
        for (const std::string& receiver : validatedReceivers) {
            if (sendMessageToUser(
                    sender, receiver, content, database, registry) ==
                DeliveryResult::Failed) {
                allDelivered = false;
                break;
            }
        }
    } catch (const std::runtime_error& error) {
        std::cerr << "[MESSAGE] Group database failure: "
                  << error.what() << std::endl;
        return sendFailure(session, "Database error");
    }

    return sendOperationResult(
        session,
        allDelivered,
        "Group message sent",
        "Message delivery failed");
}

bool processBroadcastMessage(
    const json& request,
    const std::shared_ptr<ClientSession>& session,
    const std::string& sender,
    SessionRegistry& registry) {
    std::string content;
    if (!readNonEmptyString(request, "content", content)) {
        return sendFailure(session, "Invalid broadcast message format");
    }

    return sendOperationResult(
        session,
        broadcastMessage(sender, content, registry),
        "Broadcast message sent",
        "Message delivery failed");
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
        if (type == "PRIVATE") {
            if (!processPrivateMessage(
                    request, session, sender, database, registry)) {
                return;
            }
            continue;
        }
        if (type == "GROUP") {
            if (!processGroupMessage(
                    request, session, sender, database, registry)) {
                return;
            }
            continue;
        }
        if (type == "BROADCAST") {
            if (!processBroadcastMessage(request, session, sender, registry)) {
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
        if (authenticate(session, database, registry) &&
            replayOfflineMessages(session, database)) {
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
