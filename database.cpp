#include "database.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr int CurrentSchemaVersion = 2;

constexpr const char* CreateUsersSql =
    "CREATE TABLE users ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL,"
    "username TEXT NOT NULL UNIQUE,"
    "pwd TEXT NOT NULL);";

constexpr const char* CreateLegacyMessagesSql =
    "CREATE TABLE messages ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "sender TEXT NOT NULL,"
    "receiver TEXT NOT NULL,"
    "message TEXT NOT NULL);";

constexpr const char* CreateChatGroupsSql =
    "CREATE TABLE chat_groups ("
    "id INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL UNIQUE CHECK(length(name) > 0));";

constexpr const char* CreateMessageHistorySql =
    "CREATE TABLE message_history ("
    "id INTEGER PRIMARY KEY,"
    "kind TEXT NOT NULL CHECK(kind IN ('PRIVATE', 'GROUP', 'BROADCAST')),"
    "sender_id INTEGER NOT NULL "
        "REFERENCES users(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "group_id INTEGER "
        "REFERENCES chat_groups(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "content TEXT NOT NULL,"
    "created_at INTEGER NOT NULL CHECK(typeof(created_at) = 'integer'),"
    "CHECK((kind = 'GROUP' AND group_id IS NOT NULL) OR "
          "(kind IN ('PRIVATE', 'BROADCAST') AND group_id IS NULL)));";

constexpr const char* CreateMessageRecipientsSql =
    "CREATE TABLE message_recipients ("
    "message_id INTEGER NOT NULL "
        "REFERENCES message_history(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "recipient_id INTEGER NOT NULL "
        "REFERENCES users(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "acknowledged_at INTEGER "
        "CHECK(acknowledged_at IS NULL OR "
              "typeof(acknowledged_at) = 'integer'),"
    "PRIMARY KEY(message_id, recipient_id));";

constexpr const char* CreateSenderIndexSql =
    "CREATE INDEX idx_message_history_sender "
    "ON message_history(sender_id, id);";

constexpr const char* CreateRecipientIndexSql =
    "CREATE INDEX idx_message_recipients_recipient "
    "ON message_recipients(recipient_id, message_id);";

constexpr const char* CreateGroupMembersSql =
    "CREATE TABLE group_members ("
    "group_id INTEGER NOT NULL "
        "REFERENCES chat_groups(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "user_id INTEGER NOT NULL "
        "REFERENCES users(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "PRIMARY KEY(group_id, user_id));";

constexpr const char* CreateGroupMembersUserIndexSql =
    "CREATE INDEX idx_group_members_user "
    "ON group_members(user_id, group_id);";

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
using ColumnDefinition = std::tuple<std::string, std::string, int, int>;
using ForeignKeyDefinition =
    std::tuple<std::string, std::string, std::string, std::string>;

std::runtime_error databaseError(
    sqlite3* db,
    const std::string& operation,
    int resultCode) {
    const char* detail = db ? sqlite3_errmsg(db) : sqlite3_errstr(resultCode);
    return std::runtime_error(
        operation + " failed (SQLite " + std::to_string(resultCode) +
        "): " + detail);
}

void executeSql(
    sqlite3* db,
    const std::string& sql,
    const std::string& operation) {
    char* rawErrorMessage = nullptr;
    const int result =
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &rawErrorMessage);
    std::unique_ptr<char, decltype(&sqlite3_free)> errorMessage(
        rawErrorMessage, sqlite3_free);
    if (result != SQLITE_OK) {
        const std::string detail =
            errorMessage ? errorMessage.get() : sqlite3_errmsg(db);
        throw std::runtime_error(
            operation + " failed (SQLite " + std::to_string(result) +
            "): " + detail);
    }
}

Statement prepareStatement(
    sqlite3* db,
    const char* sql,
    const std::string& operation) {
    sqlite3_stmt* rawStatement = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &rawStatement, nullptr);
    Statement statement(rawStatement, sqlite3_finalize);
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
    return statement;
}

void bindText(
    sqlite3* db,
    sqlite3_stmt* statement,
    int index,
    const std::string& value,
    const std::string& operation) {
    if (value.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(operation + " text is too large");
    }
    const int result = sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
}

void bindInt64(
    sqlite3* db,
    sqlite3_stmt* statement,
    int index,
    std::int64_t value,
    const std::string& operation) {
    const int result = sqlite3_bind_int64(statement, index, value);
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
}

