#include "client_session.h"

#include <algorithm>
#include <array>
#include <utility>

ClientSession::ClientSession(boost::asio::io_context& ioContext)
    : socket(ioContext),
      ready(false),
      stopping(false),
      endOfStreamSeen(false),
      closed(false) {
    receiveBuffer.reserve(MaxInboundJsonBytes + 1);
}

ClientSession::~ClientSession() noexcept {
    close();
}

boost::asio::ip::tcp::socket& ClientSession::socketForAccept() noexcept {
    return socket;
}

FrameReadResult ClientSession::readFrame(std::string& frame) {
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

SendResult ClientSession::sendJson(const nlohmann::json& message) {
    std::lock_guard<std::mutex> lock(writeMutex);
    if (stopping.load(std::memory_order_acquire) || closed) {
        return SendResult::Failed;
    }

    std::string encoded;
    try {
        encoded = message.dump();
        encoded.push_back('\n');
    } catch (...) {
        requestStop();
        return SendResult::Failed;
    }

    boost::system::error_code error;
    const std::size_t written =
        boost::asio::write(socket, boost::asio::buffer(encoded), error);
    if (error || written != encoded.size()) {
        requestStop();
        return SendResult::Failed;
    }
    return SendResult::Written;
}

void ClientSession::setAuthenticatedUsername(std::string authenticatedUsername) {
    username = std::move(authenticatedUsername);
}

const std::string& ClientSession::authenticatedUsername() const noexcept {
    return username;
}

bool ClientSession::isReady() const noexcept {
    return ready.load(std::memory_order_acquire);
}

bool ClientSession::isStopping() const noexcept {
    return stopping.load(std::memory_order_acquire);
}

void ClientSession::requestStop() noexcept {
    bool expected = false;
    if (!stopping.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    boost::system::error_code ignored;
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
}

void ClientSession::close() noexcept {
    requestStop();
    std::lock_guard<std::mutex> lock(writeMutex);
    if (closed) {
        return;
    }

    boost::system::error_code ignored;
    socket.close(ignored);
    closed = true;
}

void ClientSession::markReady() noexcept {
    ready.store(true, std::memory_order_release);
}
