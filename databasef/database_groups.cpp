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
using protected_chat::database_detail::prepareStatement;
using protected_chat::database_detail::readText;
using protected_chat::database_detail::recordExists;

namespace {

void validateGroupName(const std::string& name) {
    constexpr std::size_t MaxGroupNameBytes = 128;
    if (name.empty() || name.size() > MaxGroupNameBytes) {
        throw std::invalid_argument(
            "Group name must contain between 1 and 128 encoded bytes");
    }
    if (std::all_of(name.begin(), name.end(), [](unsigned char character) {
            return isAsciiWhitespace(character);
        })) {
        throw std::invalid_argument(
            "Group name cannot contain only ASCII whitespace");
    }
}

void validateGroupPageArguments(
    std::int64_t afterGroupId,
    std::size_t pageLimit) {
    if (afterGroupId < 0) {
        throw std::invalid_argument("Group cursor cannot be negative");
    }
    if (pageLimit == 0 || pageLimit > Database::MaxPageSize) {
        throw std::invalid_argument("Page limit must be between 1 and 1000");
    }
}


} // namespace

GroupSummary::GroupSummary(std::int64_t id, std::string name)
    : groupId(id), groupName(std::move(name)) {}

std::int64_t GroupSummary::id() const noexcept {
    return groupId;
}

const std::string& GroupSummary::name() const noexcept {
    return groupName;
}

GroupCreationResult::GroupCreationResult(
    GroupCreationStatus status,
    std::optional<GroupSummary> group)
    : creationStatus(status), createdGroup(std::move(group)) {}

GroupCreationStatus GroupCreationResult::status() const noexcept {
    return creationStatus;
}

const std::optional<GroupSummary>& GroupCreationResult::group() const noexcept {
    return createdGroup;
}

GroupListResult::GroupListResult(
    GroupListStatus status,
    std::vector<GroupSummary> groups,
    bool hasMore)
    : listStatus(status),
      listedGroups(std::move(groups)),
      moreGroups(hasMore) {}

GroupListStatus GroupListResult::status() const noexcept {
    return listStatus;
}

const std::vector<GroupSummary>& GroupListResult::groups() const noexcept {
    return listedGroups;
}

bool GroupListResult::hasMore() const noexcept {
    return moreGroups;
}

GroupRecipientSnapshot::GroupRecipientSnapshot(
    GroupRecipientSnapshotStatus status,
    std::vector<std::int64_t> recipientIds)
    : snapshotStatus(status),
      snapshotRecipientIds(std::move(recipientIds)) {}

GroupRecipientSnapshotStatus GroupRecipientSnapshot::status() const noexcept {
    return snapshotStatus;
}

const std::vector<std::int64_t>&
GroupRecipientSnapshot::recipientIds() const noexcept {
    return snapshotRecipientIds;
}


bool Database::groupExistsCallerLocked(std::int64_t groupId) {
    return recordExists(
        db,
        "SELECT 1 FROM chat_groups WHERE id = ?;",
        groupId,
        "Validate group identity");
}

bool Database::isGroupMemberCallerLocked(
    std::int64_t groupId,
    std::int64_t userId) {
    Statement statement = prepareStatement(
        db,
        "SELECT 1 FROM group_members WHERE group_id = ? AND user_id = ?;",
        "Prepare group membership lookup");
    bindInt64(db, statement.get(), 1, groupId, "Bind membership group ID");
    bindInt64(db, statement.get(), 2, userId, "Bind membership user ID");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_ROW && stepResult != SQLITE_DONE) {
        throw databaseError(db, "Read group membership", stepResult);
    }
    const bool member = stepResult == SQLITE_ROW;
    if (member && sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw databaseError(db, "Complete group membership lookup", sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, statement, "Finalize group membership lookup");
    return member;
}