void finalizeSuccessfulStatement(
    sqlite3* db,
    Statement& statement,
    const std::string& operation) {
    const int result = sqlite3_finalize(statement.release());
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
}

std::string readText(sqlite3_stmt* statement, int column) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!text || bytes <= 0) {
        return std::string();
    }
    return std::string(
        reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

class TransactionGuard {
public:
    TransactionGuard(
        sqlite3* database,
        const std::string& operation,
        bool immediate = true)
        : db(database), active(false) {
        executeSql(db, immediate ? "BEGIN IMMEDIATE;" : "BEGIN;", operation);
        active = true;
    }

    ~TransactionGuard() noexcept {
        if (active) {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    void commit(const std::string& operation) {
        executeSql(db, "COMMIT;", operation);
        active = false;
    }

    std::string rollback() noexcept {
        if (!active) {
            return std::string();
        }

        char* rawErrorMessage = nullptr;
        const int result = sqlite3_exec(
            db, "ROLLBACK;", nullptr, nullptr, &rawErrorMessage);
        std::unique_ptr<char, decltype(&sqlite3_free)> errorMessage(
            rawErrorMessage, sqlite3_free);
        if (result == SQLITE_OK) {
            active = false;
            return std::string();
        }

        return errorMessage ? errorMessage.get() : sqlite3_errmsg(db);
    }

private:
    sqlite3* db;
    bool active;
};

int readPragmaInteger(
    sqlite3* db,
    const char* sql,
    const std::string& operation) {
    Statement statement = prepareStatement(db, sql, operation);
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_ROW) {
        throw databaseError(db, operation, stepResult);
    }
    const int value = sqlite3_column_int(statement.get(), 0);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw databaseError(db, operation, sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, statement, "Finalize " + operation);
    return value;
}

std::set<std::string> readApplicationTables(sqlite3* db) {
    Statement statement = prepareStatement(
        db,
        "SELECT name FROM sqlite_master "
        "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name;",
        "Inspect application tables");
    std::set<std::string> tables;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        tables.insert(readText(statement.get(), 0));
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Inspect application tables", stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize table inspection");
    return tables;
}

std::string quoteIdentifier(const std::string& identifier) {
    std::string quoted = "\"";
    for (const char character : identifier) {
        if (character == '\"') {
            quoted.push_back('\"');
        }
        quoted.push_back(character);
    }
    quoted.push_back('\"');
    return quoted;
}

std::vector<ColumnDefinition> readColumns(
    sqlite3* db,
    const std::string& table) {
    const std::string sql = "PRAGMA table_info(" + quoteIdentifier(table) + ");";
    Statement statement = prepareStatement(db, sql.c_str(), "Inspect " + table);
    std::vector<ColumnDefinition> columns;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        columns.emplace_back(
            readText(statement.get(), 1),
            readText(statement.get(), 2),
            sqlite3_column_int(statement.get(), 3),
            sqlite3_column_int(statement.get(), 5));
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Inspect " + table, stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize " + table + " inspection");
    return columns;
}

bool hasUniqueIndexForColumn(
    sqlite3* db,
    const std::string& table,
    const std::string& expectedColumn) {
    const std::string listSql =
        "PRAGMA index_list(" + quoteIdentifier(table) + ");";
    Statement indexes =
        prepareStatement(db, listSql.c_str(), "Inspect indexes for " + table);
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(indexes.get())) == SQLITE_ROW) {
        if (sqlite3_column_int(indexes.get(), 2) == 0) {
            continue;
        }
        const std::string indexName = readText(indexes.get(), 1);
        const std::string infoSql =
            "PRAGMA index_info(" + quoteIdentifier(indexName) + ");";
        Statement columns = prepareStatement(
            db, infoSql.c_str(), "Inspect unique index " + indexName);
        std::vector<std::string> names;
        int infoResult = SQLITE_OK;
        while ((infoResult = sqlite3_step(columns.get())) == SQLITE_ROW) {
            names.push_back(readText(columns.get(), 2));
        }
        if (infoResult != SQLITE_DONE) {
            throw databaseError(db, "Inspect unique index " + indexName, infoResult);
        }
        finalizeSuccessfulStatement(
            db, columns, "Finalize unique index inspection");
        if (names == std::vector<std::string>{expectedColumn}) {
            finalizeSuccessfulStatement(
                db, indexes, "Finalize index-list inspection");
            return true;
        }
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Inspect indexes for " + table, stepResult);
    }
    finalizeSuccessfulStatement(db, indexes, "Finalize index-list inspection");
    return false;
}

