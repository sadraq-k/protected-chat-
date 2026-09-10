#ifndef PROTECTED_CHAT_DATABASE_INTERNAL_H
#define PROTECTED_CHAT_DATABASE_INTERNAL_H

#include <sqlite3.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace protected_chat::database_detail {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

inline std::runtime_error databaseError(
    sqlite3* db,
    const std::string& operation,
    int resultCode) {
    const char* detail = db ? sqlite3_errmsg(db) : sqlite3_errstr(resultCode);
    return std::runtime_error(
        operation + " failed (SQLite " + std::to_string(resultCode) +
        "): " + detail);
}

inline void executeSql(
    sqlite3* db,
    const std::string& sql,
    const std::string& operation) {
    char* rawErrorMessage = nullptr;
    const int result =
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &rawErrorMessage);
    std::unique_ptr<char, decltype(&sqlite3_free)> errorMessage(
        rawErrorMessage, sqlite3_free);
    if (result != SQLITE_OK) {
        const std::string detail =
            errorMessage ? errorMessage.get() : sqlite3_errmsg(db);
        throw std::runtime_error(
            operation + " failed (SQLite " + std::to_string(result) +
            "): " + detail);
    }
}

inline Statement prepareStatement(
    sqlite3* db,
    const char* sql,
    const std::string& operation) {
    sqlite3_stmt* rawStatement = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &rawStatement, nullptr);
    Statement statement(rawStatement, sqlite3_finalize);
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
    return statement;
}

inline void bindText(
    sqlite3* db,
    sqlite3_stmt* statement,
    int index,
    const std::string& value,
    const std::string& operation) {
    if (value.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(operation + " text is too large");
    }
    const int result = sqlite3_bind_text(
        statement,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
}

inline void bindInt64(
    sqlite3* db,
    sqlite3_stmt* statement,
    int index,
    std::int64_t value,
    const std::string& operation) {
    const int result = sqlite3_bind_int64(statement, index, value);
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
}

inline void finalizeSuccessfulStatement(
    sqlite3* db,
    Statement& statement,
    const std::string& operation) {
    const int result = sqlite3_finalize(statement.release());
    if (result != SQLITE_OK) {
        throw databaseError(db, operation, result);
    }
}

inline std::string readText(sqlite3_stmt* statement, int column) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!text || bytes <= 0) {
        return std::string();
    }
    return std::string(
        reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

class TransactionGuard {
public:
    TransactionGuard(
        sqlite3* database,
        const std::string& operation,
        bool immediate = true)
        : db(database), active(false) {
        executeSql(db, immediate ? "BEGIN IMMEDIATE;" : "BEGIN;", operation);
        active = true;
    }

    ~TransactionGuard() noexcept {
        if (active) {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    void commit(const std::string& operation) {
        executeSql(db, "COMMIT;", operation);
        active = false;
    }

    std::string rollback() noexcept {
        if (!active) {
            return std::string();
        }

        char* rawErrorMessage = nullptr;
        const int result = sqlite3_exec(
            db, "ROLLBACK;", nullptr, nullptr, &rawErrorMessage);
        std::unique_ptr<char, decltype(&sqlite3_free)> errorMessage(
            rawErrorMessage, sqlite3_free);
        if (result == SQLITE_OK) {
            active = false;
            return std::string();
        }

        return errorMessage ? errorMessage.get() : sqlite3_errmsg(db);
    }

private:
    sqlite3* db;
    bool active;
};

inline bool recordExists(
    sqlite3* db,
    const char* sql,
    std::int64_t id,
    const std::string& operation) {
    Statement statement = prepareStatement(db, sql, operation);
    bindInt64(db, statement.get(), 1, id, "Bind " + operation);
    const int stepResult = sqlite3_step(statement.get());
    if (stepResult != SQLITE_ROW && stepResult != SQLITE_DONE) {
        throw databaseError(db, operation, stepResult);
    }
    const bool exists = stepResult == SQLITE_ROW;
    if (exists && sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw databaseError(db, operation, sqlite3_errcode(db));
    }
    finalizeSuccessfulStatement(db, statement, "Finalize " + operation);
    return exists;
}

inline bool isAsciiWhitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\f' || character == '\v';
}

inline bool isValidUtf8(const std::string& text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first =
            static_cast<unsigned char>(text[index]);
        std::size_t continuationCount = 0;
        std::uint32_t value = 0;
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            continuationCount = 1;
            value = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuationCount = 2;
            value = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuationCount = 3;
            value = first & 0x07;
        } else {
            return false;
        }
        if (index + continuationCount >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const unsigned char next =
                static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (next & 0x3f);
        }
        if ((continuationCount == 2 && value < 0x800) ||
            (continuationCount == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}


} // namespace protected_chat::database_detail

#endif
