//
// Created by sadraq on 3/8/25.
//
#include <iostream>
#include <sqlite3.h>
#include <sstream>

using namespace std;

int insert_data(sqlite3 *db ,string name,string username,string how_they_find_us);
void add_column(sqlite3* db);

int main()
{
    //creat a databace
    sqlite3* db;

    //we creat one of this and to rest of code we gona use this and
    //because this we gana findout it work well or not
    int return_code = 0;

    //I don't know what the fuck is this.
    sqlite3_stmt* stmt;

    //when we want to exe a query or some of that we put them in this string.
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
                "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

    return_code = sqlite3_exec(db,sql_query.c_str(),NULL,0,&errmsg);

    if (return_code != SQLITE_OK)
    {
        //this option for sqlite "sqlite3_errmsg(db)" detail of errors
        cerr<<"error for creat table -> det: "<<errmsg<<endl;
        sqlite3_free(errmsg);
    } else
        cout<<"table  succesfuly create"<<endl;


    /*string name = "sadra" , username = "sadraq" , how_they_find_us = "so hard";
    insert_data(db,name,username,how_they_find_us);
    */
    add_column(db);


    sqlite3_close(db);

}
int callback(void* data, int argc, char** argv, char** colNames)
{
    for (int i = 0; i < argc; i++) {
        cout << colNames[i] << ": " << (argv[i] ? argv[i] : "NULL") << " | ";
    }
    cout << endl;
    return 0;
}

int insert_data(sqlite3 *db ,string name,string username,string how_they_find_us)
{
    stringstream query;
    query<<"INSERT INTO users(name,username,how_they_find_us) VALUES('"<<name<<
    "' , '"<<username<<"' , '" <<how_they_find_us<<"');";

    int rc;
    char* errormessage = 0;
    string finalquery;

    finalquery = query.str();

    rc = sqlite3_exec(db,finalquery.c_str(),callback,0,&errormessage);
    if (rc != SQLITE_OK)
    {
        //this option for sqlite "sqlite3_errmsg(db)" detail of errors
        cerr<<"error for add data -> det: "<<errormessage<<endl;
        sqlite3_free(errormessage);
    } else
        cout<<"data  succesfuly add"<<endl;

    return 0;
}

void add_column(sqlite3* db)
{
    const char* sql = "ALTER TABLE users ADD COLUMN password TEXT ;";
    char* errmsg = nullptr;

    int rc = sqlite3_exec(db, sql, 0, 0, &errmsg);

    if (rc != SQLITE_OK) {
        cerr << "Error adding column: " << errmsg << endl;
        sqlite3_free(errmsg);
    } else {
        cout << "Column added successfully!" << endl;
    }
}






