std::vector<std::int64_t> Database::groupRecipientIdsCallerLocked(
    std::int64_t groupId,
    std::int64_t excludedUserId) {
    Statement statement = prepareStatement(
        db,
        "SELECT user_id FROM group_members "
        "WHERE group_id = ? AND user_id <> ? ORDER BY user_id;",
        "Prepare group recipient snapshot");
    bindInt64(db, statement.get(), 1, groupId, "Bind recipient group ID");
    bindInt64(db, statement.get(), 2, excludedUserId, "Bind excluded sender ID");

    std::vector<std::int64_t> recipientIds;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        recipientIds.push_back(sqlite3_column_int64(statement.get(), 0));
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Read group recipient snapshot", stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize group recipient snapshot");
    return recipientIds;
}

GroupCreationResult Database::createGroup(
    const std::string& trustedCreatorUsername,
    const std::string& name) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin group creation");
    try {
        if (trustedCreatorUsername.empty()) {
            throw std::invalid_argument("Creator username cannot be empty");
        }
        validateGroupName(name);

        const std::optional<std::int64_t> creatorId =
            findUserIdCallerLocked(trustedCreatorUsername);
        if (!creatorId) {
            transaction.commit("Commit missing group creator lookup");
            return GroupCreationResult(GroupCreationStatus::CreatorNotFound);
        }

        Statement group = prepareStatement(
            db,
            "INSERT INTO chat_groups (name) VALUES (?);",
            "Prepare group creation");
        bindText(db, group.get(), 1, name, "Bind group name");
        const int groupResult = sqlite3_step(group.get());
        if (groupResult == SQLITE_CONSTRAINT_UNIQUE) {
            group.reset();
            transaction.commit("Commit duplicate group-name outcome");
            return GroupCreationResult(GroupCreationStatus::DuplicateName);
        }
        if (groupResult != SQLITE_DONE) {
            throw databaseError(db, "Create group", groupResult);
        }
        finalizeSuccessfulStatement(db, group, "Finalize group creation");

        const std::int64_t groupId = sqlite3_last_insert_rowid(db);
        if (groupId <= 0) {
            throw std::runtime_error("SQLite returned an invalid group ID");
        }

        Statement membership = prepareStatement(
            db,
            "INSERT INTO group_members (group_id, user_id) VALUES (?, ?);",
            "Prepare creator membership");
        bindInt64(db, membership.get(), 1, groupId, "Bind created group ID");
        bindInt64(db, membership.get(), 2, *creatorId, "Bind group creator ID");
        const int membershipResult = sqlite3_step(membership.get());
        if (membershipResult != SQLITE_DONE) {
            throw databaseError(
                db, "Create group creator membership", membershipResult);
        }
        finalizeSuccessfulStatement(
            db, membership, "Finalize creator membership");

        transaction.commit("Commit group creation");
        return GroupCreationResult(
            GroupCreationStatus::Created,
            GroupSummary(groupId, name));
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; group creation rollback failed: " + rollbackError);
        }
        throw;
    }
}

GroupJoinResult Database::joinGroup(
    const std::string& trustedUsername,
    std::int64_t groupId) {
    if (trustedUsername.empty() || groupId <= 0) {
        throw std::invalid_argument(
            "Group join requires a username and positive group ID");
    }
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin group join");
    try {
        const std::optional<std::int64_t> userId =
            findUserIdCallerLocked(trustedUsername);
        if (!userId) {
            transaction.commit("Commit missing group-join user lookup");
            return GroupJoinResult::UserNotFound;
        }
        if (!groupExistsCallerLocked(groupId)) {
            transaction.commit("Commit missing group lookup");
            return GroupJoinResult::GroupNotFound;
        }

        Statement membership = prepareStatement(
            db,
            "INSERT INTO group_members (group_id, user_id) VALUES (?, ?);",
            "Prepare group join");
        bindInt64(db, membership.get(), 1, groupId, "Bind joined group ID");
        bindInt64(db, membership.get(), 2, *userId, "Bind joining user ID");
        const int membershipResult = sqlite3_step(membership.get());
        if (membershipResult == SQLITE_CONSTRAINT_PRIMARYKEY ||
            membershipResult == SQLITE_CONSTRAINT_UNIQUE) {
            membership.reset();
            transaction.commit("Commit existing group-membership outcome");
            return GroupJoinResult::AlreadyMember;
        }
        if (membershipResult != SQLITE_DONE) {
            throw databaseError(db, "Join group", membershipResult);
        }
        finalizeSuccessfulStatement(db, membership, "Finalize group join");
        transaction.commit("Commit group join");
        return GroupJoinResult::Joined;
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; group join rollback failed: " + rollbackError);
        }
        throw;
    }
}

