#include "database.h"
#include <iostream>

Database::Database(const std::string& dbname) : db(nullptr) {
    std::cout << "[DB] Opening database: " << dbname << std::endl; // لاگ
    int rc = sqlite3_open(dbname.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Cannot open database: " << sqlite3_errmsg(db) << std::endl;
        throw std::runtime_error("Failed to open database");
    }
    std::cout << "[DB] Database opened successfully" << std::endl; // لاگ

    std::string sql = "CREATE TABLE IF NOT EXISTS users ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "name TEXT NOT NULL,"
                      "username TEXT NOT NULL UNIQUE,"
                      "pwd TEXT NOT NULL);";
    executeQuery(sql);
    sql = "CREATE TABLE IF NOT EXISTS messages ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "sender TEXT NOT NULL,"
          "receiver TEXT NOT NULL,"
          "message TEXT NOT NULL);";
    executeQuery(sql);
    std::cout << "[DB] Tables created successfully" << std::endl; // لاگ

    // چک کردن ستون‌های جدول users
    sql = "PRAGMA table_info(users);";
    sqlite3_stmt* stmt;
    std::cout << "[DB] Checking users table columns: ";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::cout << reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) << " ";
        }
        std::cout << std::endl;
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "[ERROR] Failed to check users table: " << sqlite3_errmsg(db) << std::endl;
    }
}

Database::~Database() {
    if (db) {
        sqlite3_close(db);
        std::cout << "[DB] Database closed" << std::endl; // لاگ
    }
}

void Database::executeQuery(const std::string& query) {
    char* errMsg = nullptr;
    std::cout << "[DB] Executing query: " << query << std::endl; // لاگ
    int rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        throw std::runtime_error("SQL execution failed");
    }
    std::cout << "[DB] Query executed successfully" << std::endl; // لاگ
}

bool Database::insertUser(const std::string& name, const std::string& username, const std::string& password) {
    std::string hashedPassword = hashPassword(password);
    const char* sql = "INSERT INTO users (name, username, pwd) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to insert user: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    rc = sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(stmt, 3, hashedPassword.c_str(), -1, SQLITE_TRANSIENT);
    }

    if (rc != SQLITE_OK) {
        std::cerr << "[ERROR] Failed to insert user: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[ERROR] Failed to insert user: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    std::cout << "[DB] User inserted: " << username << std::endl; // لاگ
    return true;
}

bool Database::verifyLogin(const std::string& username, const std::string& password) {
    std::string hashedPassword = hashPassword(password);
    std::string sql = "SELECT COUNT(*) FROM users WHERE username = '" + username +
                      "' AND pwd = '" + hashedPassword + "';";
    sqlite3_stmt* stmt;
    std::cout << "[DB] Verifying login for: " << username << std::endl; // لاگ
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] SQL prepare error: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    bool result = false;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
        result = true;
        std::cout << "[DB] Login verified: " << username << std::endl; // لاگ
    } else {
        std::cout << "[DB] Login failed: " << username << std::endl; // لاگ
    }
    sqlite3_finalize(stmt);
    return result;
}

void Database::storeOfflineMessages(const std::string& sender, const std::string& receiver, const std::string& message) {
    std::string sql = "INSERT INTO messages (sender, receiver, message) VALUES ('" +
                      sender + "', '" + receiver + "', '" + message + "');";
    executeQuery(sql);
    std::cout << "[DB] Stored offline message from " << sender << " to " << receiver << std::endl; // لاگ
}

std::vector<Message> Database::getOfflineMessages(const std::string& username) {
    std::vector<Message> messages;
    std::string sql = "SELECT sender, receiver, message FROM messages WHERE receiver = '" + username + "';";
    sqlite3_stmt* stmt;
    std::cout << "[DB] Fetching offline messages for: " << username << std::endl; // لاگ
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[ERROR] SQL prepare error: " << sqlite3_errmsg(db) << std::endl;
        return messages;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Message msg;
        msg.sender = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        msg.receiver = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        msg.message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        messages.push_back(msg);
    }
    sqlite3_finalize(stmt);
    std::cout << "[DB] Fetched " << messages.size() << " offline messages" << std::endl; // لاگ
    return messages;
}

void Database::clearOfflineMessages(const std::string& username) {
    std::string sql = "DELETE FROM messages WHERE receiver = '" + username + "';";
    executeQuery(sql);
    std::cout << "[DB] Cleared offline messages for: " << username << std::endl; // لاگ
}
