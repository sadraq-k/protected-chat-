# protected-chat-

## ساختار پروژه

```
protected-chat-/
├── out/
├── client
├── CMakeLists.txt
├── database.cpp
├── database.h
├── New Text Document.txt
├── README.md
├── server
├── server.cpp
└── user.cpp
```

**`database.cpp`**

```cpp
#include "database.h"
#include <iostream>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

Database::Database(const std::string& dbname) {
    if (sqlite3_open(dbname.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Error opening database: " << sqlite3_errmsg(db) << std::endl;
        exit(1);
    }
    std::string sql = "CREATE TABLE IF NOT EXISTS users ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "name TEXT NOT NULL,"
                      "username TEXT NOT NULL UNIQUE,"
                      "password TEXT NOT NULL);";
    executeQuery(sql);
    sql = "CREATE TABLE IF NOT EXISTS messages ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "sender TEXT NOT NULL,"
          "receiver TEXT NOT NULL,"
          "message TEXT NOT NULL);";
    executeQuery(sql);
}

Database::~Database() {
    sqlite3_close(db);
}

void Database::executeQuery(const std::string& sql) {
    std::lock_guard<std::mutex> lock(db_mutex);
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
    }
}

std::string Database::hashPassword(const std::string& password) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(password.c_str()), password.size(), hash);
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

bool Database::userExists(const std::string& username) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "SELECT COUNT(*) FROM users WHERE username = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return exists;
}

bool Database::insertUser(const std::string& name, const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(db_mutex);
    if (userExists(username)) return false;
    std::string hashedPassword = hashPassword(password);
    std::string sql = "INSERT INTO users (name, username, password) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hashedPassword.c_str(), -1, SQLITE_TRANSIENT);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

bool Database::validateLogin(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "SELECT password FROM users WHERE username = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string storedPassword = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        sqlite3_finalize(stmt);
        return storedPassword == hashPassword(password);
    }
    sqlite3_finalize(stmt);
    return false;
}

void Database::storeOfflineMessage(const std::string& sender, const std::string& receiver, const std::string& message) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "INSERT INTO messages (sender, receiver, message) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, receiver.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "Error storing offline message: " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
    }
}

std::vector<std::string> Database::getOfflineMessages(const std::string& receiver) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::vector<std::string> messages;
    std::string sql = "SELECT sender, message FROM messages WHERE receiver = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, receiver.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string sender = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            std::string message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            messages.push_back(sender + ": " + message);
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
    }
    return messages;
}

void Database::clearOfflineMessages(const std::string& receiver) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string sql = "DELETE FROM messages WHERE receiver = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, receiver.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "Error clearing offline messages: " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
    }
}

```

**`database.h`**

```c
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
    bool insertUser(const std::string& name, const std::string& username, const std::string& password);
    bool validateLogin(const std::string& username, const std::string& password);
    void storeOfflineMessage(const std::string& sender, const std::string& receiver, const std::string& message);
    std::vector<std::string> getOfflineMessages(const std::string& receiver);
    void clearOfflineMessages(const std::string& receiver);

private:
    void executeQuery(const std::string& sql);
    std::string hashPassword(const std::string& password);
};

#endif // DATABASE_H

```

**`README.md`**

````markdown
---

# Terminal-Based Messaging Network

## 1. Project Overview

This project is a terminal-based messaging network implemented in **C++** using **Boost.Asio** for network communication and **SQLite3** for storing user data and offline messages. The system supports public (Broadcast), private (Private), and group (Group) messaging. Additionally, offline messages are stored and delivered when the recipient logs back in.

---

## 2. Features

### 2.1 User Registration and Authentication
- **Registration (SIGN_IN):**
  - Users enter their details (name, username, password).
  - Passwords are hashed using the **SHA-256** algorithm before being stored.
  - The system checks for duplicate usernames before registration.

- **Login (LOG_IN):**
  - Users enter their username and password.
  - The hashed password is compared with the stored hash.
  - Upon successful authentication, the user is granted access.

### 2.2 Message Transmission

- **Public Messages (Broadcast):**
  - Messages sent without a prefix are considered public and broadcast to all online users (except the sender).
  - If a recipient is offline, the message is stored as an offline message.

