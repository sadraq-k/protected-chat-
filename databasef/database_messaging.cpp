#include "../database.h"
#include "database_internal.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

using protected_chat::database_detail::Statement;
using protected_chat::database_detail::TransactionGuard;
using protected_chat::database_detail::bindInt64;
using protected_chat::database_detail::bindText;
using protected_chat::database_detail::databaseError;
using protected_chat::database_detail::finalizeSuccessfulStatement;
using protected_chat::database_detail::isValidUtf8;
using protected_chat::database_detail::prepareStatement;
using protected_chat::database_detail::readText;
using protected_chat::database_detail::recordExists;

namespace {

std::string kindText(DurableMessageKind kind) {
    switch (kind) {
        case DurableMessageKind::Private:
            return "PRIVATE";
        case DurableMessageKind::Group:
            return "GROUP";
        case DurableMessageKind::Broadcast:
            return "BROADCAST";
    }
    throw std::invalid_argument("Invalid durable message kind");
}

DurableMessageKind parseKind(const std::string& kind) {
    if (kind == "PRIVATE") {
        return DurableMessageKind::Private;
    }
    if (kind == "GROUP") {
        return DurableMessageKind::Group;
    }
    if (kind == "BROADCAST") {
        return DurableMessageKind::Broadcast;
    }
    throw std::runtime_error("Database contains an invalid message kind");
}

void validatePageArguments(
    std::int64_t requesterId,
    std::int64_t afterMessageId,
    std::size_t pageLimit) {
    if (requesterId <= 0) {
        throw std::invalid_argument("Requester ID must be positive");
    }
    if (afterMessageId < 0) {
        throw std::invalid_argument("History cursor cannot be negative");
    }
    if (pageLimit == 0 || pageLimit > Database::MaxPageSize) {
        throw std::invalid_argument("Page limit must be between 1 and 1000");
    }
}

void validateWireUsername(const std::string& username) {
    constexpr std::size_t MaxWireTextBytes = 65'536;
    if (username.empty() || username.size() > MaxWireTextBytes ||
        !isValidUtf8(username)) {
        throw std::invalid_argument("Username is not representable");
    }
}

void validateAcceptedText(
    const std::string& trustedSenderUsername,
    const std::string& content) {
    constexpr std::size_t MaxWireTextBytes = 65'536;
    validateWireUsername(trustedSenderUsername);
    if (content.empty() || content.size() > MaxWireTextBytes ||
        !isValidUtf8(content)) {
        throw std::invalid_argument("Message content is not representable");
    }
}

void validateDeliveryPageArguments(
    std::int64_t afterMessageId,
    const std::optional<std::int64_t>& throughMessageId,
    std::size_t pageLimit) {
    if (afterMessageId < 0 ||
        (throughMessageId &&
         (*throughMessageId < 0 || afterMessageId > *throughMessageId)) ||
        (!throughMessageId && afterMessageId != 0)) {
        throw std::invalid_argument("Invalid message-page cursor");
    }
    if (pageLimit == 0 || pageLimit > Database::MaxPageSize) {
        throw std::invalid_argument("Invalid message-page limit");
    }
}

std::vector<StoredMessage> readStoredMessages(
    sqlite3* db,
    Statement& statement,
    const std::string& operation) {
    std::vector<StoredMessage> messages;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        std::optional<std::int64_t> groupId;
        if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL) {
            groupId = sqlite3_column_int64(statement.get(), 3);
        }
        messages.emplace_back(
            sqlite3_column_int64(statement.get(), 0),
            parseKind(readText(statement.get(), 1)),
            sqlite3_column_int64(statement.get(), 2),
            groupId,
            readText(statement.get(), 4),
            sqlite3_column_int64(statement.get(), 5));
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, operation, stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize " + operation);
    return messages;
}



} // namespace

StoredMessage::StoredMessage(
    std::int64_t id,
    DurableMessageKind kind,
    std::int64_t senderId,
    std::optional<std::int64_t> groupId,
    std::string content,
    std::int64_t createdAt)
    : messageId(id),
      messageKind(kind),
      messageSenderId(senderId),
      messageGroupId(groupId),
      messageContent(std::move(content)),
      messageCreatedAt(createdAt) {}

std::int64_t StoredMessage::id() const noexcept {
    return messageId;
}

DurableMessageKind StoredMessage::kind() const noexcept {
    return messageKind;
}

std::int64_t StoredMessage::senderId() const noexcept {
    return messageSenderId;
}

const std::optional<std::int64_t>& StoredMessage::groupId() const noexcept {
    return messageGroupId;
}

const std::string& StoredMessage::content() const noexcept {
    return messageContent;
}

std::int64_t StoredMessage::createdAt() const noexcept {
    return messageCreatedAt;
}

DeliveryMessage::DeliveryMessage(
    StoredMessage message,
    std::string senderUsername)
    : storedMessage(std::move(message)),
      messageSenderUsername(std::move(senderUsername)) {}

const StoredMessage& DeliveryMessage::message() const noexcept {
    return storedMessage;
}

