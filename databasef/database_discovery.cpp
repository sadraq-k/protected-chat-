#include "../database.h"
#include "database_internal.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using protected_chat::database_detail::Statement;
using protected_chat::database_detail::TransactionGuard;
using protected_chat::database_detail::bindInt64;
using protected_chat::database_detail::bindText;
using protected_chat::database_detail::databaseError;
using protected_chat::database_detail::finalizeSuccessfulStatement;
using protected_chat::database_detail::isAsciiWhitespace;
using protected_chat::database_detail::isValidUtf8;
using protected_chat::database_detail::prepareStatement;
using protected_chat::database_detail::readText;

namespace {

void validateDiscoveryUsername(const std::string& username) {
    constexpr std::size_t MaxUsernameBytes = 65'536;
    if (username.empty() || username.size() > MaxUsernameBytes ||
        username.find('\0') != std::string::npos || !isValidUtf8(username)) {
        throw std::invalid_argument("Invalid discovery username");
    }
}

void validateDiscoveryQuery(const std::string& query) {
    constexpr std::size_t MaxQueryBytes = 128;
    if (query.empty() || query.size() > MaxQueryBytes ||
        query.find('\0') != std::string::npos || !isValidUtf8(query) ||
        std::all_of(query.begin(), query.end(), [](unsigned char character) {
            return isAsciiWhitespace(character);
        })) {
        throw std::invalid_argument("Invalid discovery query");
    }
}

void validateDiscoveryPageArguments(
    std::int64_t cursor,
    std::size_t pageLimit) {
    constexpr std::size_t MaxDiscoveryPageSize = 100;
    if (cursor < 0) {
        throw std::invalid_argument("Discovery cursor cannot be negative");
    }
    if (pageLimit == 0 || pageLimit > MaxDiscoveryPageSize) {
        throw std::invalid_argument(
            "Discovery page limit must be between 1 and 100");
    }
}


} // namespace

UserSummary::UserSummary(std::int64_t id, std::string username)
    : userId(id), userName(std::move(username)) {}

std::int64_t UserSummary::id() const noexcept {
    return userId;
}

const std::string& UserSummary::username() const noexcept {
    return userName;
}

UserSearchEntry::UserSearchEntry(UserSummary user, bool isContact)
    : foundUser(std::move(user)), contact(isContact) {}

const UserSummary& UserSearchEntry::user() const noexcept {
    return foundUser;
}

bool UserSearchEntry::isContact() const noexcept {
    return contact;
}

GroupSearchEntry::GroupSearchEntry(GroupSummary group, bool isMember)
    : foundGroup(std::move(group)), member(isMember) {}

const GroupSummary& GroupSearchEntry::group() const noexcept {
    return foundGroup;
}

bool GroupSearchEntry::isMember() const noexcept {
    return member;
}

ContactAddResult::ContactAddResult(
    ContactAddStatus status,
    std::optional<UserSummary> contact)
    : addStatus(status), addedContact(std::move(contact)) {
    const bool requiresContact = status == ContactAddStatus::Added ||
        status == ContactAddStatus::AlreadyContact;
    if (requiresContact != addedContact.has_value()) {
        throw std::invalid_argument("Invalid contact-add result");
    }
}

ContactAddStatus ContactAddResult::status() const noexcept {
    return addStatus;
}

const std::optional<UserSummary>& ContactAddResult::contact() const noexcept {
    return addedContact;
}

ContactListResult::ContactListResult(
    DiscoveryReadStatus status,
    std::vector<UserSummary> contacts,
    bool hasMore)
    : readStatus(status),
      listedContacts(std::move(contacts)),
      moreContacts(hasMore) {
    if (status == DiscoveryReadStatus::ActorNotFound &&
        (!listedContacts.empty() || moreContacts)) {
        throw std::invalid_argument("Invalid missing-owner contact-list result");
    }
}

DiscoveryReadStatus ContactListResult::status() const noexcept {
    return readStatus;
}

const std::vector<UserSummary>& ContactListResult::contacts() const noexcept {
    return listedContacts;
}

bool ContactListResult::hasMore() const noexcept {
    return moreContacts;
}

