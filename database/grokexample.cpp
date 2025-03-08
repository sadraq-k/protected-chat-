//
// Created by sadraq on 3/8/25.
//
#include <iostream>
#include <sqlite3.h>

using namespace std;

int main() {
    sqlite3* db;
    char* errMsg = 0;
    int rc;

    // باز کردن دیتابیس
    rc = sqlite3_open("informations.db", &db);
    if (rc) {
        cerr << "نمی‌تونم دیتابیس رو باز کنم: " << sqlite3_errmsg(db) << endl;
        return 1;
    }

    // ساخت یه جدول
    const char* sql = "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);";
    rc = sqlite3_exec(db, sql, 0, 0, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "خطا تو ساخت جدول: " << errMsg << endl;
        sqlite3_free(errMsg);
    } else {
        cout << "جدول با موفقیت ساخته شد!" << endl;
    }

    // وارد کردن داده
    sql = "INSERT INTO users (name, age) VALUES ('Ali', 30);";
    rc = sqlite3_exec(db, sql, 0, 0, &errMsg);
    if (rc != SQLITE_OK)
    {
        cerr << "خطا تو وارد کردن داده: " << errMsg << endl;
        sqlite3_free(errMsg);
    } else {
        cout << "داده با موفقیت وارد شد!" << endl;
    }

    // بستن دیتابیس
    sqlite3_close(db);
    return 0;
}