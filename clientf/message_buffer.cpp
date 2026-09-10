#include "message_buffer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

using json = nlohmann::json;

namespace {

bool readPositiveInteger(
    const json& value,
    const char* field,
    std::int64_t& result) {
    const auto found = value.find(field);
    if (found == value.end()) {
        return false;
    }
    if (found->is_number_unsigned()) {
        const std::uint64_t parsed = found->get<std::uint64_t>();
        if (parsed == 0 || parsed > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        result = static_cast<std::int64_t>(parsed);
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    result = found->get<std::int64_t>();
    return result > 0;
}

bool readNonnegativeInteger(
    const json& value,
    const char* field,
    std::int64_t& result) {
    const auto found = value.find(field);
    if (found == value.end()) {
        return false;
    }
    if (found->is_number_unsigned()) {
        const std::uint64_t parsed = found->get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        result = static_cast<std::int64_t>(parsed);
        return true;
    }
    if (!found->is_number_integer()) {
        return false;
    }
    result = found->get<std::int64_t>();
    return result >= 0;
}

bool readString(const json& value, const char* field, std::string& result) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_string()) {
        return false;
    }
    result = found->get<std::string>();
    return true;
}

} // namespace

ConversationKey::ConversationKey(
    ConversationKind kind,
    std::string username,
    std::int64_t groupId)
    : conversationKind(kind),
      privateUsername(std::move(username)),
      durableGroupId(groupId) {}

ConversationKey ConversationKey::privateChat(std::string username) {
    return ConversationKey(
        ConversationKind::Private, std::move(username), 0);
}

ConversationKey ConversationKey::groupChat(std::int64_t id) {
    return ConversationKey(ConversationKind::Group, std::string(), id);
}

ConversationKey ConversationKey::broadcast() {
    return ConversationKey(ConversationKind::Broadcast, std::string(), 0);
}

ConversationKind ConversationKey::kind() const noexcept {
    return conversationKind;
}

const std::string& ConversationKey::username() const noexcept {
    return privateUsername;
}

std::int64_t ConversationKey::groupId() const noexcept {
    return durableGroupId;
}

bool ConversationKey::operator==(const ConversationKey& other) const noexcept {
    return std::tie(conversationKind, privateUsername, durableGroupId) ==
        std::tie(
            other.conversationKind,
            other.privateUsername,
            other.durableGroupId);
}

bool ConversationKey::operator<(const ConversationKey& other) const noexcept {
    return std::tie(conversationKind, privateUsername, durableGroupId) <
        std::tie(
            other.conversationKind,
            other.privateUsername,
            other.durableGroupId);
}

BufferedMessage::BufferedMessage(
    std::int64_t messageId,
    ConversationKey key,
    std::string sender,
    std::optional<std::int64_t> senderId,
    std::string content,
    std::optional<std::int64_t> createdAt,
    bool outgoing,
    std::uint64_t arrivalSequence)
    : durableMessageId(messageId),
      conversationKey(std::move(key)),
      senderName(std::move(sender)),
      durableSenderId(senderId),
      exactContent(std::move(content)),
      serverCreatedAt(createdAt),
      sentByCurrentUser(outgoing),
      localArrivalSequence(arrivalSequence) {}

std::int64_t BufferedMessage::messageId() const noexcept {
    return durableMessageId;
}

const ConversationKey& BufferedMessage::key() const noexcept {
    return conversationKey;
}

const std::string& BufferedMessage::sender() const noexcept {
    return senderName;
}

const std::optional<std::int64_t>& BufferedMessage::senderId() const noexcept {
    return durableSenderId;
}

const std::string& BufferedMessage::content() const noexcept {
    return exactContent;
}

const std::optional<std::int64_t>& BufferedMessage::createdAt() const noexcept {
    return serverCreatedAt;
}

bool BufferedMessage::outgoing() const noexcept {
    return sentByCurrentUser;
}

std::uint64_t BufferedMessage::arrivalSequence() const noexcept {
    return localArrivalSequence;
}

void BufferedMessage::acceptOutgoing(
    std::int64_t messageId, std::uint64_t arrivalSequence) noexcept {
    durableMessageId = messageId;
    localArrivalSequence = arrivalSequence;
}