UserSearchResult::UserSearchResult(
    DiscoveryReadStatus status,
    std::vector<UserSearchEntry> users,
    bool hasMore)
    : readStatus(status),
      foundUsers(std::move(users)),
      moreUsers(hasMore) {
    if (status == DiscoveryReadStatus::ActorNotFound &&
        (!foundUsers.empty() || moreUsers)) {
        throw std::invalid_argument("Invalid missing-actor user-search result");
    }
}

DiscoveryReadStatus UserSearchResult::status() const noexcept {
    return readStatus;
}

const std::vector<UserSearchEntry>& UserSearchResult::users() const noexcept {
    return foundUsers;
}

bool UserSearchResult::hasMore() const noexcept {
    return moreUsers;
}

GroupSearchResult::GroupSearchResult(
    DiscoveryReadStatus status,
    std::vector<GroupSearchEntry> groups,
    bool hasMore)
    : readStatus(status),
      foundGroups(std::move(groups)),
      moreGroups(hasMore) {
    if (status == DiscoveryReadStatus::ActorNotFound &&
        (!foundGroups.empty() || moreGroups)) {
        throw std::invalid_argument("Invalid missing-actor group-search result");
    }
}

DiscoveryReadStatus GroupSearchResult::status() const noexcept {
    return readStatus;
}

const std::vector<GroupSearchEntry>& GroupSearchResult::groups() const noexcept {
    return foundGroups;
}

bool GroupSearchResult::hasMore() const noexcept {
    return moreGroups;
}

ContactAddResult Database::addContact(
    const std::string& trustedOwnerUsername,
    const std::string& contactUsername) {
    validateDiscoveryUsername(trustedOwnerUsername);
    validateDiscoveryUsername(contactUsername);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin contact addition");
    try {
        const std::optional<std::int64_t> ownerId =
            findUserIdCallerLocked(trustedOwnerUsername);
        if (!ownerId) {
            transaction.commit("Commit missing contact owner lookup");
            return ContactAddResult(ContactAddStatus::OwnerNotFound);
        }

        Statement target = prepareStatement(
            db,
            "SELECT id, username FROM users WHERE username = ?;",
            "Prepare contact target lookup");
        bindText(db, target.get(), 1, contactUsername, "Bind contact username");
        const int targetResult = sqlite3_step(target.get());
        if (targetResult == SQLITE_DONE) {
            finalizeSuccessfulStatement(
                db, target, "Finalize missing contact target lookup");
            transaction.commit("Commit missing contact target lookup");
            return ContactAddResult(ContactAddStatus::ContactNotFound);
        }
        if (targetResult != SQLITE_ROW) {
            throw databaseError(db, "Read contact target", targetResult);
        }
        const std::int64_t contactId = sqlite3_column_int64(target.get(), 0);
        const std::string storedUsername = readText(target.get(), 1);
        if (sqlite3_step(target.get()) != SQLITE_DONE) {
            throw databaseError(
                db, "Complete contact target lookup", sqlite3_errcode(db));
        }
        finalizeSuccessfulStatement(db, target, "Finalize contact target lookup");

        if (contactId <= 0 || !isValidUtf8(storedUsername) ||
            storedUsername.find('\0') != std::string::npos) {
            throw std::runtime_error("Contact identity is not representable");
        }
        if (*ownerId == contactId) {
            transaction.commit("Commit self-contact rejection");
            return ContactAddResult(ContactAddStatus::SelfContactNotAllowed);
        }

        UserSummary contact(contactId, storedUsername);
        Statement existing = prepareStatement(
            db,
            "SELECT 1 FROM contacts "
            "WHERE owner_user_id = ? AND contact_user_id = ?;",
            "Prepare existing contact lookup");
        bindInt64(db, existing.get(), 1, *ownerId, "Bind contact owner ID");
        bindInt64(db, existing.get(), 2, contactId, "Bind contact target ID");
        const int existingResult = sqlite3_step(existing.get());
        if (existingResult != SQLITE_ROW && existingResult != SQLITE_DONE) {
            throw databaseError(db, "Read existing contact", existingResult);
        }
        const bool alreadyContact = existingResult == SQLITE_ROW;
        if (alreadyContact && sqlite3_step(existing.get()) != SQLITE_DONE) {
            throw databaseError(
                db, "Complete existing contact lookup", sqlite3_errcode(db));
        }
        finalizeSuccessfulStatement(db, existing, "Finalize existing contact lookup");
        if (alreadyContact) {
            ContactAddResult result(
                ContactAddStatus::AlreadyContact, std::move(contact));
            transaction.commit("Commit existing contact outcome");
            return result;
        }

        Statement insert = prepareStatement(
            db,
            "INSERT INTO contacts (owner_user_id, contact_user_id) "
            "VALUES (?, ?);",
            "Prepare contact insertion");
        bindInt64(db, insert.get(), 1, *ownerId, "Bind contact owner ID");
        bindInt64(db, insert.get(), 2, contactId, "Bind contact target ID");
        const int insertResult = sqlite3_step(insert.get());
        if (insertResult != SQLITE_DONE) {
            throw databaseError(db, "Insert contact", insertResult);
        }
        finalizeSuccessfulStatement(db, insert, "Finalize contact insertion");

        ContactAddResult result(ContactAddStatus::Added, std::move(contact));
        transaction.commit("Commit contact addition");
        return result;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; contact addition rollback failed: " + rollbackError);
        }
        throw;
    }
}

