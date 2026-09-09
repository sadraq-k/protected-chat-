#ifndef CLIENT_SESSION_H
#define CLIENT_SESSION_H

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

class SessionRegistry;

enum class FrameReadResult {
    Frame,
    EndOfStream,
    IncompleteFrame,
    TooLarge,
    TransportFailure
};

enum class SendResult {
    Written,
    Failed
};

class ClientSession {
public:
    static constexpr std::size_t MaxInboundJsonBytes = 65'536;

    explicit ClientSession(boost::asio::io_context& ioContext);
    ~ClientSession() noexcept;

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    ClientSession(ClientSession&&) = delete;
    ClientSession& operator=(ClientSession&&) = delete;

    // The accept loop alone accesses the socket through this method before the handler starts.
    boost::asio::ip::tcp::socket& socketForAccept() noexcept;

    // Exactly one handler thread reads a session for its entire lifetime.
    FrameReadResult readFrame(std::string& frame);
    SendResult sendJson(const nlohmann::json& message);

    // Set once by the handler before registry reservation, then read as immutable state.
    void setAuthenticatedUsername(std::string username);
    const std::string& authenticatedUsername() const noexcept;

    bool isReady() const noexcept;
    bool isStopping() const noexcept;

    // Stop interrupts blocking I/O without waiting for the session write mutex.
    void requestStop() noexcept;
    // Final close runs after the handler's read has ended and waits for active writes.
    void close() noexcept;

private:
    friend class SessionRegistry;

    void markReady() noexcept;

    boost::asio::ip::tcp::socket socket;
    std::string receiveBuffer;
    std::string username;
    std::mutex writeMutex;
    std::atomic<bool> ready;
    std::atomic<bool> stopping;
    bool endOfStreamSeen;
    bool closed;
};

#endif
