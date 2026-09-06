#include "database.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::runtime_error databaseError(sqlite3* db, const std::string& operation, int resultCode) {
    const char* detail = db ? sqlite3_errmsg(db) : sqlite3_errstr(resultCode);
    return std::runtime_error(operation + " failed (SQLite " + std::to_string(resultCode) + "): " + detail);
}

Statement prepareStatement(sqlite3* db, const char* sql, const std::string& operation) {
    sqlite3_stmt* rawStatement = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &rawStatement, nullptr);
    Statement statement(rawStatement, sqlite3_finalize);
    if (rc != SQLITE_OK) {
        throw databaseError(db, operation, rc);
    }
    return statement;
}

void bindText(sqlite3* db,
              sqlite3_stmt* statement,
              int index,
              const std::string& value,
              const std::string& operation) {
    const int rc = sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        throw databaseError(db, operation, rc);
    }
}

void finalizeSuccessfulStatement(sqlite3* db, Statement& statement, const std::string& operation) {
    const int rc = sqlite3_finalize(statement.release());
    if (rc != SQLITE_OK) {
        throw databaseError(db, operation, rc);
    }
}

} // namespace

Database::Database(const std::string& dbname) : db(nullptr) {
    std::cout << "[DB] Opening database: " << dbname << std::endl;
    const int openResult = sqlite3_open(dbname.c_str(), &db);
    if (openResult != SQLITE_OK) {
        const std::runtime_error error = databaseError(db, "Open database", openResult);
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
        throw error;
    }

    try {
        const int extendedResult = sqlite3_extended_result_codes(db, 1);
        if (extendedResult != SQLITE_OK) {
            throw databaseError(db, "Enable extended SQLite result codes", extendedResult);
        }

        std::cout << "[DB] Database opened successfully" << std::endl;
        executeQuery("CREATE TABLE IF NOT EXISTS users ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "name TEXT NOT NULL,"
                     "username TEXT NOT NULL UNIQUE,"
                     "pwd TEXT NOT NULL);");
        executeQuery("CREATE TABLE IF NOT EXISTS messages ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "sender TEXT NOT NULL,"
                     "receiver TEXT NOT NULL,"
                     "message TEXT NOT NULL);");
        std::cout << "[DB] Tables created successfully" << std::endl;

        Statement statement = prepareStatement(db, "PRAGMA table_info(users);", "Inspect users table");
        std::cout << "[DB] Checking users table columns: ";
        int stepResult = SQLITE_OK;
        while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const unsigned char* columnName = sqlite3_column_text(statement.get(), 1);
            std::cout << (columnName ? reinterpret_cast<const char*>(columnName) : "") << " ";
        }
        if (stepResult != SQLITE_DONE) {
            throw databaseError(db, "Inspect users table", stepResult);
        }
        finalizeSuccessfulStatement(db, statement, "Finalize users table inspection");
        std::cout << std::endl;
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

    const int rc = sqlite3_close(db);
    if (rc == SQLITE_OK) {
        std::cout << "[DB] Database closed" << std::endl;
    } else {
        std::cerr << "[ERROR] Failed to close database: " << sqlite3_errstr(rc) << std::endl;
        sqlite3_close_v2(db);
    }
    db = nullptr;
}

void Database::executeQuery(const std::string& query) {
    char* rawErrorMessage = nullptr;
    std::cout << "[DB] Executing query: " << query << std::endl;
    const int rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &rawErrorMessage);
    std::unique_ptr<char, decltype(&sqlite3_free)> errorMessage(rawErrorMessage, sqlite3_free);
    if (rc != SQLITE_OK) {
        const std::string detail = errorMessage ? errorMessage.get() : sqlite3_errmsg(db);
        throw std::runtime_error("Execute schema query failed (SQLite " + std::to_string(rc) + "): " + detail);
    }
    std::cout << "[DB] Query executed successfully" << std::endl;
}

RegistrationResult Database::insertUser(const std::string& name,
                                        const std::string& username,
                                        const std::string& password) {
    const std::string hashedPassword = hashPassword(password);
    std::lock_guard<std::mutex> lock(databaseMutex);

    Statement statement = prepareStatement(
        db,
        "INSERT INTO users (name, username, pwd) VALUES (?, ?, ?);",
        "Prepare user registration");
    bindText(db, statement.get(), 1, name, "Bind registration name");
    bindText(db, statement.get(), 2, username, "Bind registration username");
    bindText(db, statement.get(), 3, hashedPassword, "Bind registration password hash");

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

bool Database::verifyLogin(const std::string& username, const std::string& password) {
    const std::string hashedPassword = hashPassword(password);
    std::lock_guard<std::mutex> lock(databaseMutex);
    std::cout << "[DB] Verifying login for: " << username << std::endl;

    Statement statement = prepareStatement(
        db,
        "SELECT COUNT(*) FROM users WHERE username = ? AND pwd = ?;",
        "Prepare login verification");
    bindText(db, statement.get(), 1, username, "Bind login username");
    bindText(db, statement.get(), 2, hashedPassword, "Bind login password hash");

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

    std::cout << (verified ? "[DB] Login verified: " : "[DB] Login failed: ") << username << std::endl;
    return verified;
}

void Database::storeOfflineMessages(const std::string& sender,
                                    const std::string& receiver,
                                    const std::string& message) {
    std::lock_guard<std::mutex> lock(databaseMutex);

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

    finalizeSuccessfulStatement(db, statement, "Finalize offline message storage");
    std::cout << "[DB] Stored offline message from " << sender << " to " << receiver << std::endl;
}

std::vector<Message> Database::getOfflineMessages(const std::string& username) {
    std::lock_guard<std::mutex> lock(databaseMutex);
    std::cout << "[DB] Fetching offline messages for: " << username << std::endl;

    Statement statement = prepareStatement(
        db,
        "SELECT sender, receiver, message FROM messages WHERE receiver = ?;",
        "Prepare offline message query");
    bindText(db, statement.get(), 1, username, "Bind offline message receiver");

    std::vector<Message> messages;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const unsigned char* sender = sqlite3_column_text(statement.get(), 0);
        const unsigned char* receiver = sqlite3_column_text(statement.get(), 1);
        const unsigned char* content = sqlite3_column_text(statement.get(), 2);
        messages.push_back({
            sender ? reinterpret_cast<const char*>(sender) : "",
            receiver ? reinterpret_cast<const char*>(receiver) : "",
            content ? reinterpret_cast<const char*>(content) : ""
        });
    }
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Read offline messages", stepResult);
    }

    finalizeSuccessfulStatement(db, statement, "Finalize offline message query");
    std::cout << "[DB] Fetched " << messages.size() << " offline messages" << std::endl;
    return messages;
}

void Database::clearOfflineMessages(const std::string& username) {
    std::lock_guard<std::mutex> lock(databaseMutex);

    Statement statement = prepareStatement(
        db,
        "DELETE FROM messages WHERE receiver = ?;",
        "Prepare offline message deletion");
    bindText(db, statement.get(), 1, username, "Bind offline message deletion receiver");

    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_DONE) {
        throw databaseError(db, "Clear offline messages", stepResult);
    }

    finalizeSuccessfulStatement(db, statement, "Finalize offline message deletion");
    std::cout << "[DB] Cleared offline messages for: " << username << std::endl;
}
