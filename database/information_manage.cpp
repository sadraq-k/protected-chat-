//
// Created by sadraq on 3/11/25.
//
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <sstream>

using namespace std;

class databace;
bool userExists(const string& username);

class user{
private:
    string name,username,how_find_us,password;
    int PK;


    user(string ) {

        string cpass;
        string users = "users";
        databace* db;
        bool flag = 1;

        cout << endl;
        cout << "enter your  name:";
        cin >> name;
        cout << endl;
        cout << "enter your username:";
        cin >> username;

        if (db(users)->userExists(username))
        {

        }



            cout<<endl;
        cout<<"how do you find us:";
        cin>>how_find_us;
        cout<<endl;

        while (flag)
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
            }
        }


    }
};


class databace{
private:
    sqlite3* db;

public:
    databace(string dbname)
    {
        int return_code = 0;
        string sql_query ;
        char * errmsg;




        return_code = sqlite3_open("users.db", &db);

        //SQLITE_OK : it gona be findout it's ok or not.
        if (return_code != SQLITE_OK)
        {
            //this option for sqlite "sqlite3_errmsg(db)" detail of errors
            cerr<<"there is a error to open database -> det: "<<sqlite3_errmsg(db)<<endl;

        } else
            cout<<"databace succesfuly "<<endl;

        sql_query = "CREATE TABLE IF NOT EXISTS users "
                    "(id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "name TEXT NOT NULL,"
                    "username TEXT NOT NULL,"
                    "how_they_find_us TEXT NOT NULL,"
                    "password TEXT NOT NULL,"
                    "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

        return_code = sqlite3_exec(db,sql_query.c_str(),NULL,0,&errmsg);

        if (return_code != SQLITE_OK)
        {
            //this option for sqlite "sqlite3_errmsg(db)" detail of errors
            cerr<<"error for creat table -> det: "<<errmsg<<endl;
            sqlite3_free(errmsg);
        } else
            cout<<"table  succesfuly create OR HAS BEEN HERE"<<endl;

    }

    static int callback(void* data, int argc, char** argv, char** colNames)
    {
        for (int i = 0; i < argc; i++) {
            cout << colNames[i] << ": " << (argv[i] ? argv[i] : "NULL") << " | ";
        }
        cout << endl;
        return 0;
    }

    int insert_data(sqlite3 *db ,string name,string username,string how_they_find_us , string password)
    {
        stringstream query;
        query<<"INSERT INTO users(name,username,how_they_find_us,password) VALUES('"<<name<<
             "' , '"<<username<<"' , '" <<how_they_find_us<<"' , '"<<password<<"');";

        int rc;
        char* errormessage = 0;
        string finalquery;

        finalquery = query.str();

        rc = sqlite3_exec(db,finalquery.c_str(), callback ,0,&errormessage);
        if (rc != SQLITE_OK)
        {
            //this option for sqlite "sqlite3_errmsg(db)" detail of errors
            cerr<<"error for add data -> det: "<<errormessage<<endl;
            sqlite3_free(errormessage);
        } else
            cout<<"data  succesfuly add"<<endl;

        return 0;
    }


friend bool userExists(const string& username);

};

bool userExists(const string& username)
{
    //databace::db;
    const char* sql = "SELECT COUNT(*) FROM users WHERE username = ?;";
    sqlite3_stmt* stmt;

    // ۱. آماده‌سازی کوئری
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    // ۲. مقداردهی به پارامتر `?` در کوئری
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

    // ۳. اجرای کوئری و خواندن مقدار برگشتی
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int count = sqlite3_column_int(stmt, 0); // مقدار اولین ستون (تعداد ردیف‌های پیدا شده)
        exists = (count > 0); // اگه حداقل یک ردیف پیدا شد، یعنی کاربر وجود داره
    }

    // ۴. آزاد کردن منابع
    sqlite3_finalize(stmt);

    return exists;
}


int main()
{

}
