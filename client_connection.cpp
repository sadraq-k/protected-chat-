#include "client_connection.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using json = nlohmann::json;
using boost::asio::ip::tcp;

namespace {

bool containsOnlyFields(
    const json& value,
    std::initializer_list<const char*> allowedFields) {
    for (const auto& field : value.items()) {
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
    const json& value,
    const char* field,
    std::int64_t& result) {
    const auto found = value.find(field);
    if (found == value.end()) {
        return false;
    }
    if (found->is_number_unsigned()) {
        const std::uint64_t unsignedValue = found->get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        result = static_cast<std::int64_t>(unsignedValue);
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    result = found->get<std::int64_t>();
    return true;
}

bool validDeliveryEnvelope(const json& message) {
    const auto kind = message.find("kind");
    const bool isGroup = kind != message.end() && kind->is_string() &&
        kind->get<std::string>() == "GROUP";
    if (!containsOnlyFields(
            message,
            isGroup
                ? std::initializer_list<const char*>{
                    "type", "message_id", "kind", "sender_id", "sender",
                    "group_id", "content", "created_at"}
                : std::initializer_list<const char*>{
                    "type", "message_id", "kind", "sender_id", "sender",
                    "content", "created_at"})) {
        return false;
    }
    const auto type = message.find("type");
    const auto sender = message.find("sender");
    const auto content = message.find("content");
    if (type == message.end() || !type->is_string() ||
        type->get<std::string>() != "MESSAGE" ||
        kind == message.end() || !kind->is_string() ||
        sender == message.end() || !sender->is_string() ||
        sender->get_ref<const std::string&>().empty() ||
        content == message.end() || !content->is_string() ||
        content->get_ref<const std::string&>().empty()) {
        return false;
    }
    const std::string kindValue = kind->get<std::string>();
    if (kindValue != "PRIVATE" && kindValue != "GROUP" &&
        kindValue != "BROADCAST") {
        return false;
    }
    std::int64_t messageId = 0;
    std::int64_t senderId = 0;
    std::int64_t createdAt = 0;
    if (!readSignedInteger(message, "message_id", messageId) || messageId <= 0 ||
        !readSignedInteger(message, "sender_id", senderId) || senderId <= 0 ||
        !readSignedInteger(message, "created_at", createdAt) || createdAt < 0) {
        return false;
    }
    if (isGroup) {
        std::int64_t groupId = 0;
        return readSignedInteger(message, "group_id", groupId) && groupId > 0;
    }
    return message.find("group_id") == message.end();
}

bool validHistoryPage(const json& message) {
    if (!containsOnlyFields(
            message,
            {"status", "operation", "code", "message", "messages",
             "through_message_id", "next_after_message_id", "has_more"})) {
        return false;
    }
    const auto status = message.find("status");
    const auto operation = message.find("operation");
    const auto code = message.find("code");
    const auto detail = message.find("message");
    const auto records = message.find("messages");
    const auto hasMore = message.find("has_more");
    if (status == message.end() || !status->is_string() ||
        status->get<std::string>() != "SUCCESS" ||
        operation == message.end() || !operation->is_string() ||
        operation->get<std::string>() != "HISTORY" ||
        code == message.end() || !code->is_string() ||
        code->get<std::string>() != "HISTORY_PAGE" ||
        detail == message.end() || !detail->is_string() ||
        records == message.end() || !records->is_array() ||
        hasMore == message.end() || !hasMore->is_boolean()) {
        return false;
    }
    std::int64_t throughMessageId = 0;
    std::int64_t nextAfterMessageId = 0;
    if (!readSignedInteger(
            message, "through_message_id", throughMessageId) ||
        throughMessageId < 0 ||
        !readSignedInteger(
            message, "next_after_message_id", nextAfterMessageId) ||
        nextAfterMessageId < 0 || nextAfterMessageId > throughMessageId) {
        return false;
    }
    std::int64_t previousId = 0;
    for (const json& record : *records) {
        std::int64_t messageId = 0;
        if (!validDeliveryEnvelope(record) ||
            !readSignedInteger(record, "message_id", messageId) ||
            messageId <= previousId || messageId > throughMessageId) {
            return false;
        }
        previousId = messageId;
    }
    return records->empty() || nextAfterMessageId == previousId;
}

} // namespace

ClientConnection::ClientConnection()
    : socket(ioContext),
      pendingSynchronizationActive(false),
      pendingAfterMessageId(0),
      authenticated(false),
      stopping(false),
      authenticationComplete(false),
      authenticationResult(AuthenticationResult::ConnectionStopped),
      postAuthenticationEnabled(false),
      endOfStreamSeen(false),
      connected(false),
      closed(false) {
    receiveBuffer.reserve(MaxInboundJsonBytes + 1);
}

ClientConnection::~ClientConnection() noexcept {
    requestStop("Client connection destroyed");
    joinReceiver();
    close();
}

void ClientConnection::connect(
    const std::string& host,
    const std::string& port) {
    tcp::resolver resolver(ioContext);
    boost::asio::connect(socket, resolver.resolve(host, port));
    connected = true;
}

void ClientConnection::startReceiver(
    FrameHandler newFrameHandler,
    DiagnosticHandler newDiagnosticHandler,
    StopHandler newStopHandler) {
    frameHandler = std::move(newFrameHandler);
    diagnosticHandler = std::move(newDiagnosticHandler);
    stopHandler = std::move(newStopHandler);
    receiverThread = std::thread(&ClientConnection::receiveLoop, this);
}

ClientSendResult ClientConnection::sendJson(const json& message) {
    std::string encoded;
    try {
        encoded = message.dump();
    } catch (...) {
        requestStop("Failed to encode outgoing request");
        return ClientSendResult::Failed;
    }

    if (encoded.size() > MaxOutboundJsonBytes) {
        return ClientSendResult::TooLarge;
    }
    encoded.push_back('\n');

    std::lock_guard<std::mutex> lock(writeMutex);
    if (stopping.load(std::memory_order_acquire) || !connected || closed) {
        return ClientSendResult::Failed;
    }

    boost::system::error_code error;
    const std::size_t written =
        boost::asio::write(socket, boost::asio::buffer(encoded), error);
    if (error || written != encoded.size()) {
        requestStop("Socket write failed");
        return ClientSendResult::Failed;
    }
    return ClientSendResult::Written;
}

AuthenticationResult ClientConnection::waitForAuthentication(
    std::string& message) {
    std::unique_lock<std::mutex> lock(stateMutex);
    stateChanged.wait(lock, [this] { return authenticationComplete; });
    message = authenticationMessage;
    return authenticationResult;
}

void ClientConnection::beginPostAuthentication() {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        postAuthenticationEnabled = true;
    }
    stateChanged.notify_all();
}

void ClientConnection::setAutomaticRequestNotifier(
    std::function<void()> notifier) {
    std::lock_guard<std::mutex> lock(automaticRequestMutex);
    automaticRequestNotifier = std::move(notifier);
}

bool ClientConnection::startPendingSynchronization() {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(automaticRequestMutex);
        if (pendingSynchronizationActive) {
            return false;
        }
        if (automaticRequests.size() >= MaxAutomaticRequests) {
            queued = false;
        } else {
            pendingSynchronizationActive = true;
            pendingAfterMessageId = 0;
            pendingThroughMessageId.reset();
            automaticRequests.push_back({
                {"type", "SYNC_PENDING"},
                {"after_message_id", 0},
                {"limit", 50}
            });
            queued = true;
        }
    }
    if (!queued) {
        emitDiagnostic("Automatic request queue overflow", true);
        requestStop("Automatic request queue overflow");
        return false;
    }
    notifyAutomaticRequest();
    return true;
}