const std::string& DeliveryMessage::senderUsername() const noexcept {
    return messageSenderUsername;
}

RecipientRoute::RecipientRoute(
    std::int64_t userId,
    std::string username)
    : recipientUserId(userId), recipientUsername(std::move(username)) {}

std::int64_t RecipientRoute::userId() const noexcept {
    return recipientUserId;
}

const std::string& RecipientRoute::username() const noexcept {
    return recipientUsername;
}

AcceptedMessage::AcceptedMessage(
    DeliveryMessage message,
    std::vector<RecipientRoute> recipients)
    : acceptedDeliveryMessage(std::move(message)),
      acceptedRecipients(std::move(recipients)) {}

const DeliveryMessage& AcceptedMessage::message() const noexcept {
    return acceptedDeliveryMessage;
}

const std::vector<RecipientRoute>&
AcceptedMessage::recipients() const noexcept {
    return acceptedRecipients;
}

MessageAcceptanceResult::MessageAcceptanceResult(
    MessageAcceptanceStatus status,
    std::optional<AcceptedMessage> acceptedMessage)
    : acceptanceStatus(status), committedMessage(std::move(acceptedMessage)) {
    if ((status == MessageAcceptanceStatus::Accepted) !=
        committedMessage.has_value()) {
        throw std::invalid_argument("Invalid message acceptance result");
    }
}

MessageAcceptanceStatus MessageAcceptanceResult::status() const noexcept {
    return acceptanceStatus;
}

const std::optional<AcceptedMessage>&
MessageAcceptanceResult::acceptedMessage() const noexcept {
    return committedMessage;
}

MessagePageResult::MessagePageResult(
    MessagePageStatus status,
    std::vector<DeliveryMessage> messages,
    std::int64_t throughMessageId,
    bool hasMore)
    : pageStatus(status),
      pageMessages(std::move(messages)),
      pageThroughMessageId(throughMessageId),
      moreMessages(hasMore) {
    if (status == MessagePageStatus::UserNotFound &&
        (!pageMessages.empty() || pageThroughMessageId != 0 || moreMessages)) {
        throw std::invalid_argument("Invalid missing-user page result");
    }
}

MessagePageStatus MessagePageResult::status() const noexcept {
    return pageStatus;
}

const std::vector<DeliveryMessage>& MessagePageResult::messages() const noexcept {
    return pageMessages;
}

std::int64_t MessagePageResult::throughMessageId() const noexcept {
    return pageThroughMessageId;
}

bool MessagePageResult::hasMore() const noexcept {
    return moreMessages;
}

void Database::storeOfflineMessages(
    const std::string& sender,
    const std::string& receiver,
    const std::string& message) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    Statement statement = prepareStatement(
        db,
        "INSERT INTO messages (sender, receiver, message) VALUES (?, ?, ?);",
        "Prepare offline message storage");
    bindText(db, statement.get(), 1, sender, "Bind offline message sender");
    bindText(db, statement.get(), 2, receiver, "Bind offline message receiver");
    bindText(db, statement.get(), 3, message, "Bind offline message content");

    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Store offline message", stepResult);
    }

    finalizeSuccessfulStatement(
        db, statement, "Finalize offline message storage");
    std::cout << "[DB] Stored offline message from " << sender << " to "
              << receiver << std::endl;
}

std::vector<Message> Database::getOfflineMessages(
    const std::string& username) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    std::cout << "[DB] Fetching offline messages for: " << username << std::endl;

    Statement statement = prepareStatement(
        db,
        "SELECT sender, receiver, message FROM messages WHERE receiver = ?;",
        "Prepare offline message query");
    bindText(db, statement.get(), 1, username, "Bind offline message receiver");

    std::vector<Message> messages;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        messages.push_back({
            readText(statement.get(), 0),
            readText(statement.get(), 1),
            readText(statement.get(), 2)
        });
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Read offline messages", stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize offline message query");

    std::cout << "[DB] Fetched " << messages.size() << " offline messages"
              << std::endl;
    return messages;
}

void Database::clearOfflineMessages(const std::string& username) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    Statement statement = prepareStatement(
        db,
        "DELETE FROM messages WHERE receiver = ?;",
        "Prepare offline message deletion");
    bindText(db, statement.get(), 1, username, "Bind offline message receiver");

    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Clear offline messages", stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize offline message deletion");

    std::cout << "[DB] Cleared offline messages for: " << username << std::endl;
}