ContactListResult Database::listContacts(
    const std::string& trustedOwnerUsername,
    std::int64_t afterUserId,
    std::size_t pageLimit) {
    validateDiscoveryUsername(trustedOwnerUsername);
    validateDiscoveryPageArguments(afterUserId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin contact listing", false);
    try {
        const std::optional<std::int64_t> ownerId =
            findUserIdCallerLocked(trustedOwnerUsername);
        if (!ownerId) {
            transaction.commit("Commit missing contact-list owner lookup");
            return ContactListResult(
                DiscoveryReadStatus::ActorNotFound, {}, false);
        }

        Statement statement = prepareStatement(
            db,
            "SELECT u.id, u.username FROM contacts AS c "
            "JOIN users AS u ON u.id = c.contact_user_id "
            "WHERE c.owner_user_id = ? AND u.id > ? "
            "ORDER BY u.id LIMIT ?;",
            "Prepare contact listing");
        bindInt64(db, statement.get(), 1, *ownerId, "Bind contact-list owner ID");
        bindInt64(db, statement.get(), 2, afterUserId, "Bind contact-list cursor");
        bindInt64(
            db, statement.get(), 3,
            static_cast<std::int64_t>(pageLimit + 1),
            "Bind contact-list limit");

        std::vector<UserSummary> contacts;
        int stepResult = SQLITE_OK;
        while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
            contacts.emplace_back(
                sqlite3_column_int64(statement.get(), 0),
                readText(statement.get(), 1));
        }
        if (stepResult != SQLITE_DONE) {
            throw databaseError(db, "Read contact listing", stepResult);
        }
        finalizeSuccessfulStatement(db, statement, "Finalize contact listing");
        const bool hasMore = contacts.size() > pageLimit;
        if (hasMore) {
            contacts.pop_back();
        }
        transaction.commit("Commit contact listing");
        return ContactListResult(
            DiscoveryReadStatus::Ready, std::move(contacts), hasMore);
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; contact listing rollback failed: " + rollbackError);
        }
        throw;
    }
}