bool ClientConnection::flushAutomaticRequests() {
    while (!isStopping()) {
        json request;
        {
            std::lock_guard<std::mutex> lock(automaticRequestMutex);
            if (automaticRequests.empty()) {
                return true;
            }
            request = std::move(automaticRequests.front());
            automaticRequests.pop_front();
        }
        if (sendJson(request) != ClientSendResult::Written) {
            emitDiagnostic("Automatic request write failed", true);
            requestStop("Automatic request write failed");
            return false;
        }
    }
    return false;
}

bool ClientConnection::isAuthenticated() const noexcept {
    return authenticated.load(std::memory_order_acquire);
}

bool ClientConnection::isStopping() const noexcept {
    return stopping.load(std::memory_order_acquire);
}

std::string ClientConnection::stopReason() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return recordedStopReason;
}

void ClientConnection::requestStop(const std::string& reason) noexcept {
    bool expected = false;
    if (!stopping.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(stateMutex);
        recordedStopReason = reason;
        if (!authenticationComplete) {
            authenticationComplete = true;
            authenticationResult = AuthenticationResult::ConnectionStopped;
            authenticationMessage = reason;
        }
    } catch (...) {
    }
    stateChanged.notify_all();

    boost::system::error_code ignored;
    if (connected) {
        socket.shutdown(tcp::socket::shutdown_both, ignored);
    }

    try {
        if (stopHandler) {
            stopHandler();
        }
    } catch (...) {
    }
}

