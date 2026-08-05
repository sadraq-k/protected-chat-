#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#include <string>
#include <vector>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

struct Message {
    std::string sender;
    std::string receiver;
    std::string message;
};

class Database {
private:
    sqlite3* db;

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
    ~Database();

    bool insertUser(const std::string& name, const std::string& username, const std::string& password);
    bool verifyLogin(const std::string& username, const std::string& password);
    void storeOfflineMessages(const std::string& sender, const std::string& receiver, const std::string& message);
    std::vector<Message> getOfflineMessages(const std::string& username);
    void clearOfflineMessages(const std::string& username);
};

#endif