std::string normalizeSql(const std::string& sql) {
    std::string normalized;
    normalized.reserve(sql.size());
    for (const unsigned char character : sql) {
        if (!std::isspace(character) && character != ';') {
            normalized.push_back(static_cast<char>(std::toupper(character)));
        }
    }
    return normalized;
}

std::string readSchemaSql(
    sqlite3* db,
    const std::string& type,
    const std::string& name) {
    Statement statement = prepareStatement(
        db,
        "SELECT sql FROM sqlite_master WHERE type = ? AND name = ?;",
        "Inspect schema object " + name);
    bindText(db, statement.get(), 1, type, "Bind schema object type");
    bindText(db, statement.get(), 2, name, "Bind schema object name");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_ROW) {
        throw std::runtime_error("Required schema object is missing: " + name);
    }
    const std::string sql = readText(statement.get(), 0);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw databaseError(db, "Inspect schema object " + name, sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, statement, "Finalize schema-object inspection");
    return sql;
}

void requireExactSql(
    sqlite3* db,
    const std::string& type,
    const std::string& name,
    const std::string& expectedSql) {
    if (normalizeSql(readSchemaSql(db, type, name)) != normalizeSql(expectedSql)) {
        throw std::runtime_error("Incompatible schema object: " + name);
    }
}

void requireLegacySchema(sqlite3* db) {
    const std::vector<ColumnDefinition> expectedUsers = {
        {"id", "INTEGER", 0, 1},
        {"name", "TEXT", 1, 0},
        {"username", "TEXT", 1, 0},
        {"pwd", "TEXT", 1, 0}
    };
    const std::vector<ColumnDefinition> expectedMessages = {
        {"id", "INTEGER", 0, 1},
        {"sender", "TEXT", 1, 0},
        {"receiver", "TEXT", 1, 0},
        {"message", "TEXT", 1, 0}
    };
    if (readColumns(db, "users") != expectedUsers ||
        !hasUniqueIndexForColumn(db, "users", "username") ||
        readColumns(db, "messages") != expectedMessages) {
        throw std::runtime_error("Incompatible legacy database schema");
    }
}

std::vector<ForeignKeyDefinition> readForeignKeys(
    sqlite3* db,
    const std::string& table) {
    const std::string sql =
        "PRAGMA foreign_key_list(" + quoteIdentifier(table) + ");";
    Statement statement =
        prepareStatement(db, sql.c_str(), "Inspect foreign keys for " + table);
    std::vector<ForeignKeyDefinition> foreignKeys;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        foreignKeys.emplace_back(
            readText(statement.get(), 2),
            readText(statement.get(), 3),
            readText(statement.get(), 5),
            readText(statement.get(), 6));
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Inspect foreign keys for " + table, stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize foreign-key inspection");
    std::sort(foreignKeys.begin(), foreignKeys.end());
    return foreignKeys;
}

void requireVersionOneSchema(sqlite3* db) {
    const std::set<std::string> tables = readApplicationTables(db);
    const std::set<std::string> required = {
        "chat_groups",
        "message_history",
        "message_recipients",
        "messages",
        "users"
    };
    if (!std::includes(
            tables.begin(), tables.end(), required.begin(), required.end())) {
        throw std::runtime_error("Current database schema is incomplete");
    }

    requireLegacySchema(db);
    requireExactSql(db, "table", "chat_groups", CreateChatGroupsSql);
    requireExactSql(db, "table", "message_history", CreateMessageHistorySql);
    requireExactSql(
        db, "table", "message_recipients", CreateMessageRecipientsSql);
    requireExactSql(
        db, "index", "idx_message_history_sender", CreateSenderIndexSql);
    requireExactSql(
        db,
        "index",
        "idx_message_recipients_recipient",
        CreateRecipientIndexSql);

    const std::vector<ForeignKeyDefinition> expectedHistory = {
        {"chat_groups", "group_id", "RESTRICT", "RESTRICT"},
        {"users", "sender_id", "RESTRICT", "RESTRICT"}
    };
    const std::vector<ForeignKeyDefinition> expectedRecipients = {
        {"message_history", "message_id", "RESTRICT", "RESTRICT"},
        {"users", "recipient_id", "RESTRICT", "RESTRICT"}
    };
    if (readForeignKeys(db, "message_history") != expectedHistory ||
        readForeignKeys(db, "message_recipients") != expectedRecipients) {
        throw std::runtime_error("Incompatible durable-history foreign keys");
    }
}

