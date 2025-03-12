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
    databace(string dbname)
    {
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
        if (return_code != SQLITE_OK)
        {
            cerr << "Error creating table: " << errmsg << endl;
            sqlite3_free(errmsg);
        } else {
            cout << "Table created successfully or already exists." << endl;
        }
    }

    bool userExists(const string& username)
    {
        const char* sql = "SELECT COUNT(*) FROM users WHERE username = ?;";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            cerr << "Error preparing statement: " << sqlite3_errmsg(db) << endl;
            return false;
        }

        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

        bool exists = false;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
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
    int PK;
    databace* db;

public:


    user(databace* database)
    {

        int user_input;
        bool flag = 0;
        do{
            cout<<"do you want to sign in(for sign in press 1) or login(for login press 2):"<<endl;
            cin>>user_input;
            if (user_input == 1)
            {
                sign_in();
                flag = 0;
            }
            else if (user_input == 2)
            {
                login();
                flag = 0;
            } else{
                cout<<"rong data try again"<<endl;
                flag = 1;
            }
        } while (flag);

        db->insert_data(name, username, how_find_us, password);
    }
    int sign_in()
    {
        bool flag = 0;
        string cpass;
        do {
            cout << endl;
            cout << "enter your  name:";
            cin >> name;
            cout << endl;
            cout << "enter your username:";
            cin >> username;
            if (db->userExists(username))
            {
                cout << "Error: Username '" << username
                << "' already exists! Choose another one." << endl;
                flag = 1;
            } else
                flag = 0;
        } while (flag);

        cout<<endl;
        cout<<"how do you find us:";
        cin>>how_find_us;
        cout<<endl;

        do
        {
            cout<<"enter Your password:";
            cin>>cpass;
            cout<<"enter confirm password:";
            cin>>password;

            if (cpass == password)
            {
                flag = 0;
            } else
            {
                cout<<"there is no mach try again"<<endl;
                flag = 1;
            }
        } while (flag);

        cout<<"welcome "<<username<<" now you are in global chat."<<endl;


    }
    int login()
    {

    }
};




int main() {
    databace db("users.db");

    return 0;
}
