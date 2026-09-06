#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#include <iomanip>
#include <mutex>
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

class Database {
private:
    sqlite3* db;
    // Public operations hold this mutex for their complete SQLite interaction.
    std::mutex databaseMutex;

    // Used only during construction, before the Database can be shared.
    void executeQuery(const std::string& query);

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
};

#endif
