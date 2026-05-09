#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#include <string>
#include <vector>
#include <mutex>

class Database {
private:
    sqlite3* db;
    std::mutex db_mutex;

public:
    Database(const std::string& dbname);
    ~Database();

    bool userExists(const std::string& username);
    bool insertUser(const std::string& name,
        const std::string& username, const std::string& display_name
        , const std::string& password);
    bool validateLogin(const std::string& username, const std::string& password);
    //void storeOfflineMessage(const std::string& sender,
     //   const std::string& receiver, const std::string& message);
    //std::vector<std::string> getOfflineMessages(const std::string& receiver);
    //void clearOfflineMessages(const std::string& receiver);

private:
    void executeQuery(const std::string& sql);
    std::string hashPassword(const std::string& password);
};

#endif // DATABASE_H
