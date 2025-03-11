#include <iostream>
#include <sqlite3.h>
#include <string>
#include <sstream>
#include <vector>

using namespace std;

class databace {
private:
    sqlite3* db;

public:
    databace(string dbname) {
        int return_code;
        char* errmsg;

        return_code = sqlite3_open(dbname.c_str(), &db);

        if (return_code != SQLITE_OK) {
            cerr << "Error opening database: " << sqlite3_errmsg(db) << endl;
        } else {
            cout << "Database opened successfully!" << endl;
        }

        string sql_query = "CREATE TABLE IF NOT EXISTS users ("
                           "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                           "name TEXT NOT NULL,"
                           "username TEXT NOT NULL UNIQUE,"
                           "how_they_find_us TEXT NOT NULL,"
                           "password TEXT NOT NULL,"
                           "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

        return_code = sqlite3_exec(db, sql_query.c_str(), NULL, 0, &errmsg);
        if (return_code != SQLITE_OK) {
            cerr << "Error creating table: " << errmsg << endl;
            sqlite3_free(errmsg);
        } else {
            cout << "Table created successfully or already exists." << endl;
        }
    }

    bool userExists(const string& username) {
        const char* sql = "SELECT COUNT(*) FROM users WHERE username = ?;";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            cerr << "Error preparing statement: " << sqlite3_errmsg(db) << endl;
            return false;
        }

        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

        bool exists = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            exists = (count > 0);
        }

        sqlite3_finalize(stmt);
        return exists;
    }

    void insert_data(const string& name, const string& username, const string& how_they_find_us, const string& password) {
        stringstream query;
        query << "INSERT INTO users (name, username, how_they_find_us, password) VALUES ('"
              << name << "', '" << username << "', '" << how_they_find_us << "', '" << password << "');";

        int rc;
        char* errormessage = 0;
        string finalquery = query.str();

        rc = sqlite3_exec(db, finalquery.c_str(), NULL, 0, &errormessage);
        if (rc != SQLITE_OK) {
            cerr << "Error inserting data: " << errormessage << endl;
            sqlite3_free(errormessage);
        } else {
            cout << "User registered successfully!" << endl;
        }
    }
};

class user {
private:
    string name, username, how_find_us, password;
    databace* db;

public:
    user(databace* database, string test_name, string test_username, string test_how_find_us, string test_password) 
        : db(database), name(test_name), username(test_username), how_find_us(test_how_find_us), password(test_password) 
    {
        if (db->userExists(username)) {
            cout << "Error: Username '" << username << "' already exists! Choose another one." << endl;
            return;
        }

        db->insert_data(name, username, how_find_us, password);
    }
};

void runTests(databace& db) {
    struct TestCase {
        string name;
        string username;
        string how_find_us;
        string password;
    };

    vector<TestCase> testCases = {
        {"Alice", "alice123", "Google", "pass123"},
        {"Bob", "bob456", "Friend", "securepass"},
        {"Charlie", "charlie789", "Advertisement", "mypassword"},
        {"David", "alice123", "Website", "newpass"}, // تکراری: باید ارور دهد
        {"Eve", "eve999", "Social Media", "evesecret"},
        {"Frank", "bob456", "YouTube", "tryagain"}   // تکراری: باید ارور دهد
    };

    for (const auto& test : testCases) {
        cout << "\nTesting user creation: " << test.username << endl;
        user newUser(&db, test.name, test.username, test.how_find_us, test.password);
    }
}

int main() {
    databace db("users.db");
    runTests(db);
    return 0;
}
