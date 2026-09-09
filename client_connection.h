#ifndef CLIENT_CONNECTION_H
#define CLIENT_CONNECTION_H

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

enum class AuthenticationResult {
    Success,
    Rejected,
    ConnectionStopped
};

enum class ClientFrameKind {
    Message,
    OperationResult
};

enum class ClientSendResult {
    Written,
    TooLarge,
    Failed
};

class ClientConnection {
public:
    static constexpr std::size_t MaxOutboundJsonBytes = 65'536;
    static constexpr std::size_t MaxInboundJsonBytes = 1'048'576;
    static constexpr std::size_t MaxAutomaticRequests = 256;

    using FrameHandler =
        std::function<void(ClientFrameKind, const nlohmann::json&)>;
    using DiagnosticHandler =
        std::function<void(const std::string&, bool)>;
    using StopHandler = std::function<void()>;

    ClientConnection();
    ~ClientConnection() noexcept;

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;
    ClientConnection(ClientConnection&&) = delete;
    ClientConnection& operator=(ClientConnection&&) = delete;

    void connect(const std::string& host, const std::string& port);
    void startReceiver(
        FrameHandler frameHandler,
        DiagnosticHandler diagnosticHandler,
        StopHandler stopHandler);

    ClientSendResult sendJson(const nlohmann::json& message);
    AuthenticationResult waitForAuthentication(std::string& message);
    void beginPostAuthentication();
    void setAutomaticRequestNotifier(std::function<void()> notifier);
    bool startPendingSynchronization();
    bool flushAutomaticRequests();

    bool isAuthenticated() const noexcept;
    bool isStopping() const noexcept;
    std::string stopReason() const;

    // Stop may run on either thread and interrupts synchronous socket I/O.
    void requestStop(const std::string& reason) noexcept;
    // The owning thread joins before it performs the final close.
    void joinReceiver() noexcept;
    void close() noexcept;

private:
    enum class FrameReadResult {
        Frame,
        EndOfStream,
        IncompleteFrame,
        TooLarge,
        TransportFailure
    };

    FrameReadResult readFrame(std::string& frame);
    void receiveLoop() noexcept;
    bool dispatchFrame(const nlohmann::json& message);
    bool processDeliveryMessage(const nlohmann::json& message);
    bool processPendingPage(const nlohmann::json& message);
    bool enqueueDeliveryAcknowledgement(std::int64_t messageId);
    void notifyAutomaticRequest() noexcept;
    void completeAuthentication(
        AuthenticationResult result,
        const std::string& message);
    bool waitForPostAuthentication();
    void emitDiagnostic(const std::string& message, bool error) noexcept;

    boost::asio::io_context ioContext;
    boost::asio::ip::tcp::socket socket;
    std::string receiveBuffer;
    std::thread receiverThread;
    std::mutex writeMutex;
    std::mutex automaticRequestMutex;
    std::deque<nlohmann::json> automaticRequests;
    std::function<void()> automaticRequestNotifier;
    bool pendingSynchronizationActive;
    std::int64_t pendingAfterMessageId;
    std::optional<std::int64_t> pendingThroughMessageId;
    mutable std::mutex stateMutex;
    std::condition_variable stateChanged;
    FrameHandler frameHandler;
    DiagnosticHandler diagnosticHandler;
    StopHandler stopHandler;
    std::atomic<bool> authenticated;
    std::atomic<bool> stopping;
    bool authenticationComplete;
    AuthenticationResult authenticationResult;
    std::string authenticationMessage;
    std::string recordedStopReason;
    bool postAuthenticationEnabled;
    bool endOfStreamSeen;
    bool connected;
    bool closed;
};

#endif