std::int64_t Database::insertDurableMessage(
    DurableMessageKind kind,
    std::int64_t senderId,
    const std::optional<std::int64_t>& groupId,
    const std::string& content,
    const std::vector<std::int64_t>& recipientIds) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin durable message insertion");
    try {
        const std::string storedKind = kindText(kind);
        if (senderId <= 0) {
            throw std::invalid_argument("Sender ID must be positive");
        }
        if (content.empty()) {
            throw std::invalid_argument("Message content cannot be empty");
        }
        if (kind == DurableMessageKind::Group) {
            if (!groupId || *groupId <= 0) {
                throw std::invalid_argument(
                    "GROUP messages require a positive group ID");
            }
        } else if (groupId) {
            throw std::invalid_argument(
                "Only GROUP messages may have a group ID");
        }
        if (kind == DurableMessageKind::Private && recipientIds.size() != 1) {
            throw std::invalid_argument(
                "PRIVATE messages require exactly one recipient");
        }

        std::unordered_set<std::int64_t> uniqueRecipients;
        for (const std::int64_t recipientId : recipientIds) {
            if (recipientId <= 0) {
                throw std::invalid_argument("Recipient IDs must be positive");
            }
            if (recipientId == senderId) {
                throw std::invalid_argument(
                    "Durable recipient snapshots must exclude the sender");
            }
            if (!uniqueRecipients.insert(recipientId).second) {
                throw std::invalid_argument(
                    "Durable recipient snapshots cannot contain duplicates");
            }
        }

        if (!recordExists(
                db,
                "SELECT 1 FROM users WHERE id = ?;",
                senderId,
                "Validate durable sender")) {
            throw std::invalid_argument("Durable sender does not exist");
        }
        for (const std::int64_t recipientId : recipientIds) {
            if (!recordExists(
                    db,
                    "SELECT 1 FROM users WHERE id = ?;",
                    recipientId,
                    "Validate durable recipient")) {
                throw std::invalid_argument(
                    "A durable message recipient does not exist");
            }
        }
        if (groupId && !recordExists(
                db,
                "SELECT 1 FROM chat_groups WHERE id = ?;",
                *groupId,
                "Validate durable group")) {
            throw std::invalid_argument("Durable message group does not exist");
        }

        const std::int64_t messageId = insertDurableMessageCallerLocked(
            kind, senderId, groupId, content, recipientIds);
        transaction.commit("Commit durable message insertion");
        return messageId;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; durable insertion rollback failed: " + rollbackError);
        }
        throw;
    }
}

std::int64_t Database::insertDurableMessageCallerLocked(
    DurableMessageKind kind,
    std::int64_t senderId,
    const std::optional<std::int64_t>& groupId,
    const std::string& content,
    const std::vector<std::int64_t>& recipientIds) {
    Statement history = prepareStatement(
        db,
        "INSERT INTO message_history "
        "(kind, sender_id, group_id, content, created_at) "
        "VALUES (?, ?, ?, ?, CAST(strftime('%s', 'now') AS INTEGER));",
        "Prepare durable history insertion");
    bindText(db, history.get(), 1, kindText(kind), "Bind durable message kind");
    bindInt64(db, history.get(), 2, senderId, "Bind durable sender");
    int bindResult = groupId
        ? sqlite3_bind_int64(history.get(), 3, *groupId)
        : sqlite3_bind_null(history.get(), 3);
    if (bindResult != SQLITE_OK) {
        throw databaseError(db, "Bind durable group", bindResult);
    }
    bindText(db, history.get(), 4, content, "Bind durable content");

    const int historyResult = sqlite3_step(history.get());
    if (historyResult != SQLITE_DONE) {
        throw databaseError(db, "Insert durable history", historyResult);
    }
    finalizeSuccessfulStatement(db, history, "Finalize durable history insertion");

    const std::int64_t messageId = sqlite3_last_insert_rowid(db);
    if (messageId <= 0) {
        throw std::runtime_error("SQLite returned an invalid durable message ID");
    }

    Statement recipient = prepareStatement(
        db,
        "INSERT INTO message_recipients "
        "(message_id, recipient_id, acknowledged_at) VALUES (?, ?, NULL);",
        "Prepare durable recipient insertion");
    for (const std::int64_t recipientId : recipientIds) {
        sqlite3_reset(recipient.get());
        sqlite3_clear_bindings(recipient.get());
        bindInt64(
            db, recipient.get(), 1, messageId, "Bind recipient message ID");
        bindInt64(
            db, recipient.get(), 2, recipientId, "Bind durable recipient ID");
        const int recipientResult = sqlite3_step(recipient.get());
        if (recipientResult != SQLITE_DONE) {
            throw databaseError(
                db, "Insert durable message recipient", recipientResult);
        }
    }
    finalizeSuccessfulStatement(
        db, recipient, "Finalize durable recipient insertion");
    return messageId;
}