ConversationSummary::ConversationSummary(
    ConversationKey key,
    std::size_t handle,
    std::size_t messageCount,
    std::size_t unreadCount,
    std::uint64_t latestUnreadSequence)
    : conversationKey(std::move(key)),
      stableHandle(handle),
      totalMessages(messageCount),
      unreadMessages(unreadCount),
      latestUnreadArrival(latestUnreadSequence) {}

const ConversationKey& ConversationSummary::key() const noexcept {
    return conversationKey;
}

std::size_t ConversationSummary::handle() const noexcept {
    return stableHandle;
}

std::size_t ConversationSummary::messageCount() const noexcept {
    return totalMessages;
}

std::size_t ConversationSummary::unreadCount() const noexcept {
    return unreadMessages;
}

std::uint64_t ConversationSummary::latestUnreadSequence() const noexcept {
    return latestUnreadArrival;
}

BufferSnapshot::BufferSnapshot(
    std::uint64_t version,
    std::vector<ConversationSummary> summaries,
    std::vector<std::shared_ptr<const BufferedMessage>> messages,
    std::uint64_t maximumArrivalSequence)
    : stateVersion(version),
      conversationSummaries(std::move(summaries)),
      activeMessages(std::move(messages)),
      activeMaximumArrival(maximumArrivalSequence) {}

std::uint64_t BufferSnapshot::version() const noexcept {
    return stateVersion;
}

const std::vector<ConversationSummary>& BufferSnapshot::summaries() const
    noexcept {
    return conversationSummaries;
}

const std::vector<std::shared_ptr<const BufferedMessage>>&
BufferSnapshot::messages() const noexcept {
    return activeMessages;
}

std::uint64_t BufferSnapshot::maximumArrivalSequence() const noexcept {
    return activeMaximumArrival;
}

MessageBuffer::ConversationData::ConversationData(std::size_t newHandle)
    : handle(newHandle) {}

std::size_t MessageBuffer::keyTextBytes(
    const ConversationKey& key) const noexcept {
    return key.kind() == ConversationKind::Private ? key.username().size() : 0;
}

std::size_t MessageBuffer::messageTextBytes(
    const ConversationKey& key,
    const std::string& sender,
    const std::string& content) const noexcept {
    return keyTextBytes(key) + sender.size() + content.size();
}

BufferInsertResult MessageBuffer::ensureConversationLocked(
    const ConversationKey& key) {
    if (conversations.find(key) != conversations.end()) {
        return BufferInsertResult::Duplicate;
    }
    const std::size_t bytes = keyTextBytes(key);
    if (conversations.size() >= MaxConversations ||
        bytes > MaxTextBytes - retainedTextBytes) {
        return BufferInsertResult::CapacityExceeded;
    }
    if (nextHandle == 0 || nextHandle > MaxConversations ||
        stateVersion == std::numeric_limits<std::uint64_t>::max()) {
        return BufferInsertResult::CapacityExceeded;
    }
    conversations.emplace(key, ConversationData(nextHandle));
    ++nextHandle;
    retainedTextBytes += bytes;
    ++stateVersion;
    return BufferInsertResult::Stored;
}

BufferInsertResult MessageBuffer::ensureConversation(
    const ConversationKey& key) {
    std::lock_guard<std::mutex> lock(mutex);
    return ensureConversationLocked(key);
}

bool MessageBuffer::sameIncoming(
    const BufferedMessage& stored,
    const ConversationKey& key,
    const std::string& sender,
    std::int64_t senderId,
    const std::string& content,
    std::int64_t createdAt) const noexcept {
    return !stored.outgoing() && stored.key() == key &&
        stored.sender() == sender && stored.senderId() &&
        *stored.senderId() == senderId && stored.content() == content &&
        stored.createdAt() && *stored.createdAt() == createdAt;
}