GroupListResult Database::listGroupsForUser(
    const std::string& trustedUsername,
    std::int64_t afterGroupId,
    std::size_t pageLimit) {
    if (trustedUsername.empty()) {
        throw std::invalid_argument("Group-list username cannot be empty");
    }
    validateGroupPageArguments(afterGroupId, pageLimit);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin group listing", false);
    try {
        const std::optional<std::int64_t> userId =
            findUserIdCallerLocked(trustedUsername);
        if (!userId) {
            transaction.commit("Commit missing group-list user lookup");
            return GroupListResult(GroupListStatus::UserNotFound, {}, false);
        }

        Statement statement = prepareStatement(
            db,
            "SELECT g.id, g.name FROM group_members AS m "
            "JOIN chat_groups AS g ON g.id = m.group_id "
            "WHERE m.user_id = ? AND g.id > ? "
            "ORDER BY g.id LIMIT ?;",
            "Prepare group listing");
        bindInt64(db, statement.get(), 1, *userId, "Bind group-list user ID");
        bindInt64(db, statement.get(), 2, afterGroupId, "Bind group-list cursor");
        bindInt64(
            db,
            statement.get(),
            3,
            static_cast<std::int64_t>(pageLimit + 1),
            "Bind group-list page limit");

        std::vector<GroupSummary> groups;
        int stepResult = SQLITE_OK;
        while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
            groups.emplace_back(
                sqlite3_column_int64(statement.get(), 0),
                readText(statement.get(), 1));
        }
        if (stepResult != SQLITE_DONE) {
            throw databaseError(db, "Read group listing", stepResult);
        }
        finalizeSuccessfulStatement(db, statement, "Finalize group listing");

        const bool hasMore = groups.size() > pageLimit;
        if (hasMore) {
            groups.pop_back();
        }
        transaction.commit("Commit group listing");
        return GroupListResult(
            GroupListStatus::Listed, std::move(groups), hasMore);
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; group listing rollback failed: " + rollbackError);
        }
        throw;
    }
}

GroupRecipientSnapshot Database::getGroupRecipientSnapshot(
    const std::string& trustedUsername,
    std::int64_t groupId) {
    if (trustedUsername.empty() || groupId <= 0) {
        throw std::invalid_argument(
            "Group snapshot requires a username and positive group ID");
    }
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    TransactionGuard transaction(db, "Begin group recipient snapshot", false);
    try {
        const std::optional<std::int64_t> senderId =
            findUserIdCallerLocked(trustedUsername);
        if (!senderId) {
            transaction.commit("Commit missing snapshot-user lookup");
            return GroupRecipientSnapshot(
                GroupRecipientSnapshotStatus::UserNotFound, {});
        }
        if (!groupExistsCallerLocked(groupId)) {
            transaction.commit("Commit missing snapshot-group lookup");
            return GroupRecipientSnapshot(
                GroupRecipientSnapshotStatus::GroupNotFound, {});
        }
        if (!isGroupMemberCallerLocked(groupId, *senderId)) {
            transaction.commit("Commit nonmember snapshot lookup");
            return GroupRecipientSnapshot(
                GroupRecipientSnapshotStatus::SenderNotMember, {});
        }

        std::vector<std::int64_t> recipientIds =
            groupRecipientIdsCallerLocked(groupId, *senderId);
        transaction.commit("Commit group recipient snapshot");
        return GroupRecipientSnapshot(
            GroupRecipientSnapshotStatus::Ready,
            std::move(recipientIds));
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            connectionUsable = false;
            throw std::runtime_error(
                std::string(error.what()) +
                "; group snapshot rollback failed: " + rollbackError);
        }
        throw;
    }
}
