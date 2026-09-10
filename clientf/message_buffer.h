#ifndef PROTECTED_CHAT_MESSAGE_BUFFER_H
#define PROTECTED_CHAT_MESSAGE_BUFFER_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

enum class ConversationKind { Private, Group, Broadcast };
enum class BufferInsertResult { Stored, Duplicate, CapacityExceeded, Conflict };

class ConversationKey {
public:
    static ConversationKey privateChat(std::string username);
    static ConversationKey groupChat(std::int64_t id);
    static ConversationKey broadcast();

    ConversationKind kind() const noexcept;
    const std::string& username() const noexcept;
    std::int64_t groupId() const noexcept;

    bool operator==(const ConversationKey& other) const noexcept;
    bool operator<(const ConversationKey& other) const noexcept;

private:
    ConversationKey(
        ConversationKind kind, std::string username, std::int64_t groupId);

    ConversationKind conversationKind;
    std::string privateUsername;
    std::int64_t durableGroupId;
};

class BufferedMessage {
public:
    BufferedMessage(
        std::int64_t messageId,
        ConversationKey key,
        std::string sender,
        std::optional<std::int64_t> senderId,
        std::string content,
        std::optional<std::int64_t> createdAt,
        bool outgoing,
        std::uint64_t arrivalSequence);

    std::int64_t messageId() const noexcept;
    const ConversationKey& key() const noexcept;
    const std::string& sender() const noexcept;
    const std::optional<std::int64_t>& senderId() const noexcept;
    const std::string& content() const noexcept;
    const std::optional<std::int64_t>& createdAt() const noexcept;
    bool outgoing() const noexcept;
    std::uint64_t arrivalSequence() const noexcept;

private:
    friend class MessageBuffer;
    void acceptOutgoing(
        std::int64_t messageId, std::uint64_t arrivalSequence) noexcept;

    std::int64_t durableMessageId;
    ConversationKey conversationKey;
    std::string senderName;
    std::optional<std::int64_t> durableSenderId;
    std::string exactContent;
    std::optional<std::int64_t> serverCreatedAt;
    bool sentByCurrentUser;
    std::uint64_t localArrivalSequence;
};

class ConversationSummary {
public:
    ConversationSummary(
        ConversationKey key,
        std::size_t handle,
        std::size_t messageCount,
        std::size_t unreadCount,
        std::uint64_t latestUnreadSequence);

    const ConversationKey& key() const noexcept;
    std::size_t handle() const noexcept;
    std::size_t messageCount() const noexcept;
    std::size_t unreadCount() const noexcept;
    std::uint64_t latestUnreadSequence() const noexcept;

private:
    ConversationKey conversationKey;
    std::size_t stableHandle;
    std::size_t totalMessages;
    std::size_t unreadMessages;
    std::uint64_t latestUnreadArrival;
};

class BufferSnapshot {
public:
    BufferSnapshot(
        std::uint64_t version,
        std::vector<ConversationSummary> summaries,
        std::vector<std::shared_ptr<const BufferedMessage>> messages,
        std::uint64_t maximumArrivalSequence);

    std::uint64_t version() const noexcept;
    const std::vector<ConversationSummary>& summaries() const noexcept;
    const std::vector<std::shared_ptr<const BufferedMessage>>& messages() const
        noexcept;
    std::uint64_t maximumArrivalSequence() const noexcept;

private:
    std::uint64_t stateVersion;
    std::vector<ConversationSummary> conversationSummaries;
    std::vector<std::shared_ptr<const BufferedMessage>> activeMessages;
    std::uint64_t activeMaximumArrival;
};

class MessageBuffer {
public:
    static constexpr std::size_t MaxConversations = 256;
    static constexpr std::size_t MaxMessages = 4096;
    static constexpr std::size_t MaxTextBytes = 16 * 1024 * 1024;

    MessageBuffer() = default;
    MessageBuffer(const MessageBuffer&) = delete;
    MessageBuffer& operator=(const MessageBuffer&) = delete;
    MessageBuffer(MessageBuffer&&) = delete;
    MessageBuffer& operator=(MessageBuffer&&) = delete;

    BufferInsertResult ensureConversation(const ConversationKey& key);
    BufferInsertResult receiveValidated(const nlohmann::json& message);
    BufferInsertResult reserveOutgoing(
        const ConversationKey& key,
        const std::string& sender,
        const std::string& content);
    BufferInsertResult commitOutgoing(std::int64_t messageId);
    void cancelOutgoing() noexcept;
    BufferSnapshot snapshot(
        const std::optional<ConversationKey>& active) const;
    bool markViewed(
        const ConversationKey& key,
        std::uint64_t throughArrivalSequence);
    std::uint64_t version() const;

private:
    class ConversationData {
    public:
        explicit ConversationData(std::size_t handle);

        std::size_t handle;
        std::vector<std::shared_ptr<const BufferedMessage>> messages;
        std::set<std::uint64_t> unreadSequences;
    };

    std::size_t keyTextBytes(const ConversationKey& key) const noexcept;
    std::size_t messageTextBytes(
        const ConversationKey& key,
        const std::string& sender,
        const std::string& content) const noexcept;
    BufferInsertResult ensureConversationLocked(const ConversationKey& key);
    bool sameIncoming(
        const BufferedMessage& stored,
        const ConversationKey& key,
        const std::string& sender,
        std::int64_t senderId,
        const std::string& content,
        std::int64_t createdAt) const noexcept;

    mutable std::mutex mutex;
    std::map<ConversationKey, ConversationData> conversations;
    std::map<std::int64_t, std::shared_ptr<const BufferedMessage>> messagesById;
    std::shared_ptr<BufferedMessage> reservedMessage;
    std::map<std::int64_t,
             std::shared_ptr<const BufferedMessage>>::node_type reservedIndexNode;
    bool reservedConversationCreated = false;
    std::size_t retainedTextBytes = 0;
    std::size_t reservedTextBytes = 0;
    std::size_t nextHandle = 1;
    std::uint64_t nextArrivalSequence = 1;
    std::uint64_t stateVersion = 0;
};

#endif