void ClientConnection::joinReceiver() noexcept {
    if (receiverThread.joinable() &&
        receiverThread.get_id() != std::this_thread::get_id()) {
        try {
            receiverThread.join();
        } catch (...) {
        }
    }
}

void ClientConnection::close() noexcept {
    std::lock_guard<std::mutex> lock(writeMutex);
    if (closed) {
        return;
    }

    boost::system::error_code ignored;
    socket.close(ignored);
    connected = false;
    closed = true;
}

ClientConnection::FrameReadResult ClientConnection::readFrame(
    std::string& frame) {
    constexpr std::size_t ReadChunkBytes = 4096;
    std::array<char, ReadChunkBytes> chunk{};
    frame.clear();

    while (true) {
        const std::size_t newline = receiveBuffer.find('\n');
        if (newline != std::string::npos) {
            if (newline > MaxInboundJsonBytes) {
                return FrameReadResult::TooLarge;
            }

            frame.assign(receiveBuffer.data(), newline);
            receiveBuffer.erase(0, newline + 1);
            if (!frame.empty() && frame.back() == '\r') {
                frame.pop_back();
            }
            return FrameReadResult::Frame;
        }

        if (receiveBuffer.size() > MaxInboundJsonBytes) {
            return FrameReadResult::TooLarge;
        }
        if (endOfStreamSeen) {
            return receiveBuffer.empty()
                ? FrameReadResult::EndOfStream
                : FrameReadResult::IncompleteFrame;
        }
        if (stopping.load(std::memory_order_acquire)) {
            return FrameReadResult::TransportFailure;
        }

        const std::size_t remainingUntilOversized =
            MaxInboundJsonBytes + 1 - receiveBuffer.size();
        const std::size_t requestedBytes =
            std::min(chunk.size(), remainingUntilOversized);

        boost::system::error_code error;
        const std::size_t received =
            socket.read_some(boost::asio::buffer(chunk.data(), requestedBytes), error);
        if (received > 0) {
            receiveBuffer.append(chunk.data(), received);
        }

        if (error == boost::asio::error::eof) {
            endOfStreamSeen = true;
        } else if (error) {
            return FrameReadResult::TransportFailure;
        }
    }
}

void ClientConnection::receiveLoop() noexcept {
    try {
        while (!isStopping()) {
            std::string frame;
            const FrameReadResult result = readFrame(frame);
            if (result == FrameReadResult::Frame) {
                if (frame.empty()) {
                    if (!isAuthenticated()) {
                        completeAuthentication(
                            AuthenticationResult::ConnectionStopped,
                            "Invalid authentication response");
                        emitDiagnostic("Invalid authentication response", true);
                        requestStop("Invalid authentication response");
                        return;
                    }
                    emitDiagnostic("Ignored an empty server frame", true);
                    continue;
                }

                json message = json::parse(frame, nullptr, false);
                if (message.is_discarded()) {
                    if (!isAuthenticated()) {
                        completeAuthentication(
                            AuthenticationResult::ConnectionStopped,
                            "Invalid authentication response");
                        emitDiagnostic("Invalid authentication response", true);
                        requestStop("Invalid authentication response");
                        return;
                    }
                    emitDiagnostic("Ignored malformed server JSON", true);
                    continue;
                }

                if (!dispatchFrame(message)) {
                    return;
                }
                if (isAuthenticated() && !waitForPostAuthentication()) {
                    return;
                }
                continue;
            }

            if (result == FrameReadResult::TooLarge) {
                emitDiagnostic("Server frame exceeded the client receive limit", true);
                requestStop("Server frame too large");
            } else if (result == FrameReadResult::IncompleteFrame) {
                emitDiagnostic("Server closed during an incomplete frame", true);
                requestStop("Incomplete server frame");
            } else if (result == FrameReadResult::EndOfStream) {
                if (!isStopping()) {
                    emitDiagnostic("Server closed the connection", false);
                    requestStop("Server closed the connection");
                }
            } else if (!isStopping()) {
                emitDiagnostic("Socket read failed", true);
                requestStop("Socket read failed");
            }
            return;
        }
    } catch (...) {
        emitDiagnostic("Unexpected receiver failure", true);
        requestStop("Unexpected receiver failure");
    }
}

