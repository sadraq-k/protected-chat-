#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <optional>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <vector>

struct Message {
    std::string sender;
    std::string receiver;
    std::string message;
};

enum class RegistrationResult {
    Created,
    DuplicateUsername
};

enum class DurableMessageKind {
    Private,
    Group,
    Broadcast
};

enum class AcknowledgementResult {
    Acknowledged,
    AlreadyAcknowledged,
    NotFoundOrNotRecipient
};

enum class GroupCreationStatus {
    Created,
    DuplicateName,
    CreatorNotFound
};

enum class GroupJoinResult {
    Joined,
    AlreadyMember,
    GroupNotFound,
    UserNotFound
};

enum class GroupListStatus {
    Listed,
    UserNotFound
};

enum class GroupRecipientSnapshotStatus {
    Ready,
    GroupNotFound,
    UserNotFound,
    SenderNotMember
};

class GroupSummary {
public:
    GroupSummary(std::int64_t id, std::string name);

    std::int64_t id() const noexcept;
    const std::string& name() const noexcept;

private:
    std::int64_t groupId;
    std::string groupName;
};

class GroupCreationResult {
public:
    GroupCreationResult(
        GroupCreationStatus status,
        std::optional<GroupSummary> group = std::nullopt);

    GroupCreationStatus status() const noexcept;
    const std::optional<GroupSummary>& group() const noexcept;

private:
    GroupCreationStatus creationStatus;
    std::optional<GroupSummary> createdGroup;
};

class GroupListResult {
public:
    GroupListResult(
        GroupListStatus status,
        std::vector<GroupSummary> groups,
        bool hasMore);

    GroupListStatus status() const noexcept;
    const std::vector<GroupSummary>& groups() const noexcept;
    bool hasMore() const noexcept;

private:
    GroupListStatus listStatus;
    std::vector<GroupSummary> listedGroups;
    bool moreGroups;
};

class GroupRecipientSnapshot {
public:
    GroupRecipientSnapshot(
        GroupRecipientSnapshotStatus status,
        std::vector<std::int64_t> recipientIds);

    GroupRecipientSnapshotStatus status() const noexcept;
    const std::vector<std::int64_t>& recipientIds() const noexcept;

private:
    GroupRecipientSnapshotStatus snapshotStatus;
    std::vector<std::int64_t> snapshotRecipientIds;
};

class StoredMessage {
public:
    StoredMessage(
        std::int64_t id,
        DurableMessageKind kind,
        std::int64_t senderId,
        std::optional<std::int64_t> groupId,
        std::string content,
        std::int64_t createdAt);

    std::int64_t id() const noexcept;
    DurableMessageKind kind() const noexcept;
    std::int64_t senderId() const noexcept;
    const std::optional<std::int64_t>& groupId() const noexcept;
    const std::string& content() const noexcept;
    std::int64_t createdAt() const noexcept;

private:
    std::int64_t messageId;
    DurableMessageKind messageKind;
    std::int64_t messageSenderId;
    std::optional<std::int64_t> messageGroupId;
    std::string messageContent;
    std::int64_t messageCreatedAt;
};

class Database {
private:
    sqlite3* db;
    bool connectionUsable;
    // Public operations hold this mutex for their complete SQLite interaction.
    std::mutex databaseMutex;

    // Used only during construction, before the Database can be shared.
    void executeQuery(const std::string& query);
    void initializeSchema();
    void ensureUsableCallerLocked() const;
    std::optional<std::int64_t> findUserIdCallerLocked(
        const std::string& trustedUsername);
    bool groupExistsCallerLocked(std::int64_t groupId);
    bool isGroupMemberCallerLocked(
        std::int64_t groupId,
        std::int64_t userId);
    std::vector<std::int64_t> groupRecipientIdsCallerLocked(
        std::int64_t groupId,
        std::int64_t excludedUserId);
    // The caller holds the Database mutex and an active write transaction.
    std::int64_t insertDurableMessageCallerLocked(
        DurableMessageKind kind,
        std::int64_t senderId,
        const std::optional<std::int64_t>& groupId,
        const std::string& content,
        const std::vector<std::int64_t>& recipientIds);

    std::string hashPassword(const std::string& password) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(password.c_str()), password.length(), hash);
        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

public:
    static constexpr std::size_t MaxPageSize = 1000;

    Database(const std::string& dbname);
    // Callers must finish using the object before destruction begins.
    ~Database() noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    RegistrationResult insertUser(const std::string& name, const std::string& username, const std::string& password);
    bool verifyLogin(const std::string& username, const std::string& password);
    void storeOfflineMessages(const std::string& sender, const std::string& receiver, const std::string& message);
    std::vector<Message> getOfflineMessages(const std::string& username);
    void clearOfflineMessages(const std::string& username);

    // recipientIds is a trusted snapshot that excludes the sender. A successful
    // call returns a stable committed ID, but durable IDs may contain gaps.
    std::int64_t insertDurableMessage(
        DurableMessageKind kind,
        std::int64_t senderId,
        const std::optional<std::int64_t>& groupId,
        const std::string& content,
        const std::vector<std::int64_t>& recipientIds);
    // afterMessageId is exclusive; zero is the initial cursor. Reading does not
    // acknowledge delivery.
    std::vector<StoredMessage> getPendingMessages(
        std::int64_t recipientId,
        std::int64_t afterMessageId,
        std::size_t pageLimit);
    // Repeated acknowledgement preserves the first Unix-epoch timestamp.
    AcknowledgementResult acknowledgeMessage(
        std::int64_t messageId,
        std::int64_t recipientId);
    // History is limited to messages sent by or snapshotted for the requester.
    std::vector<StoredMessage> getAuthorizedHistory(
        std::int64_t requesterId,
        std::int64_t afterMessageId,
        std::size_t pageLimit);

    // The username is supplied by trusted authenticated application code.
    GroupCreationResult createGroup(
        const std::string& trustedCreatorUsername,
        const std::string& name);
    GroupJoinResult joinGroup(
        const std::string& trustedUsername,
        std::int64_t groupId);
    GroupListResult listGroupsForUser(
        const std::string& trustedUsername,
        std::int64_t afterGroupId,
        std::size_t pageLimit);
    // This is a snapshot at this read. A later insertion requires a new
    // membership check and recipient selection in the same transaction.
    GroupRecipientSnapshot getGroupRecipientSnapshot(
        const std::string& trustedUsername,
        std::int64_t groupId);
};

#endif