- **Private Messages (Private):**
  - Format:  
    ```
    PRIVATE:username:message
    ```
  - The server processes the message:
    - If the recipient is online, the message is delivered instantly.
    - If offline, the message is stored in the database and sent upon login.

- **Group Messages (Group):**
  - Format:  
    ```
    GROUP:user1,user2:message
    ```
  - The server extracts the recipient list and sends messages accordingly, storing them for offline users.

### 2.3 Offline Message Management

- **Storing Offline Messages:**
  - If a recipient is offline, messages are stored in the database.
  
- **Retrieving Offline Messages:**
  - Upon user login, offline messages are retrieved and delivered.
  - After delivery, the messages are removed from the database.

### 2.4 Architecture and Concurrency

- **Server Side:**
  - Uses Boost.Asio to listen on a specified IP and port.
  - Each user connection is handled in a separate thread.
  - Online users are stored in an `unordered_map` with the username as the key and the corresponding socket as the value.

- **Concurrency Management:**
  - Mutex locks ensure thread-safe access to the database and user map to prevent race conditions.

---

## 3. Usage Scenarios

### 3.1 Public Messaging (Broadcast)
- **Online Users:**  
  - If **Ali** sends "Hello everyone," the server broadcasts this message to all online users (e.g., **Reza**, **Sara**, etc.) except **Ali**.
- **Offline Users:**  
  - If **Reza** is offline, the message "Hello everyone" is stored and sent when he logs in.

### 3.2 Private Messaging (Private)
- **Scenario:**  
  - **Ali** wants to send a private message to **Reza**.
  - **Ali** sends the message in the format:
    ```
    PRIVATE:reza:Hello Reza
    ```
  - The server processes the message:
    - If **Reza** is online, the message is delivered instantly.
    - If **Reza** is offline, the message is stored and sent upon login.

### 3.3 Group Messaging (Group)
- **Scenario:**  
  - **Ali** wants to send a message to **Reza** and **Sara**.
  - **Ali** sends:
    ```
    GROUP:reza,sara:Hello friends
    ```
  - The server extracts the recipient list and processes messages accordingly.

---

## 4. Installation and Execution

### 4.1 Prerequisites
- **C++ Compiler** (g++ or Clang with C++11 support)
- **Boost Libraries** (especially Boost.Asio)
- **SQLite3** for database management
- **OpenSSL** for SHA-256 hashing

### 4.2 Compiling the Project

To compile the server and database:
```bash
g++ -std=c++11 -pthread server.cpp database.cpp -o server -lboost_system -lssl -lcrypto -lsqlite3
```

To compile the client:
```bash
g++ -std=c++11 -pthread client.cpp -o client -lboost_system
```

### 4.3 Running the Project

1. **Run the Server:**
   ```bash
   ./server
   ```
   The server listens on a specified IP and port (e.g., 192.168.57.10:1403).

2. **Run the Client:**
   ```bash
   ./client
   ```
   The client connects to the server and prompts the user for login or registration.

3. **Sending Messages:**
   - Public messages: Enter text without a prefix.
   - Private messages:
     ```
     PRIVATE:username:message
     ```
   - Group messages:
     ```
     GROUP:user1,user2:message
     ```

---

## 5. Technical Notes and Future Enhancements

- **Password Security:**
  - Implement **bcrypt**, **PBKDF2**, or **Argon2** instead of SHA-256 for stronger password hashing.

- **Message Protocol:**
  - Using structured formats like **JSON** would enhance message processing and scalability.

- **Scalability Improvements:**
  - Switching from **thread-per-client** to **asynchronous models** in Boost.Asio for better performance at scale.

- **Error Logging:**
  - Use logging libraries like **spdlog** or **Boost.Log** for better debugging and monitoring.

- **Secure Communication:**
  - Implement **TLS/SSL** to encrypt client-server communication.

---

## 6. Conclusion

This project provides a functional terminal-based messaging system with:
- Secure user authentication using password hashing.
- Public, private, and group messaging capabilities.
- Offline message storage and retrieval.
- Efficient concurrency management using Boost.Asio and mutex locks.