AcceptedMessage Database::buildAcceptedMessageCallerLocked(
    std::int64_t messageId,
    const std::string& senderUsername) {
    Statement message = prepareStatement(
        db,
        "SELECT id, kind, sender_id, group_id, content, created_at "
        "FROM message_history WHERE id = ?;",
        "Prepare accepted-message lookup");
    bindInt64(db, message.get(), 1, messageId, "Bind accepted message ID");
    const int messageResult = sqlite3_step(message.get());
    if (messageResult != SQLITE_ROW) {
        throw databaseError(db, "Read accepted message", messageResult);
    }
    std::optional<std::int64_t> groupId;
    if (sqlite3_column_type(message.get(), 3) != SQLITE_NULL) {
        groupId = sqlite3_column_int64(message.get(), 3);
    }
    DeliveryMessage delivery(
        StoredMessage(
            sqlite3_column_int64(message.get(), 0),
            parseKind(readText(message.get(), 1)),
            sqlite3_column_int64(message.get(), 2),
            groupId,
            readText(message.get(), 4),
            sqlite3_column_int64(message.get(), 5)),
        senderUsername);
    if (sqlite3_step(message.get()) != SQLITE_DONE) {
        throw databaseError(
            db, "Complete accepted-message lookup", sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(
        db, message, "Finalize accepted-message lookup");

    Statement routes = prepareStatement(
        db,
        "SELECT r.recipient_id, u.username "
        "FROM message_recipients AS r "
        "JOIN users AS u ON u.id = r.recipient_id "
        "WHERE r.message_id = ? ORDER BY r.recipient_id;",
        "Prepare committed recipient routes");
    bindInt64(db, routes.get(), 1, messageId, "Bind routed message ID");
    std::vector<RecipientRoute> recipients;
    int routeResult = SQLITE_OK;
    while ((routeResult = sqlite3_step(routes.get())) == SQLITE_ROW) {
        recipients.emplace_back(
            sqlite3_column_int64(routes.get(), 0), readText(routes.get(), 1));
    }
    if (routeResult != SQLITE_DONE) {
        throw databaseError(db, "Read committed recipient routes", routeResult);
    }
    finalizeSuccessfulStatement(
        db, routes, "Finalize committed recipient routes");
    return AcceptedMessage(std::move(delivery), std::move(recipients));
}

MessageAcceptanceResult Database::acceptPrivateMessage(
    const std::string& trustedSenderUsername,
    const std::string& recipientUsername,
    const std::string& content) {
    validateAcceptedText(trustedSenderUsername, content);
    if (recipientUsername.empty()) {
        throw std::invalid_argument("Recipient username cannot be empty");
    }
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin Private message acceptance");
    try {
        const std::optional<std::int64_t> senderId =
            findUserIdCallerLocked(trustedSenderUsername);
        if (!senderId) {
            transaction.commit("Commit missing Private sender lookup");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::SenderNotFound);
        }
        const std::optional<std::int64_t> recipientId =
            findUserIdCallerLocked(recipientUsername);
        if (!recipientId) {
            transaction.commit("Commit missing Private recipient lookup");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::RecipientNotFound);
        }
        if (*senderId == *recipientId) {
            transaction.commit("Commit rejected Private self-send");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::SelfMessageNotAllowed);
        }

        const std::int64_t messageId = insertDurableMessageCallerLocked(
            DurableMessageKind::Private,
            *senderId,
            std::nullopt,
            content,
            {*recipientId});
        AcceptedMessage accepted = buildAcceptedMessageCallerLocked(
            messageId, trustedSenderUsername);
        MessageAcceptanceResult result(
            MessageAcceptanceStatus::Accepted, std::move(accepted));
        transaction.commit("Commit Private message acceptance");
        return result;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; Private acceptance rollback failed: " + rollbackError);
        }
        throw;
    }
}

MessageAcceptanceResult Database::acceptGroupMessage(
    const std::string& trustedSenderUsername,
    std::int64_t groupId,
    const std::string& content) {
    validateAcceptedText(trustedSenderUsername, content);
    if (groupId <= 0) {
        throw std::invalid_argument("Group ID must be positive");
    }
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin Group message acceptance");
    try {
        const std::optional<std::int64_t> senderId =
            findUserIdCallerLocked(trustedSenderUsername);
        if (!senderId) {
            transaction.commit("Commit missing Group sender lookup");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::SenderNotFound);
        }
        if (!groupExistsCallerLocked(groupId)) {
            transaction.commit("Commit missing Group lookup");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::GroupNotFound);
        }
        if (!isGroupMemberCallerLocked(groupId, *senderId)) {
            transaction.commit("Commit rejected Group nonmember");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::SenderNotMember);
        }
        const std::vector<std::int64_t> recipientIds =
            groupRecipientIdsCallerLocked(groupId, *senderId);
        const std::int64_t messageId = insertDurableMessageCallerLocked(
            DurableMessageKind::Group,
            *senderId,
            groupId,
            content,
            recipientIds);
        AcceptedMessage accepted = buildAcceptedMessageCallerLocked(
            messageId, trustedSenderUsername);
        MessageAcceptanceResult result(
            MessageAcceptanceStatus::Accepted, std::move(accepted));
        transaction.commit("Commit Group message acceptance");
        return result;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; Group acceptance rollback failed: " + rollbackError);
        }
        throw;
    }
}