BufferInsertResult MessageBuffer::receiveValidated(const json& message) {
    std::int64_t messageId = 0;
    std::int64_t senderId = 0;
    std::int64_t createdAt = 0;
    std::string kind;
    std::string sender;
    std::string content;
    if (!readPositiveInteger(message, "message_id", messageId) ||
        !readPositiveInteger(message, "sender_id", senderId) ||
        !readNonnegativeInteger(message, "created_at", createdAt) ||
        !readString(message, "kind", kind) ||
        !readString(message, "sender", sender) ||
        !readString(message, "content", content)) {
        return BufferInsertResult::Conflict;
    }

    ConversationKey key = ConversationKey::broadcast();
    if (kind == "PRIVATE") {
        key = ConversationKey::privateChat(sender);
    } else if (kind == "GROUP") {
        std::int64_t groupId = 0;
        if (!readPositiveInteger(message, "group_id", groupId)) {
            return BufferInsertResult::Conflict;
        }
        key = ConversationKey::groupChat(groupId);
    } else if (kind != "BROADCAST") {
        return BufferInsertResult::Conflict;
    }

    std::lock_guard<std::mutex> lock(mutex);
    const auto existing = messagesById.find(messageId);
    if (existing != messagesById.end()) {
        return sameIncoming(
            *existing->second, key, sender, senderId, content, createdAt)
            ? BufferInsertResult::Duplicate
            : BufferInsertResult::Conflict;
    }

    const bool newConversation = conversations.find(key) == conversations.end();
    const std::size_t bytes = messageTextBytes(key, sender, content) +
        (newConversation ? keyTextBytes(key) : 0);
    if (messagesById.size() + (reservedMessage ? 1u : 0u) >= MaxMessages ||
        (newConversation && conversations.size() >= MaxConversations) ||
        bytes > MaxTextBytes - retainedTextBytes - reservedTextBytes ||
        nextArrivalSequence == 0 ||
        stateVersion == std::numeric_limits<std::uint64_t>::max()) {
        return BufferInsertResult::CapacityExceeded;
    }

    const auto stored = std::make_shared<const BufferedMessage>(
        messageId,
        key,
        sender,
        senderId,
        content,
        createdAt,
        false,
        nextArrivalSequence);
    auto indexPosition = messagesById.emplace(0, stored).first;
    auto indexNode = messagesById.extract(indexPosition);
    if (newConversation) {
        ConversationData prepared(nextHandle);
        prepared.messages.push_back(stored);
        prepared.unreadSequences.insert(nextArrivalSequence);
        conversations.emplace(key, std::move(prepared));
    } else {
        auto& conversation = conversations.at(key);
        conversation.messages.reserve(conversation.messages.size() + 1);
        auto unreadPosition =
            conversation.unreadSequences.insert(nextArrivalSequence).first;
        auto unreadNode = conversation.unreadSequences.extract(unreadPosition);
        const auto position = std::lower_bound(
            conversation.messages.begin(),
            conversation.messages.end(),
            messageId,
            [](const std::shared_ptr<const BufferedMessage>& current,
               std::int64_t id) {
                return current->messageId() < id;
            });
        conversation.messages.insert(position, stored);
        conversation.unreadSequences.insert(std::move(unreadNode));
    }
    indexNode.key() = messageId;
    messagesById.insert(std::move(indexNode));
    if (newConversation) {
        ++nextHandle;
    }
    retainedTextBytes += bytes;
    ++nextArrivalSequence;
    ++stateVersion;
    return BufferInsertResult::Stored;
}

BufferInsertResult MessageBuffer::reserveOutgoing(
    const ConversationKey& key,
    const std::string& sender,
    const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex);
    if (reservedMessage) {
        return BufferInsertResult::Conflict;
    }
    const bool newConversation = conversations.find(key) == conversations.end();
    const std::size_t reservationBytes =
        messageTextBytes(key, sender, content);
    const std::size_t conversationBytes =
        newConversation ? keyTextBytes(key) : 0;
    if (messagesById.size() >= MaxMessages ||
        (newConversation && conversations.size() >= MaxConversations) ||
        reservationBytes + conversationBytes >
            MaxTextBytes - retainedTextBytes ||
        stateVersion == std::numeric_limits<std::uint64_t>::max()) {
        return BufferInsertResult::CapacityExceeded;
    }
    try {
        auto preparedMessage = std::make_shared<BufferedMessage>(
            0, key, sender, std::nullopt, content, std::nullopt, true, 0);
        auto preparedIndexPosition =
            messagesById.emplace(0, preparedMessage).first;
        auto preparedIndexNode = messagesById.extract(preparedIndexPosition);
        if (newConversation) {
            ConversationData preparedConversation(nextHandle);
            preparedConversation.messages.reserve(1);
            conversations.emplace(key, std::move(preparedConversation));
            ++nextHandle;
            retainedTextBytes += conversationBytes;
        } else {
            auto& records = conversations.at(key).messages;
            records.reserve(records.size() + 1);
        }
        reservedMessage = std::move(preparedMessage);
        reservedIndexNode = std::move(preparedIndexNode);
        reservedConversationCreated = newConversation;
        reservedTextBytes = reservationBytes;
        ++stateVersion;
    } catch (...) {
        if (newConversation) {
            const auto found = conversations.find(key);
            if (found != conversations.end() && found->second.messages.empty()) {
                conversations.erase(found);
                --nextHandle;
                retainedTextBytes -= conversationBytes;
            }
        }
        throw;
    }
    return BufferInsertResult::Stored;
}

