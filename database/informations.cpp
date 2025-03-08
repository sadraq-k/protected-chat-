//
// Created by sadraq on 3/8/25.
//
#include <iostream>
#include <sqlite3.h>



using namespace std;

int main()
{
    //creat a databace
    sqlite3* db;
    sqlite3_stmt* stmt;
    sqlite3_open("informations.db", &db);

}