This README comprehensively documents the project, covering features, use cases, and technical details. Future improvements can further enhance security, scalability, and performance, making it a fully functional chat system.

---


````

**`server.cpp`**

```cpp
#include <boost/asio.hpp>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <sstream>
#include <vector>
#include "database.h"
#include <iostream>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;

std::mutex mtx;
std::unordered_map<std::string, std::shared_ptr<tcp::socket>> clients;
Database db("chat.db");

void sendResponse(std::shared_ptr<tcp::socket> socket, const std::string& response) {
    if (socket && socket->is_open()) {
        boost::asio::write(*socket, boost::asio::buffer(response + "\n"));
    }
}

std::string receiveData(tcp::socket& socket) {
    boost::asio::streambuf buf;
    boost::asio::read_until(socket, buf, "\n");
    std::istream is(&buf);
    std::string line;
    std::getline(is, line);
    return line;
}

// تابع برای ارسال پیام به یک کاربر خاص
void sendMessageToUser(const std::string& sender, const std::string& receiver, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = clients.find(receiver);
    if (it != clients.end() && it->second && it->second->is_open()) {
        sendResponse(it->second, sender + ": " + message);
    } else {
        db.storeOfflineMessage(sender, receiver, message);
    }
}

// تابع برای پخش پیام به همه کاربران
void broadcastMessage(const std::string& sender, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto it = clients.begin(); it != clients.end();) {
        if (it->first != sender) {
            if (it->second && it->second->is_open()) {
                sendResponse(it->second, sender + ": " + message);
                ++it;
            } else {
                db.storeOfflineMessage(sender, it->first, message);
                it = clients.erase(it);
            }
        } else {
            ++it;
        }
    }
}

void handleClient(std::shared_ptr<tcp::socket> socket) {
    std::string username;
    try {
        std::string request = receiveData(*socket);
        std::istringstream iss(request);
        std::string token;
        std::getline(iss, token, ':');
        if (token == "SIGN_IN") {
            std::string name, username_temp, password;
            std::getline(iss, name, ':');
            std::getline(iss, username_temp, ':');
            std::getline(iss, password, ':');
            if (db.insertUser(name, username_temp, password)) {
                sendResponse(socket, "SUCCESS:Your ID: " + username_temp);
                username = username_temp;
            } else {
                sendResponse(socket, "FAIL:Username exists");
                socket->close();
                return;
            }
        } else if (token == "LOG_IN") {
            std::string username_temp, password;
            std::getline(iss, username_temp, ':');
            std::getline(iss, password, ':');
            if (db.validateLogin(username_temp, password)) {
                sendResponse(socket, "SUCCESS:Your ID: " + username_temp);
                username = username_temp;
            } else {
                sendResponse(socket, "FAIL:Invalid credentials");
                socket->close();
                return;
            }
        } else {
            sendResponse(socket, "FAIL:Invalid request");
            socket->close();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            clients[username] = socket;
        }

        auto messages = db.getOfflineMessages(username);
        for (const auto& msg : messages) {
            sendResponse(socket, msg);
        }
        db.clearOfflineMessages(username);

        while (true) {
            std::string message = receiveData(*socket);
            if (message == "the end") {
                break;
            }

            std::istringstream msgIss(message);
            std::string msgType;
            std::getline(msgIss, msgType, ':');

            if (msgType == "PRIVATE") {
                std::string receiver, msgContent;
                std::getline(msgIss, receiver, ':');
                std::getline(msgIss, msgContent);
                sendMessageToUser(username, receiver, msgContent);
            } else if (msgType == "GROUP") {
                std::string receivers, msgContent;
                std::getline(msgIss, receivers, ':');
                std::getline(msgIss, msgContent);
                std::istringstream receiversIss(receivers);
                std::string receiver;
                while (std::getline(receiversIss, receiver, ',')) {
                    sendMessageToUser(username, receiver, msgContent);
                }
            } else {
                broadcastMessage(username, message);
            }
            sendResponse(socket, "Message received");
        }
    } catch (const std::exception& e) {
        std::cerr << "Client " << username << " error: " << e.what() << std::endl;
    }
    std::lock_guard<std::mutex> lock(mtx);
    clients.erase(username);
    if (socket->is_open()) {
        socket->close();
    }
    std::cout << "Client " << username << " disconnected." << std::endl;
}

void runServer(const std::string& ip, int port) {
    io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(address::from_string(ip), port));
    std::cout << "Server running on " << ip << ":" << port << std::endl;

    while (true) {
        auto socket = std::make_shared<tcp::socket>(io);
        acceptor.accept(*socket);
        std::thread(handleClient, socket).detach();
    }
}

int main() {
    try {
        runServer("192.168.57.10", 1403);
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
    }
    return 0;
}

```

