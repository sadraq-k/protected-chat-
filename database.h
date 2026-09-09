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

enum class MessageAcceptanceStatus {
    Accepted,
    SenderNotFound,
    RecipientNotFound,
    SelfMessageNotAllowed,
    GroupNotFound,
    SenderNotMember
};

enum class MessagePageStatus {
    Ready,
    UserNotFound
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

enum class ContactAddStatus {
    Added,
    AlreadyContact,
    OwnerNotFound,
    ContactNotFound,
    SelfContactNotAllowed
};

enum class DiscoveryReadStatus {
    Ready,
    ActorNotFound
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

class UserSummary {
public:
    UserSummary(std::int64_t id, std::string username);

    std::int64_t id() const noexcept;
    const std::string& username() const noexcept;

private:
    std::int64_t userId;
    std::string userName;
};

class UserSearchEntry {
public:
    UserSearchEntry(UserSummary user, bool isContact);

    const UserSummary& user() const noexcept;
    bool isContact() const noexcept;

private:
    UserSummary foundUser;
    bool contact;
};

class GroupSearchEntry {
public:
    GroupSearchEntry(GroupSummary group, bool isMember);

    const GroupSummary& group() const noexcept;
    bool isMember() const noexcept;

private:
    GroupSummary foundGroup;
    bool member;
};

class ContactAddResult {
public:
    ContactAddResult(
        ContactAddStatus status,
        std::optional<UserSummary> contact = std::nullopt);

    ContactAddStatus status() const noexcept;
    const std::optional<UserSummary>& contact() const noexcept;

private:
    ContactAddStatus addStatus;
    std::optional<UserSummary> addedContact;
};

class ContactListResult {
public:
    ContactListResult(
        DiscoveryReadStatus status,
        std::vector<UserSummary> contacts,
        bool hasMore);

    DiscoveryReadStatus status() const noexcept;
    const std::vector<UserSummary>& contacts() const noexcept;
    bool hasMore() const noexcept;

private:
    DiscoveryReadStatus readStatus;
    std::vector<UserSummary> listedContacts;
    bool moreContacts;
};

class UserSearchResult {
public:
    UserSearchResult(
        DiscoveryReadStatus status,
        std::vector<UserSearchEntry> users,
        bool hasMore);

    DiscoveryReadStatus status() const noexcept;
    const std::vector<UserSearchEntry>& users() const noexcept;
    bool hasMore() const noexcept;

private:
    DiscoveryReadStatus readStatus;
    std::vector<UserSearchEntry> foundUsers;
    bool moreUsers;
};

class GroupSearchResult {
public:
    GroupSearchResult(
        DiscoveryReadStatus status,
        std::vector<GroupSearchEntry> groups,
        bool hasMore);

    DiscoveryReadStatus status() const noexcept;
    const std::vector<GroupSearchEntry>& groups() const noexcept;
    bool hasMore() const noexcept;

private:
    DiscoveryReadStatus readStatus;
    std::vector<GroupSearchEntry> foundGroups;
    bool moreGroups;
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

class DeliveryMessage {
public:
    DeliveryMessage(StoredMessage message, std::string senderUsername);

    const StoredMessage& message() const noexcept;
    const std::string& senderUsername() const noexcept;

private:
    StoredMessage storedMessage;
    std::string messageSenderUsername;
};

class RecipientRoute {
public:
    RecipientRoute(std::int64_t userId, std::string username);

    std::int64_t userId() const noexcept;
    const std::string& username() const noexcept;

private:
    std::int64_t recipientUserId;
    std::string recipientUsername;
};

class AcceptedMessage {
public:
    AcceptedMessage(
        DeliveryMessage message,
        std::vector<RecipientRoute> recipients);

    const DeliveryMessage& message() const noexcept;
    const std::vector<RecipientRoute>& recipients() const noexcept;

private:
    DeliveryMessage acceptedDeliveryMessage;
    std::vector<RecipientRoute> acceptedRecipients;
};

class MessageAcceptanceResult {
public:
    MessageAcceptanceResult(
        MessageAcceptanceStatus status,
        std::optional<AcceptedMessage> acceptedMessage = std::nullopt);

    MessageAcceptanceStatus status() const noexcept;
    const std::optional<AcceptedMessage>& acceptedMessage() const noexcept;

private:
    MessageAcceptanceStatus acceptanceStatus;
    std::optional<AcceptedMessage> committedMessage;
};

class MessagePageResult {
public:
    MessagePageResult(
        MessagePageStatus status,
        std::vector<DeliveryMessage> messages,
        std::int64_t throughMessageId,
        bool hasMore);

    MessagePageStatus status() const noexcept;
    const std::vector<DeliveryMessage>& messages() const noexcept;
    std::int64_t throughMessageId() const noexcept;
    bool hasMore() const noexcept;

private:
    MessagePageStatus pageStatus;
    std::vector<DeliveryMessage> pageMessages;
    std::int64_t pageThroughMessageId;
    bool moreMessages;
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
    AcceptedMessage buildAcceptedMessageCallerLocked(
        std::int64_t messageId,
        const std::string& senderUsername);
    AcknowledgementResult acknowledgeMessageCallerLocked(
        std::int64_t messageId,
        std::int64_t recipientId);

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

    MessageAcceptanceResult acceptPrivateMessage(
        const std::string& trustedSenderUsername,
        const std::string& recipientUsername,
        const std::string& content);
    MessageAcceptanceResult acceptGroupMessage(
        const std::string& trustedSenderUsername,
        std::int64_t groupId,
        const std::string& content);
    MessageAcceptanceResult acceptBroadcastMessage(
        const std::string& trustedSenderUsername,
        const std::string& content);
    MessagePageResult getPendingDeliveryPage(
        const std::string& trustedUsername,
        std::int64_t afterMessageId,
        const std::optional<std::int64_t>& throughMessageId,
        std::size_t pageLimit);
    MessagePageResult getHistoryDeliveryPage(
        const std::string& trustedUsername,
        std::int64_t afterMessageId,
        const std::optional<std::int64_t>& throughMessageId,
        std::size_t pageLimit);
    AcknowledgementResult acknowledgeDelivery(
        const std::string& trustedUsername,
        std::int64_t messageId);
    std::int64_t importLegacyMessages();

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

    ContactAddResult addContact(
        const std::string& trustedOwnerUsername,
        const std::string& contactUsername);
    ContactListResult listContacts(
        const std::string& trustedOwnerUsername,
        std::int64_t afterUserId,
        std::size_t pageLimit);
    UserSearchResult searchUsers(
        const std::string& trustedRequesterUsername,
        const std::string& query,
        std::int64_t afterUserId,
        std::size_t pageLimit);
    GroupSearchResult searchGroups(
        const std::string& trustedRequesterUsername,
        const std::string& query,
        std::int64_t afterGroupId,
        std::size_t pageLimit);
};

#endif