void requireCurrentSchema(sqlite3* db) {
    requireVersionOneSchema(db);
    requireExactSql(db, "table", "group_members", CreateGroupMembersSql);
    requireExactSql(
        db,
        "index",
        "idx_group_members_user",
        CreateGroupMembersUserIndexSql);

    const std::vector<ForeignKeyDefinition> expectedMemberships = {
        {"chat_groups", "group_id", "RESTRICT", "RESTRICT"},
        {"users", "user_id", "RESTRICT", "RESTRICT"}
    };
    if (readForeignKeys(db, "group_members") != expectedMemberships) {
        throw std::runtime_error("Incompatible group membership foreign keys");
    }
}

bool recordExists(
    sqlite3* db,
    const char* sql,
    std::int64_t id,
    const std::string& operation) {
    Statement statement = prepareStatement(db, sql, operation);
    bindInt64(db, statement.get(), 1, id, "Bind " + operation);
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_ROW && stepResult != SQLITE_DONE) {
        throw databaseError(db, operation, stepResult);
    }
    const bool exists = stepResult == SQLITE_ROW;
    if (exists && sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw databaseError(db, operation, sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, statement, "Finalize " + operation);
    return exists;
}

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

bool isAsciiWhitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\f' || character == '\v';
}

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

Database::Database(const std::string& dbname)
    : db(nullptr), connectionUsable(true) {
    std::cout << "[DB] Opening database: " << dbname << std::endl;
    const int openResult = sqlite3_open(dbname.c_str(), &db);
    if (openResult != SQLITE_OK) {
        const std::runtime_error error =
            databaseError(db, "Open database", openResult);
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
        throw error;
    }

    try {
        const int extendedResult = sqlite3_extended_result_codes(db, 1);
        if (extendedResult != SQLITE_OK) {
            throw databaseError(
                db, "Enable extended SQLite result codes", extendedResult);
        }

        const int busyTimeoutResult = sqlite3_busy_timeout(db, 2000);
        if (busyTimeoutResult != SQLITE_OK) {
            throw databaseError(
                db, "Configure SQLite busy timeout", busyTimeoutResult);
        }

        executeQuery("PRAGMA foreign_keys = ON;");
        if (readPragmaInteger(
                db, "PRAGMA foreign_keys;", "Verify foreign-key enforcement") != 1) {
            throw std::runtime_error("SQLite foreign-key enforcement is unavailable");
        }

        initializeSchema();
        std::cout << "[DB] Database opened successfully" << std::endl;
    } catch (...) {
        sqlite3_close(db);
        db = nullptr;
        throw;
    }
}

Database::~Database() noexcept {
    std::lock_guard<std::mutex> lock(databaseMutex);
    if (!db) {
        return;
    }

    const int result = sqlite3_close(db);
    if (result == SQLITE_OK) {
        std::cout << "[DB] Database closed" << std::endl;
    } else {
        std::cerr << "[ERROR] Failed to close database: "
                  << sqlite3_errstr(result) << std::endl;
        sqlite3_close_v2(db);
    }
    db = nullptr;
}

void Database::executeQuery(const std::string& query) {
    executeSql(db, query, "Execute schema query");
}

void Database::initializeSchema() {
    TransactionGuard transaction(db, "Begin schema initialization");
    try {
        const int version =
            readPragmaInteger(db, "PRAGMA user_version;", "Read schema version");
        if (version == 0) {
            const std::set<std::string> tables = readApplicationTables(db);
            if (tables.empty()) {
                executeSql(db, CreateUsersSql, "Create users table");
                executeSql(
                    db, CreateLegacyMessagesSql, "Create legacy messages table");
            } else if (tables == std::set<std::string>{"messages", "users"}) {
                requireLegacySchema(db);
            } else {
                throw std::runtime_error(
                    "Unrecognized version-0 database schema");
            }

            executeSql(db, CreateChatGroupsSql, "Create chat_groups table");
            executeSql(
                db, CreateMessageHistorySql, "Create message_history table");
            executeSql(
                db,
                CreateMessageRecipientsSql,
                "Create message_recipients table");
            executeSql(
                db, CreateSenderIndexSql, "Create sender-history index");
            executeSql(
                db, CreateRecipientIndexSql, "Create recipient-history index");
            requireVersionOneSchema(db);
        } else if (version == 1) {
            requireVersionOneSchema(db);
            if (readApplicationTables(db).count("group_members") != 0) {
                throw std::runtime_error(
                    "Schema version 1 contains an unexpected group_members table");
            }
        } else if (version != CurrentSchemaVersion) {
            throw std::runtime_error(
                "Unsupported database schema version: " +
                std::to_string(version));
        }

        if (version < CurrentSchemaVersion) {
            executeSql(
                db, CreateGroupMembersSql, "Create group_members table");
            executeSql(
                db,
                CreateGroupMembersUserIndexSql,
                "Create group-membership listing index");
            executeSql(
                db,
                "PRAGMA user_version = 2;",
                "Record schema version");
        }

        requireCurrentSchema(db);
        transaction.commit("Commit schema initialization");
    } catch (const std::exception& error) {
        const std::string rollbackError = transaction.rollback();
        if (!rollbackError.empty()) {
            throw std::runtime_error(
                std::string(error.what()) +
                "; schema rollback failed: " + rollbackError);
        }
        throw;
    }
}

