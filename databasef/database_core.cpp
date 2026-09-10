#include "../database.h"
#include "database_internal.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using protected_chat::database_detail::Statement;
using protected_chat::database_detail::TransactionGuard;
using protected_chat::database_detail::bindInt64;
using protected_chat::database_detail::bindText;
using protected_chat::database_detail::databaseError;
using protected_chat::database_detail::executeSql;
using protected_chat::database_detail::finalizeSuccessfulStatement;
using protected_chat::database_detail::prepareStatement;
using protected_chat::database_detail::readText;

namespace {

constexpr int CurrentSchemaVersion = 3;

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

constexpr const char* CreateContactsSql =
    "CREATE TABLE contacts ("
    "owner_user_id INTEGER NOT NULL "
        "REFERENCES users(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "contact_user_id INTEGER NOT NULL "
        "REFERENCES users(id) ON UPDATE RESTRICT ON DELETE RESTRICT,"
    "PRIMARY KEY (owner_user_id, contact_user_id),"
    "CHECK (owner_user_id <> contact_user_id));";

using ColumnDefinition = std::tuple<std::string, std::string, int, int>;
using ForeignKeyDefinition =
    std::tuple<std::string, std::string, std::string, std::string>;

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

void requireVersionTwoSchema(sqlite3* db) {
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

void requireCurrentSchema(sqlite3* db) {
    requireVersionTwoSchema(db);
    requireExactSql(db, "table", "contacts", CreateContactsSql);

    const std::vector<ColumnDefinition> expectedContactsColumns = {
        {"owner_user_id", "INTEGER", 1, 1},
        {"contact_user_id", "INTEGER", 1, 2}
    };
    if (readColumns(db, "contacts") != expectedContactsColumns) {
        throw std::runtime_error("Incompatible contact columns");
    }

    const std::vector<ForeignKeyDefinition> expectedContacts = {
        {"users", "contact_user_id", "RESTRICT", "RESTRICT"},
        {"users", "owner_user_id", "RESTRICT", "RESTRICT"}
    };
    if (readForeignKeys(db, "contacts") != expectedContacts) {
        throw std::runtime_error("Incompatible contact foreign keys");
    }
}


} // namespace

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
        } else if (version == 2) {
            requireVersionTwoSchema(db);
            if (readApplicationTables(db).count("contacts") != 0) {
                throw std::runtime_error(
                    "Schema version 2 contains an unexpected contacts table");
            }
        } else if (version != CurrentSchemaVersion) {
            throw std::runtime_error(
                "Unsupported database schema version: " +
                std::to_string(version));
        }

        if (version < 2) {
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

        if (version < CurrentSchemaVersion) {
            requireVersionTwoSchema(db);
            if (readApplicationTables(db).count("contacts") != 0) {
                throw std::runtime_error(
                    "Older schema contains an unexpected contacts table");
            }
            executeSql(db, CreateContactsSql, "Create contacts table");
            requireCurrentSchema(db);
            executeSql(
                db,
                "PRAGMA user_version = 3;",
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