MessageAcceptanceResult Database::acceptBroadcastMessage(
    const std::string& trustedSenderUsername,
    const std::string& content) {
    validateAcceptedText(trustedSenderUsername, content);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin Broadcast message acceptance");
    try {
        const std::optional<std::int64_t> senderId =
            findUserIdCallerLocked(trustedSenderUsername);
        if (!senderId) {
            transaction.commit("Commit missing Broadcast sender lookup");
            return MessageAcceptanceResult(
                MessageAcceptanceStatus::SenderNotFound);
        }

        Statement recipients = prepareStatement(
            db,
            "SELECT id FROM users WHERE id <> ? ORDER BY id;",
            "Prepare Broadcast recipient snapshot");
        bindInt64(
            db, recipients.get(), 1, *senderId, "Bind Broadcast sender ID");
        std::vector<std::int64_t> recipientIds;
        int recipientResult = SQLITE_OK;
        while ((recipientResult = sqlite3_step(recipients.get())) == SQLITE_ROW) {
            recipientIds.push_back(sqlite3_column_int64(recipients.get(), 0));
        }
        if (recipientResult != SQLITE_DONE) {
            throw databaseError(
                db, "Read Broadcast recipient snapshot", recipientResult);
        }
        finalizeSuccessfulStatement(
            db, recipients, "Finalize Broadcast recipient snapshot");

        const std::int64_t messageId = insertDurableMessageCallerLocked(
            DurableMessageKind::Broadcast,
            *senderId,
            std::nullopt,
            content,
            recipientIds);
        AcceptedMessage accepted = buildAcceptedMessageCallerLocked(
            messageId, trustedSenderUsername);
        MessageAcceptanceResult result(
            MessageAcceptanceStatus::Accepted, std::move(accepted));
        transaction.commit("Commit Broadcast message acceptance");
        return result;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; Broadcast acceptance rollback failed: " + rollbackError);
        }
        throw;
    }
}

std::vector<StoredMessage> Database::getPendingMessages(
    std::int64_t recipientId,
    std::int64_t afterMessageId,
    std::size_t pageLimit) {
    validatePageArguments(recipientId, afterMessageId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    if (!recordExists(
            db,
            "SELECT 1 FROM users WHERE id = ?;",
            recipientId,
            "Validate pending-message recipient")) {
        throw std::invalid_argument("Pending-message recipient does not exist");
    }

    Statement statement = prepareStatement(
        db,
        "SELECT h.id, h.kind, h.sender_id, h.group_id, h.content, h.created_at "
        "FROM message_recipients AS r "
        "JOIN message_history AS h ON h.id = r.message_id "
        "WHERE r.recipient_id = ? AND r.acknowledged_at IS NULL "
        "AND h.id > ? ORDER BY h.id LIMIT ?;",
        "Prepare pending-message query");
    bindInt64(db, statement.get(), 1, recipientId, "Bind pending recipient");
    bindInt64(db, statement.get(), 2, afterMessageId, "Bind pending cursor");
    bindInt64(
        db,
        statement.get(),
        3,
        static_cast<std::int64_t>(pageLimit),
        "Bind pending page limit");
    return readStoredMessages(db, statement, "Read pending messages");
}

AcknowledgementResult Database::acknowledgeMessage(
    std::int64_t messageId,
    std::int64_t recipientId) {
    if (messageId <= 0 || recipientId <= 0) {
        throw std::invalid_argument(
            "Acknowledgement IDs must be positive");
    }
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin message acknowledgement");
    try {
        const AcknowledgementResult result =
            acknowledgeMessageCallerLocked(messageId, recipientId);
        transaction.commit("Commit message acknowledgement");
        return result;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; acknowledgement rollback failed: " + rollbackError);
        }
        throw;
    }
}