UserSearchResult Database::searchUsers(
    const std::string& trustedRequesterUsername,
    const std::string& query,
    std::int64_t afterUserId,
    std::size_t pageLimit) {
    validateDiscoveryUsername(trustedRequesterUsername);
    validateDiscoveryQuery(query);
    validateDiscoveryPageArguments(afterUserId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin user search", false);
    try {
        const std::optional<std::int64_t> requesterId =
            findUserIdCallerLocked(trustedRequesterUsername);
        if (!requesterId) {
            transaction.commit("Commit missing user-search actor lookup");
            return UserSearchResult(
                DiscoveryReadStatus::ActorNotFound, {}, false);
        }

        Statement statement = prepareStatement(
            db,
            "SELECT u.id, u.username, EXISTS("
                "SELECT 1 FROM contacts AS c "
                "WHERE c.owner_user_id = ? AND c.contact_user_id = u.id) "
            "FROM users AS u "
            "WHERE u.id <> ? AND u.id > ? "
            "AND instr(CAST(u.username AS BLOB), CAST(? AS BLOB)) > 0 "
            "ORDER BY u.id LIMIT ?;",
            "Prepare user search");
        bindInt64(db, statement.get(), 1, *requesterId, "Bind contact-flag owner ID");
        bindInt64(db, statement.get(), 2, *requesterId, "Bind excluded requester ID");
        bindInt64(db, statement.get(), 3, afterUserId, "Bind user-search cursor");
        bindText(db, statement.get(), 4, query, "Bind user-search query");
        bindInt64(
            db, statement.get(), 5,
            static_cast<std::int64_t>(pageLimit + 1),
            "Bind user-search limit");

        std::vector<UserSearchEntry> users;
        int stepResult = SQLITE_OK;
        while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
            users.emplace_back(
                UserSummary(
                    sqlite3_column_int64(statement.get(), 0),
                    readText(statement.get(), 1)),
                sqlite3_column_int(statement.get(), 2) != 0);
        }
        if (stepResult != SQLITE_DONE) {
            throw databaseError(db, "Read user search", stepResult);
        }
        finalizeSuccessfulStatement(db, statement, "Finalize user search");
        const bool hasMore = users.size() > pageLimit;
        if (hasMore) {
            users.pop_back();
        }
        transaction.commit("Commit user search");
        return UserSearchResult(
            DiscoveryReadStatus::Ready, std::move(users), hasMore);
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; user search rollback failed: " + rollbackError);
        }
        throw;
    }
}

GroupSearchResult Database::searchGroups(
    const std::string& trustedRequesterUsername,
    const std::string& query,
    std::int64_t afterGroupId,
    std::size_t pageLimit) {
    validateDiscoveryUsername(trustedRequesterUsername);
    validateDiscoveryQuery(query);
    validateDiscoveryPageArguments(afterGroupId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin group search", false);
    try {
        const std::optional<std::int64_t> requesterId =
            findUserIdCallerLocked(trustedRequesterUsername);
        if (!requesterId) {
            transaction.commit("Commit missing group-search actor lookup");
            return GroupSearchResult(
                DiscoveryReadStatus::ActorNotFound, {}, false);
        }

        Statement statement = prepareStatement(
            db,
            "SELECT g.id, g.name, EXISTS("
                "SELECT 1 FROM group_members AS m "
                "WHERE m.group_id = g.id AND m.user_id = ?) "
            "FROM chat_groups AS g "
            "WHERE g.id > ? "
            "AND instr(CAST(g.name AS BLOB), CAST(? AS BLOB)) > 0 "
            "ORDER BY g.id LIMIT ?;",
            "Prepare group search");
        bindInt64(db, statement.get(), 1, *requesterId, "Bind membership actor ID");
        bindInt64(db, statement.get(), 2, afterGroupId, "Bind group-search cursor");
        bindText(db, statement.get(), 3, query, "Bind group-search query");
        bindInt64(
            db, statement.get(), 4,
            static_cast<std::int64_t>(pageLimit + 1),
            "Bind group-search limit");

        std::vector<GroupSearchEntry> groups;
        int stepResult = SQLITE_OK;
        while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
            groups.emplace_back(
                GroupSummary(
                    sqlite3_column_int64(statement.get(), 0),
                    readText(statement.get(), 1)),
                sqlite3_column_int(statement.get(), 2) != 0);
        }
        if (stepResult != SQLITE_DONE) {
            throw databaseError(db, "Read group search", stepResult);
        }
        finalizeSuccessfulStatement(db, statement, "Finalize group search");
        const bool hasMore = groups.size() > pageLimit;
        if (hasMore) {
            groups.pop_back();
        }
        transaction.commit("Commit group search");
        return GroupSearchResult(
            DiscoveryReadStatus::Ready, std::move(groups), hasMore);
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; group search rollback failed: " + rollbackError);
        }
        throw;
    }
}