bool ClientConnection::dispatchFrame(const json& message) {
    if (!message.is_object()) {
        if (!isAuthenticated()) {
            completeAuthentication(
                AuthenticationResult::ConnectionStopped,
                "Invalid authentication response");
            emitDiagnostic("Invalid authentication response", true);
            requestStop("Invalid authentication response");
            return false;
        }
        emitDiagnostic("Ignored a server frame with an invalid shape", true);
        return true;
    }

    if (!isAuthenticated()) {
        const auto status = message.find("status");
        const auto detail = message.find("message");
        if (status == message.end() || !status->is_string() ||
            detail == message.end() || !detail->is_string()) {
            completeAuthentication(
                AuthenticationResult::ConnectionStopped,
                "Invalid authentication response");
            emitDiagnostic("Invalid authentication response", true);
            requestStop("Invalid authentication response");
            return false;
        }

        const std::string statusValue = status->get<std::string>();
        const std::string detailValue = detail->get<std::string>();
        if (statusValue == "SUCCESS") {
            authenticated.store(true, std::memory_order_release);
            completeAuthentication(AuthenticationResult::Success, detailValue);
            return true;
        }
        if (statusValue == "FAIL") {
            completeAuthentication(AuthenticationResult::Rejected, detailValue);
            requestStop("Authentication rejected");
            return false;
        }

        completeAuthentication(
            AuthenticationResult::ConnectionStopped,
            "Invalid authentication response");
        emitDiagnostic("Invalid authentication response", true);
        requestStop("Invalid authentication response");
        return false;
    }

    const auto type = message.find("type");
    if (type != message.end() && type->is_string() &&
        type->get<std::string>() == "MESSAGE") {
        if (!processDeliveryMessage(message)) {
            emitDiagnostic("Ignored an invalid MESSAGE event", true);
        }
        return true;
    }

    const auto status = message.find("status");
    const auto detail = message.find("message");
    if (status != message.end() && status->is_string() &&
        (status->get<std::string>() == "SUCCESS" ||
         status->get<std::string>() == "FAIL") &&
        detail != message.end() && detail->is_string()) {
        const auto operation = message.find("operation");
        if (operation != message.end() && operation->is_string() &&
            operation->get<std::string>() == "DELIVERY_ACK") {
            std::int64_t messageId = 0;
            const auto code = message.find("code");
            if (status->get<std::string>() != "SUCCESS" ||
                code == message.end() || !code->is_string() ||
                (code->get<std::string>() != "ACKNOWLEDGED" &&
                 code->get<std::string>() != "ALREADY_ACKNOWLEDGED") ||
                !readSignedInteger(message, "message_id", messageId) ||
                messageId <= 0) {
                emitDiagnostic("Automatic delivery acknowledgement failed", true);
                requestStop("Automatic delivery acknowledgement failed");
                return false;
            }
            return true;
        }
        if (operation != message.end() && operation->is_string() &&
            operation->get<std::string>() == "SYNC_PENDING") {
            if (status->get<std::string>() != "SUCCESS" ||
                !processPendingPage(message)) {
                emitDiagnostic("Pending synchronization failed", true);
                requestStop("Pending synchronization failed");
                return false;
            }
            return true;
        }
        if (operation != message.end() && operation->is_string() &&
            operation->get<std::string>() == "HISTORY" &&
            status->get<std::string>() == "SUCCESS" &&
            !validHistoryPage(message)) {
            emitDiagnostic("Invalid HISTORY response", true);
            requestStop("Invalid HISTORY response");
            return false;
        }
        if (frameHandler) {
            frameHandler(ClientFrameKind::OperationResult, message);
        }
        return true;
    }

    emitDiagnostic("Ignored an unrecognized server frame", true);
    return true;
}

bool ClientConnection::processDeliveryMessage(const json& message) {
    if (!validDeliveryEnvelope(message)) {
        return false;
    }
    if (frameHandler) {
        frameHandler(ClientFrameKind::Message, message);
    }
    std::int64_t messageId = 0;
    readSignedInteger(message, "message_id", messageId);
    return enqueueDeliveryAcknowledgement(messageId);
}

bool ClientConnection::enqueueDeliveryAcknowledgement(
    std::int64_t messageId) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(automaticRequestMutex);
        if (automaticRequests.size() < MaxAutomaticRequests) {
            automaticRequests.push_back({
                {"type", "DELIVERY_ACK"},
                {"message_id", messageId}
            });
            queued = true;
        }
    }
    if (!queued) {
        emitDiagnostic("Automatic request queue overflow", true);
        requestStop("Automatic request queue overflow");
        return false;
    }
    notifyAutomaticRequest();
    return true;
}