AcknowledgementResult Database::acknowledgeMessageCallerLocked(
    std::int64_t messageId,
    std::int64_t recipientId) {
    Statement lookup = prepareStatement(
        db,
        "SELECT acknowledged_at FROM message_recipients "
        "WHERE message_id = ? AND recipient_id = ?;",
        "Prepare acknowledgement lookup");
    bindInt64(db, lookup.get(), 1, messageId, "Bind acknowledged message ID");
    bindInt64(db, lookup.get(), 2, recipientId, "Bind acknowledging recipient");

    const int lookupResult = sqlite3_step(lookup.get());
    if (lookupResult == SQLITE_DONE) {
        finalizeSuccessfulStatement(
            db, lookup, "Finalize acknowledgement lookup");
        return AcknowledgementResult::NotFoundOrNotRecipient;
    }
    if (lookupResult != SQLITE_ROW) {
        throw databaseError(db, "Read acknowledgement state", lookupResult);
    }
    const bool alreadyAcknowledged =
        sqlite3_column_type(lookup.get(), 0) != SQLITE_NULL;
    if (sqlite3_step(lookup.get()) != SQLITE_DONE) {
        throw databaseError(
            db, "Complete acknowledgement lookup", sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, lookup, "Finalize acknowledgement lookup");
    if (alreadyAcknowledged) {
        return AcknowledgementResult::AlreadyAcknowledged;
    }

    Statement update = prepareStatement(
        db,
        "UPDATE message_recipients "
        "SET acknowledged_at = CAST(strftime('%s', 'now') AS INTEGER) "
        "WHERE message_id = ? AND recipient_id = ? "
        "AND acknowledged_at IS NULL;",
        "Prepare message acknowledgement");
    bindInt64(db, update.get(), 1, messageId, "Bind acknowledged message ID");
    bindInt64(db, update.get(), 2, recipientId, "Bind acknowledging recipient");
    const int updateResult = sqlite3_step(update.get());
    if (updateResult != SQLITE_DONE) {
        throw databaseError(db, "Acknowledge message", updateResult);
    }
    if (sqlite3_changes(db) != 1) {
        throw std::runtime_error("Acknowledgement state changed unexpectedly");
    }
    finalizeSuccessfulStatement(
        db, update, "Finalize message acknowledgement");
    return AcknowledgementResult::Acknowledged;
}

AcknowledgementResult Database::acknowledgeDelivery(
    const std::string& trustedUsername,
    std::int64_t messageId) {
    if (trustedUsername.empty() || messageId <= 0) {
        throw std::invalid_argument(
            "Delivery acknowledgement requires a username and positive ID");
    }
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin delivery acknowledgement");
    try {
        const std::optional<std::int64_t> recipientId =
            findUserIdCallerLocked(trustedUsername);
        const AcknowledgementResult result = recipientId
            ? acknowledgeMessageCallerLocked(messageId, *recipientId)
            : AcknowledgementResult::NotFoundOrNotRecipient;
        transaction.commit("Commit delivery acknowledgement");
        return result;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; delivery acknowledgement rollback failed: " +
                rollbackError);
        }
        throw;
    }
}

