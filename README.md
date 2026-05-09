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