void Database::ensureUsableCallerLocked() const {
    if (!connectionUsable || !db) {
        throw std::runtime_error("Database connection is not usable");
    }
}

std::optional<std::int64_t> Database::findUserIdCallerLocked(
    const std::string& trustedUsername) {
    Statement statement = prepareStatement(
        db,
        "SELECT id FROM users WHERE username = ?;",
        "Prepare user identity lookup");
    bindText(db, statement.get(), 1, trustedUsername, "Bind trusted username");
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult == SQLITE_DONE) {
        finalizeSuccessfulStatement(db, statement, "Finalize user identity lookup");
        return std::nullopt;
    }
    if (stepResult != SQLITE_ROW) {
        throw databaseError(db, "Read user identity", stepResult);
    }
    const std::int64_t userId = sqlite3_column_int64(statement.get(), 0);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw databaseError(db, "Complete user identity lookup", sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, statement, "Finalize user identity lookup");
    return userId;
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

RegistrationResult Database::insertUser(
    const std::string& name,
    const std::string& username,
    const std::string& password) {
    const std::string hashedPassword = hashPassword(password);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();

    Statement statement = prepareStatement(
        db,
        "INSERT INTO users (name, username, pwd) VALUES (?, ?, ?);",
        "Prepare user registration");
    bindText(db, statement.get(), 1, name, "Bind registration name");
    bindText(db, statement.get(), 2, username, "Bind registration username");
    bindText(
        db,
        statement.get(),
        3,
        hashedPassword,
        "Bind registration password hash");

    const int stepResult = sqlite3_step(statement.get());
    if (stepResult == SQLITE_CONSTRAINT_UNIQUE) {
        return RegistrationResult::DuplicateUsername;
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Insert user", stepResult);
    }

    finalizeSuccessfulStatement(db, statement, "Finalize user registration");
    std::cout << "[DB] User inserted: " << username << std::endl;
    return RegistrationResult::Created;
}

bool Database::verifyLogin(
    const std::string& username,
    const std::string& password) {
    const std::string hashedPassword = hashPassword(password);
    std::lock_guard<std::mutex> lock(databaseMutex);
    ensureUsableCallerLocked();
    std::cout << "[DB] Verifying login for: " << username << std::endl;

    Statement statement = prepareStatement(
        db,
        "SELECT COUNT(*) FROM users WHERE username = ? AND pwd = ?;",
        "Prepare login verification");
    bindText(db, statement.get(), 1, username, "Bind login username");
    bindText(
        db,
        statement.get(),
        2,
        hashedPassword,
        "Bind login password hash");

    int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_ROW) {
        throw databaseError(db, "Read login result", stepResult);
    }
    const bool verified = sqlite3_column_int(statement.get(), 0) > 0;

    stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Complete login query", stepResult);
    }
    finalizeSuccessfulStatement(db, statement, "Finalize login verification");

    std::cout << (verified ? "[DB] Login verified: " : "[DB] Login failed: ")
              << username << std::endl;
    return verified;
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
            transaction.commit("Commit missing acknowledgement lookup");
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
        finalizeSuccessfulStatement(
            db, lookup, "Finalize acknowledgement lookup");

        if (alreadyAcknowledged) {
            transaction.commit("Commit repeated acknowledgement lookup");
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
            throw std::runtime_error(
                "Acknowledgement state changed unexpectedly");
        }
        finalizeSuccessfulStatement(
            db, update, "Finalize message acknowledgement");
        transaction.commit("Commit message acknowledgement");
        return AcknowledgementResult::Acknowledged;
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
