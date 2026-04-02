#include "database.h"
#include <iostream>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

Database::Database(const std::string& dbname) {
    if (sqlite3_open(dbname.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db) << std::endl;
        exit(1);
    }
    std::string sql = "CREATE TABLE IF NOT EXISTS Users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "username TEXT NOT NULL UNIQUE,"
        "display_name TEXT,"
        "hashed_password TEXT NOT NULL);";
    executeQuery(sql);
    sql = "CREATE TABLE IF NOT EXISTS Chats("
        "chat_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "type TEXT NOT NULL,"
        "title TEXT,"
        "created_at INTEGER NOT NULL);";
    executeQuery(sql);

    sql = "CREATE TABLE IF NOT EXISTS ChatMembers("
        "chat_id INTEGER NOT NULL,"
        "user_id INTEGER NOT NULL,"
        "role TEXT DEFAULT 'member',"
        "joined_at INTEGER NOT NULL,"
        "PRIMARY KEY(chat_id, user_id),"
        "FOREIGN KEY(chat_id) REFERENCES Chats(chat_id),"
        "FOREIGN KEY(user_id) REFERENCES Users(user_id));";
    executeQuery(sql);
    sql = "CREATE TABLE IF NOT EXISTS Messages("
        "msg_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "chat_id INTEGER NOT NULL,"
        "sender_id INTEGER NOT NULL,"
        "body TEXT NOT NULL,"
        "sent_at INTEGER NOT NULL,"
        "FOREIGN KEY(chat_id) REFERENCES Chats(chat_id),"
        "FOREIGN KEY(sender_id) REFERENCES Users(user_id));";
    executeQuery(sql);
    sql = "CREATE TABLE IF NOT EXISTS SyncState("
        "user_id INTEGER NOT NULL,"
        "device_id TEXT NOT NULL,"
        "last_msg_id INTEGER DEFAULT 0,"
        "PRIMARY KEY(user_id, device_id),"
        "FOREIGN KEY(user_id) REFERENCES Users(user_id));";
    executeQuery(sql);
}

Database::~Database() {
    sqlite3_close(db);
}

void Database::executeQuery(const std::string& sql) {
    std::lock_guard<std::mutex> lock(db_mutex);
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
    }
}

std::string Database::hashPassword(const std::string& password) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(password.c_str()), password.size(), hash);
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

bool Database::userExists(const std::string& username) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "SELECT COUNT(*) FROM Users WHERE username = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return exists;
}

bool Database::insertUser(const std::string& name,
    const std::string& username, const std::string& display_name,
    const std::string& password) {

    std::lock_guard<std::mutex> lock(db_mutex);
    if (userExists(username)) return false;
    std::string hashedPassword = hashPassword(password);
    std::string sql = "INSERT INTO Users (name, username,display_name, hashed_password) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, hashedPassword.c_str(), -1, SQLITE_TRANSIENT);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool Database::validateLogin(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "SELECT hashed_password FROM Users WHERE username = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string storedPassword = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        return storedPassword == hashPassword(password);
    }
    sqlite3_finalize(stmt);
    return false;
}
/*
void Database::storeOfflineMessage(const std::string& sender, const std::string& receiver, const std::string& message) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "INSERT INTO messages (sender, receiver, message) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, receiver.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "Error storing offline message: " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
    }
}
*/
/*
std::vector<std::string> Database::getOfflineMessages(const std::string& receiver) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::vector<std::string> messages;
    std::string sql = "SELECT sender, message FROM messages WHERE receiver = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, receiver.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string sender = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::string message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            messages.push_back(sender + ": " + message);
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
    }
    return messages;
}
*/
/*
void Database::clearOfflineMessages(const std::string& receiver) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "DELETE FROM messages WHERE receiver = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, receiver.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "Error clearing offline messages: " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
    }
}
*/