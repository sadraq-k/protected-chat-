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
};

#endif