BufferInsertResult MessageBuffer::commitOutgoing(std::int64_t messageId) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!reservedMessage || messageId <= 0) {
        return BufferInsertResult::Conflict;
    }
    if (messagesById.find(messageId) != messagesById.end() ||
        nextArrivalSequence == 0 ||
        stateVersion == std::numeric_limits<std::uint64_t>::max()) {
        return BufferInsertResult::Conflict;
    }
    reservedMessage->acceptOutgoing(messageId, nextArrivalSequence);
    auto& records = conversations.at(reservedMessage->key()).messages;
    const auto position = std::lower_bound(
        records.begin(), records.end(), messageId,
        [](const std::shared_ptr<const BufferedMessage>& current,
           std::int64_t id) {
            return current->messageId() < id;
        });
    records.insert(position, reservedMessage);
    reservedIndexNode.key() = messageId;
    messagesById.insert(std::move(reservedIndexNode));
    retainedTextBytes += reservedTextBytes;
    reservedMessage.reset();
    reservedConversationCreated = false;
    reservedTextBytes = 0;
    ++nextArrivalSequence;
    ++stateVersion;
    return BufferInsertResult::Stored;
}

void MessageBuffer::cancelOutgoing() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        if (!reservedMessage) {
            return;
        }
        const ConversationKey key = reservedMessage->key();
        reservedMessage.reset();
        reservedIndexNode = {};
        reservedTextBytes = 0;
        const auto found = conversations.find(key);
        if (reservedConversationCreated && found != conversations.end() &&
            found->second.messages.empty()) {
            retainedTextBytes -= keyTextBytes(key);
            conversations.erase(found);
            --nextHandle;
        }
        reservedConversationCreated = false;
        if (stateVersion != std::numeric_limits<std::uint64_t>::max()) {
            ++stateVersion;
        }
    } catch (...) {
    }
}

BufferSnapshot MessageBuffer::snapshot(
    const std::optional<ConversationKey>& active) const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<ConversationSummary> summaries;
    summaries.reserve(conversations.size());
    for (const auto& entry : conversations) {
        const auto& unread = entry.second.unreadSequences;
        summaries.emplace_back(
            entry.first,
            entry.second.handle,
            entry.second.messages.size(),
            unread.size(),
            unread.empty() ? 0 : *unread.rbegin());
    }
    std::vector<std::shared_ptr<const BufferedMessage>> activeMessages;
    std::uint64_t maximumArrival = 0;
    if (active) {
        const auto found = conversations.find(*active);
        if (found != conversations.end()) {
            activeMessages = found->second.messages;
            for (const auto& message : activeMessages) {
                maximumArrival = std::max(
                    maximumArrival, message->arrivalSequence());
            }
        }
    }
    return BufferSnapshot(
        stateVersion,
        std::move(summaries),
        std::move(activeMessages),
        maximumArrival);
}

bool MessageBuffer::markViewed(
    const ConversationKey& key,
    std::uint64_t throughArrivalSequence) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = conversations.find(key);
    if (found == conversations.end()) {
        return false;
    }
    auto& unread = found->second.unreadSequences;
    const auto end = unread.upper_bound(throughArrivalSequence);
    if (end == unread.begin()) {
        return false;
    }
    unread.erase(unread.begin(), end);
    if (stateVersion == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Message-buffer version exhausted");
    }
    ++stateVersion;
    return true;
}

std::uint64_t MessageBuffer::version() const {
    std::lock_guard<std::mutex> lock(mutex);
    return stateVersion;
}