bool ClientConnection::processPendingPage(const json& message) {
    if (!containsOnlyFields(
            message,
            {"status", "operation", "code", "message", "messages",
             "through_message_id", "next_after_message_id", "has_more"})) {
        return false;
    }
    const auto code = message.find("code");
    const auto records = message.find("messages");
    const auto hasMoreField = message.find("has_more");
    if (code == message.end() || !code->is_string() ||
        code->get<std::string>() != "PENDING_PAGE" ||
        records == message.end() || !records->is_array() ||
        hasMoreField == message.end() || !hasMoreField->is_boolean()) {
        return false;
    }
    std::int64_t throughMessageId = 0;
    std::int64_t nextAfterMessageId = 0;
    if (!readSignedInteger(
            message, "through_message_id", throughMessageId) ||
        throughMessageId < 0 ||
        !readSignedInteger(
            message, "next_after_message_id", nextAfterMessageId) ||
        nextAfterMessageId < 0) {
        return false;
    }

    std::int64_t expectedAfter = 0;
    std::optional<std::int64_t> expectedThrough;
    {
        std::lock_guard<std::mutex> lock(automaticRequestMutex);
        if (!pendingSynchronizationActive) {
            return false;
        }
        expectedAfter = pendingAfterMessageId;
        expectedThrough = pendingThroughMessageId;
    }
    if ((expectedThrough && *expectedThrough != throughMessageId) ||
        expectedAfter > throughMessageId) {
        return false;
    }

    std::int64_t previousId = expectedAfter;
    std::vector<std::int64_t> messageIds;
    messageIds.reserve(records->size());
    for (const json& record : *records) {
        if (!validDeliveryEnvelope(record)) {
            return false;
        }
        std::int64_t messageId = 0;
        readSignedInteger(record, "message_id", messageId);
        if (messageId <= previousId || messageId > throughMessageId) {
            return false;
        }
        previousId = messageId;
        messageIds.push_back(messageId);
    }
    const bool hasMore = hasMoreField->get<bool>();
    const std::int64_t expectedNext =
        messageIds.empty() ? expectedAfter : messageIds.back();
    if (nextAfterMessageId != expectedNext ||
        (hasMore && messageIds.empty())) {
        return false;
    }

    for (const json& record : *records) {
        if (frameHandler) {
            frameHandler(ClientFrameKind::Message, record);
        }
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(automaticRequestMutex);
        const std::size_t required =
            messageIds.size() + (hasMore ? 1u : 0u);
        if (automaticRequests.size() + required <= MaxAutomaticRequests) {
            for (const std::int64_t messageId : messageIds) {
                automaticRequests.push_back({
                    {"type", "DELIVERY_ACK"},
                    {"message_id", messageId}
                });
            }
            if (hasMore) {
                automaticRequests.push_back({
                    {"type", "SYNC_PENDING"},
                    {"after_message_id", nextAfterMessageId},
                    {"through_message_id", throughMessageId},
                    {"limit", 50}
                });
                pendingAfterMessageId = nextAfterMessageId;
                pendingThroughMessageId = throughMessageId;
            } else {
                pendingSynchronizationActive = false;
                pendingAfterMessageId = 0;
                pendingThroughMessageId.reset();
            }
            queued = true;
        }
    }
    if (!queued) {
        emitDiagnostic("Automatic request queue overflow", true);
        requestStop("Automatic request queue overflow");
        return false;
    }
    if (!messageIds.empty() || hasMore) {
        notifyAutomaticRequest();
    }
    return true;
}

void ClientConnection::notifyAutomaticRequest() noexcept {
    std::function<void()> notifier;
    try {
        std::lock_guard<std::mutex> lock(automaticRequestMutex);
        notifier = automaticRequestNotifier;
    } catch (...) {
        return;
    }
    try {
        if (notifier) {
            notifier();
        }
    } catch (...) {
    }
}

void ClientConnection::completeAuthentication(
    AuthenticationResult result,
    const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (authenticationComplete) {
            return;
        }
        authenticationComplete = true;
        authenticationResult = result;
        authenticationMessage = message;
    }
    stateChanged.notify_all();
}

bool ClientConnection::waitForPostAuthentication() {
    std::unique_lock<std::mutex> lock(stateMutex);
    stateChanged.wait(lock, [this] {
        return postAuthenticationEnabled ||
            stopping.load(std::memory_order_acquire);
    });
    return !stopping.load(std::memory_order_acquire);
}

void ClientConnection::emitDiagnostic(
    const std::string& message,
    bool error) noexcept {
    try {
        if (diagnosticHandler) {
            diagnosticHandler(message, error);
        }
    } catch (...) {
    }
}
