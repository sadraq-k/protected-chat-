#include "client_connection.h"

#include <algorithm>
#include <array>
#include <utility>

using json = nlohmann::json;
using boost::asio::ip::tcp;

ClientConnection::ClientConnection()
    : socket(ioContext),
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
        const auto sender = message.find("sender");
        const auto content = message.find("content");
        if (sender == message.end() || !sender->is_string() ||
            content == message.end() || !content->is_string()) {
            emitDiagnostic("Ignored an invalid MESSAGE event", true);
            return true;
        }
        if (frameHandler) {
            frameHandler(ClientFrameKind::Message, message);
        }
        return true;
    }

    const auto status = message.find("status");
    const auto detail = message.find("message");
    if (status != message.end() && status->is_string() &&
        (status->get<std::string>() == "SUCCESS" ||
         status->get<std::string>() == "FAIL") &&
        detail != message.end() && detail->is_string()) {
        if (frameHandler) {
            frameHandler(ClientFrameKind::OperationResult, message);
        }
        return true;
    }

    emitDiagnostic("Ignored an unrecognized server frame", true);
    return true;
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