**`user.cpp`**

```cpp
#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <iostream>
#include <string>

using namespace boost::asio;
using namespace boost::asio::ip;

std::atomic<bool> running(true);

void receiveMessages(tcp::socket& socket) {
    try {
        while (running) {
            boost::asio::streambuf buf;
            boost::asio::read_until(socket, buf, "\n");
            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            if (line.empty()) continue;  // جلوگیری از پردازش پیام‌های خالی
            std::cout << "Received: " << line << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Connection lost: " << e.what() << std::endl;
        running = false;
        if (socket.is_open()) {
            socket.close();
        }
    }
}

void runClient(const std::string& host, const std::string& port) {
    io_context io;
    tcp::socket socket(io);
    tcp::resolver resolver(io);

    try {
        connect(socket, resolver.resolve(host, port));
        std::cout << "Connected to server :)\n";
    } catch (const std::exception& e) {
        std::cerr << "Error connecting to server: " << e.what() << std::endl;
        return;
    }

    std::string choice;
    std::cout << "Enter 1 for sign in, 2 for log in: ";
    std::cin >> choice;
    std::cin.ignore();

    std::string request;
    if (choice == "1") {
        std::string name, username, password;
        std::cout << "Enter name: ";
        std::getline(std::cin, name);
        std::cout << "Enter username: ";
        std::getline(std::cin, username);
        std::cout << "Enter password: ";
        std::getline(std::cin, password);
        
        if (name.empty() || username.empty() || password.empty()) {
            std::cerr << "All fields must be filled!\n";
            return;
        }

        request = "SIGN_IN:" + name + ":" + username + ":" + password;
    } else if (choice == "2") {
        std::string username, password;
        std::cout << "Enter username: ";
        std::getline(std::cin, username);
        std::cout << "Enter password: ";
        std::getline(std::cin, password);

        if (username.empty() || password.empty()) {
            std::cerr << "Username and password must not be empty!\n";
            return;
        }

        request = "LOG_IN:" + username + ":" + password;
    } else {
        std::cout << "Invalid choice\n";
        return;
    }

    try {
        boost::asio::write(socket, boost::asio::buffer(request + "\n"));
    } catch (const std::exception& e) {
        std::cerr << "Error sending data: " << e.what() << std::endl;
        return;
    }

    boost::asio::streambuf buf;
    try {
        boost::asio::read_until(socket, buf, "\n");
    } catch (const std::exception& e) {
        std::cerr << "Error receiving response: " << e.what() << std::endl;
        return;
    }

    std::istream is(&buf);
    std::string response;
    std::getline(is, response);

    if (response.find("SUCCESS") != 0) {
        std::cout << "Authentication failed: " << response << std::endl;
        socket.close();
        return;
    }
    std::cout << "Authentication successful! " << response.substr(8) << std::endl;

    std::thread receiveThread(receiveMessages, std::ref(socket));
    while (running) {
        std::string message;
        std::cout << "Enter message (or 'the end' to quit): ";
        std::getline(std::cin, message);

        if (message.empty()) continue;
        
        try {
            boost::asio::write(socket, boost::asio::buffer(message + "\n"));
        } catch (const std::exception& e) {
            std::cerr << "Error sending message: " << e.what() << std::endl;
            break;
        }

        if (message == "the end") {
            running = false;
            break;
        }
    }
    
    if (socket.is_open()) {
        socket.close();
    }
    receiveThread.join();
}

int main() {
    runClient("192.168.57.10", "1403");
    return 0;
}

```

## out/