std::vector<StoredMessage> Database::getAuthorizedHistory(
    std::int64_t requesterId,
    std::int64_t afterMessageId,
    std::size_t pageLimit) {
    validatePageArguments(requesterId, afterMessageId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    if (!recordExists(
            db,
            "SELECT 1 FROM users WHERE id = ?;",
            requesterId,
            "Validate history requester")) {
        throw std::invalid_argument("History requester does not exist");
    }

    Statement statement = prepareStatement(
        db,
        "SELECT h.id, h.kind, h.sender_id, h.group_id, h.content, h.created_at "
        "FROM message_history AS h "
        "WHERE h.id > ? AND (h.sender_id = ? OR EXISTS ("
            "SELECT 1 FROM message_recipients AS r "
            "WHERE r.message_id = h.id AND r.recipient_id = ?)) "
        "ORDER BY h.id LIMIT ?;",
        "Prepare authorized-history query");
    bindInt64(db, statement.get(), 1, afterMessageId, "Bind history cursor");
    bindInt64(db, statement.get(), 2, requesterId, "Bind history sender");
    bindInt64(db, statement.get(), 3, requesterId, "Bind history recipient");
    bindInt64(
        db,
        statement.get(),
        4,
        static_cast<std::int64_t>(pageLimit),
        "Bind history page limit");
    return readStoredMessages(db, statement, "Read authorized history");
}

MessagePageResult Database::getPendingDeliveryPage(
    const std::string& trustedUsername,
    std::int64_t afterMessageId,
    const std::optional<std::int64_t>& throughMessageId,
    std::size_t pageLimit) {
    if (trustedUsername.empty()) {
        throw std::invalid_argument("Pending-page username cannot be empty");
    }
    validateDeliveryPageArguments(
        afterMessageId, throughMessageId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin pending-delivery page", false);
    try {
        const std::optional<std::int64_t> recipientId =
            findUserIdCallerLocked(trustedUsername);
        if (!recipientId) {
            transaction.commit("Commit missing pending-page user lookup");
            return MessagePageResult(
                MessagePageStatus::UserNotFound, {}, 0, false);
        }

        std::int64_t watermark = throughMessageId.value_or(0);
        if (!throughMessageId) {
            Statement maximum = prepareStatement(
                db,
                "SELECT COALESCE(MAX(message_id), 0) "
                "FROM message_recipients "
                "WHERE recipient_id = ? AND acknowledged_at IS NULL;",
                "Prepare pending-delivery watermark");
            bindInt64(
                db, maximum.get(), 1, *recipientId,
                "Bind pending-watermark recipient");
            const int maximumResult = sqlite3_step(maximum.get());
            if (maximumResult != SQLITE_ROW) {
                throw databaseError(
                    db, "Read pending-delivery watermark", maximumResult);
            }
            watermark = sqlite3_column_int64(maximum.get(), 0);
            if (sqlite3_step(maximum.get()) != SQLITE_DONE) {
                throw databaseError(
                    db, "Complete pending-delivery watermark", sqlite3_errcode(db));
            }
            finalizeSuccessfulStatement(
                db, maximum, "Finalize pending-delivery watermark");
        }

        Statement page = prepareStatement(
            db,
            "SELECT h.id, h.kind, h.sender_id, h.group_id, h.content, "
            "h.created_at, u.username "
            "FROM message_recipients AS r "
            "JOIN message_history AS h ON h.id = r.message_id "
            "JOIN users AS u ON u.id = h.sender_id "
            "WHERE r.recipient_id = ? AND r.acknowledged_at IS NULL "
            "AND h.id > ? AND h.id <= ? ORDER BY h.id LIMIT ?;",
            "Prepare pending-delivery page query");
        bindInt64(db, page.get(), 1, *recipientId, "Bind pending-page recipient");
        bindInt64(db, page.get(), 2, afterMessageId, "Bind pending-page cursor");
        bindInt64(db, page.get(), 3, watermark, "Bind pending-page watermark");
        bindInt64(
            db,
            page.get(),
            4,
            static_cast<std::int64_t>(pageLimit + 1),
            "Bind pending-page limit");

        std::vector<DeliveryMessage> messages;
        int pageResult = SQLITE_OK;
        while ((pageResult = sqlite3_step(page.get())) == SQLITE_ROW) {
            std::optional<std::int64_t> groupId;
            if (sqlite3_column_type(page.get(), 3) != SQLITE_NULL) {
                groupId = sqlite3_column_int64(page.get(), 3);
            }
            messages.emplace_back(
                StoredMessage(
                    sqlite3_column_int64(page.get(), 0),
                    parseKind(readText(page.get(), 1)),
                    sqlite3_column_int64(page.get(), 2),
                    groupId,
                    readText(page.get(), 4),
                    sqlite3_column_int64(page.get(), 5)),
                readText(page.get(), 6));
        }
        if (pageResult != SQLITE_DONE) {
            throw databaseError(db, "Read pending-delivery page", pageResult);
        }
        finalizeSuccessfulStatement(
            db, page, "Finalize pending-delivery page query");
        const bool hasMore = messages.size() > pageLimit;
        if (hasMore) {
            messages.pop_back();
        }
        transaction.commit("Commit pending-delivery page");
        return MessagePageResult(
            MessagePageStatus::Ready,
            std::move(messages),
            watermark,
            hasMore);
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; pending-page rollback failed: " + rollbackError);
        }
        throw;
    }
}

MessagePageResult Database::getHistoryDeliveryPage(
    const std::string& trustedUsername,
    std::int64_t afterMessageId,
    const std::optional<std::int64_t>& throughMessageId,
    std::size_t pageLimit) {
    if (trustedUsername.empty()) {
        throw std::invalid_argument("History-page username cannot be empty");
    }
    validateDeliveryPageArguments(
        afterMessageId, throughMessageId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin history-delivery page", false);
    try {
        const std::optional<std::int64_t> requesterId =
            findUserIdCallerLocked(trustedUsername);
        if (!requesterId) {
            transaction.commit("Commit missing history-page user lookup");
            return MessagePageResult(
                MessagePageStatus::UserNotFound, {}, 0, false);
        }

        std::int64_t watermark = throughMessageId.value_or(0);
        if (!throughMessageId) {
            Statement maximum = prepareStatement(
                db,
                "SELECT COALESCE(MAX(h.id), 0) FROM message_history AS h "
                "WHERE h.sender_id = ? OR EXISTS ("
                    "SELECT 1 FROM message_recipients AS r "
                    "WHERE r.message_id = h.id AND r.recipient_id = ?);",
                "Prepare history-delivery watermark");
            bindInt64(db, maximum.get(), 1, *requesterId, "Bind history sender");
            bindInt64(db, maximum.get(), 2, *requesterId, "Bind history recipient");
            const int maximumResult = sqlite3_step(maximum.get());
            if (maximumResult != SQLITE_ROW) {
                throw databaseError(
                    db, "Read history-delivery watermark", maximumResult);
            }
            watermark = sqlite3_column_int64(maximum.get(), 0);
            if (sqlite3_step(maximum.get()) != SQLITE_DONE) {
                throw databaseError(
                    db, "Complete history-delivery watermark", sqlite3_errcode(db));
            }
            finalizeSuccessfulStatement(
                db, maximum, "Finalize history-delivery watermark");
        }

        Statement page = prepareStatement(
            db,
            "SELECT h.id, h.kind, h.sender_id, h.group_id, h.content, "
            "h.created_at, u.username FROM message_history AS h "
            "JOIN users AS u ON u.id = h.sender_id "
            "WHERE h.id > ? AND h.id <= ? AND "
            "(h.sender_id = ? OR EXISTS ("
                "SELECT 1 FROM message_recipients AS r "
                "WHERE r.message_id = h.id AND r.recipient_id = ?)) "
            "ORDER BY h.id LIMIT ?;",
            "Prepare history-delivery page query");
        bindInt64(db, page.get(), 1, afterMessageId, "Bind history-page cursor");
        bindInt64(db, page.get(), 2, watermark, "Bind history-page watermark");
        bindInt64(db, page.get(), 3, *requesterId, "Bind history-page sender");
        bindInt64(db, page.get(), 4, *requesterId, "Bind history-page recipient");
        bindInt64(
            db,
            page.get(),
            5,
            static_cast<std::int64_t>(pageLimit + 1),
            "Bind history-page limit");

        std::vector<DeliveryMessage> messages;
        int pageResult = SQLITE_OK;
        while ((pageResult = sqlite3_step(page.get())) == SQLITE_ROW) {
            std::optional<std::int64_t> groupId;
            if (sqlite3_column_type(page.get(), 3) != SQLITE_NULL) {
                groupId = sqlite3_column_int64(page.get(), 3);
            }
            messages.emplace_back(
                StoredMessage(
                    sqlite3_column_int64(page.get(), 0),
                    parseKind(readText(page.get(), 1)),
                    sqlite3_column_int64(page.get(), 2),
                    groupId,
                    readText(page.get(), 4),
                    sqlite3_column_int64(page.get(), 5)),
                readText(page.get(), 6));
        }
        if (pageResult != SQLITE_DONE) {
            throw databaseError(db, "Read history-delivery page", pageResult);
        }
        finalizeSuccessfulStatement(
            db, page, "Finalize history-delivery page query");
        const bool hasMore = messages.size() > pageLimit;
        if (hasMore) {
            messages.pop_back();
        }
        transaction.commit("Commit history-delivery page");
        return MessagePageResult(
            MessagePageStatus::Ready,
            std::move(messages),
            watermark,
            hasMore);
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; history-page rollback failed: " + rollbackError);
        }
        throw;
    }
}

std::int64_t Database::importLegacyMessages() {
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    TransactionGuard transaction(db, "Begin legacy-message import");
    try {
        constexpr std::int64_t ImportPageSize = 100;
        std::int64_t afterLegacyId = 0;
        std::int64_t imported = 0;
        while (true) {
            Statement page = prepareStatement(
                db,
                "SELECT id, sender, receiver, message FROM messages "
                "WHERE id > ? ORDER BY id LIMIT ?;",
                "Prepare legacy-message import page");
            bindInt64(db, page.get(), 1, afterLegacyId, "Bind legacy cursor");
            bindInt64(db, page.get(), 2, ImportPageSize, "Bind legacy page size");
            std::vector<std::tuple<
                std::int64_t, std::string, std::string, std::string>> rows;
            int pageResult = SQLITE_OK;
            while ((pageResult = sqlite3_step(page.get())) == SQLITE_ROW) {
                rows.emplace_back(
                    sqlite3_column_int64(page.get(), 0),
                    readText(page.get(), 1),
                    readText(page.get(), 2),
                    readText(page.get(), 3));
            }
            if (pageResult != SQLITE_DONE) {
                throw databaseError(db, "Read legacy-message import page", pageResult);
            }
            finalizeSuccessfulStatement(
                db, page, "Finalize legacy-message import page");
            if (rows.empty()) {
                break;
            }

            for (const auto& row : rows) {
                const std::int64_t legacyId = std::get<0>(row);
                const std::string& sender = std::get<1>(row);
                const std::string& receiver = std::get<2>(row);
                const std::string& content = std::get<3>(row);
                try {
                    validateAcceptedText(sender, content);
                    validateWireUsername(receiver);
                } catch (const std::invalid_argument&) {
                    throw std::runtime_error(
                        "Legacy import rejected row " +
                        std::to_string(legacyId) + ": INVALID_TEXT");
                }
                const std::optional<std::int64_t> senderId =
                    findUserIdCallerLocked(sender);
                if (!senderId) {
                    throw std::runtime_error(
                        "Legacy import rejected row " +
                        std::to_string(legacyId) + ": SENDER_NOT_FOUND");
                }
                const std::optional<std::int64_t> receiverId =
                    findUserIdCallerLocked(receiver);
                if (!receiverId) {
                    throw std::runtime_error(
                        "Legacy import rejected row " +
                        std::to_string(legacyId) + ": RECIPIENT_NOT_FOUND");
                }
                if (*senderId == *receiverId) {
                    throw std::runtime_error(
                        "Legacy import rejected row " +
                        std::to_string(legacyId) + ": SELF_MESSAGE_NOT_ALLOWED");
                }

                insertDurableMessageCallerLocked(
                    DurableMessageKind::Private,
                    *senderId,
                    std::nullopt,
                    content,
                    {*receiverId});
                Statement deletion = prepareStatement(
                    db,
                    "DELETE FROM messages WHERE id = ?;",
                    "Prepare imported legacy-row deletion");
                bindInt64(
                    db, deletion.get(), 1, legacyId,
                    "Bind imported legacy-row ID");
                const int deletionResult = sqlite3_step(deletion.get());
                if (deletionResult != SQLITE_DONE || sqlite3_changes(db) != 1) {
                    throw std::runtime_error(
                        "Legacy import row deletion failed");
                }
                finalizeSuccessfulStatement(
                    db, deletion, "Finalize imported legacy-row deletion");
                afterLegacyId = legacyId;
                ++imported;
            }
        }
        transaction.commit("Commit legacy-message import");
        return imported;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; legacy import rollback failed: " + rollbackError);
        }
        throw;
    }
}
