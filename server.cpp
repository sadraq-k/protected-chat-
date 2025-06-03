#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <sstream>
#include <vector>
#include <iostream>
#include "database.h"

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using json = nlohmann::json;

std::mutex mtx;
std::unordered_map<std::string, std::shared_ptr<tcp::socket>> clients;
Database db("chat.db");

void sendResponse(std::shared_ptr<tcp::socket> socket, const json& response) {
    if (!socket || !socket->is_open()) {
        std::cerr << "[ERROR] Socket is closed or invalid" << std::endl;
        return;
    }
    try {
        std::string data = response.dump() + "\n";
        std::cout << "[SEND] Sending response: " << data; // لاگ
        boost::asio::write(*socket, boost::asio::buffer(data));
        std::cout << "[SEND] Response sent successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to send response: " << e.what() << std::endl;
    }
}

json receiveData(tcp::socket& socket) {
    if (!socket.is_open()) {
        throw std::runtime_error("Socket is closed");
    }
    try {
        boost::asio::streambuf buf;
        boost::asio::read_until(socket, buf, "\n");
        std::istream is(&buf);
        std::string line;
        std::getline(is, line);
        std::cout << "[RECEIVE] Raw data: " << line << std::endl; // لاگ
        if (line.empty()) {
            throw std::runtime_error("Empty message received");
        }
        return json::parse(line);
    } catch (const json::parse_error& e) {
        std::cerr << "[ERROR] Invalid JSON: " << e.what() << std::endl;
        throw;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Receive error: " << e.what() << std::endl;
        throw;
    }
}

void sendMessageToUser(const std::string& sender, const std::string& receiver, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = clients.find(receiver);
    json response = {{"type", "MESSAGE"}, {"sender", sender}, {"content", message}};
    if (it != clients.end() && it->second && it->second->is_open()) {
        std::cout << "[MESSAGE] Sending to " << receiver << std::endl; // لاگ
        sendResponse(it->second, response);
    } else {
        std::cout << "[MESSAGE] Storing offline message for " << receiver << std::endl; // لاگ
        db.storeOfflineMessages(sender, receiver, message); // اصلاح نام تابع
    }
}

void broadcastMessage(const std::string& sender, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    json response = {{"type", "MESSAGE"}, {"sender", sender}, {"content", message}};
    for (auto it = clients.begin(); it != clients.end();) {
        if (it->first != sender) {
            if (it->second && it->second->is_open()) {
                sendResponse(it->second, response);
                ++it;
            } else {
                db.storeOfflineMessages(sender, it->first, message); // اصلاح نام تابع
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
        std::cout << "[CLIENT] New client connected" << std::endl; // لاگ
        json request = receiveData(*socket);
        std::cout << "[CLIENT] Request: " << request.dump() << std::endl; // لاگ
        std::string action = request.value("action", "");
        std::cout << "[CLIENT] Action: " << action << std::endl; // لاگ

        if (action == "SIGN_IN") {
            std::string name = request.value("name", "");
            std::string username_temp = request.value("username", "");
            std::string password = request.value("password", "");
            std::cout << "[SIGN_IN] Attempt: name=" << name << ", username=" << username_temp << std::endl; // لاگ
            if (name.empty() || username_temp.empty() || password.empty()) {
                sendResponse(socket, {{"status", "FAIL"}, {"message", "All fields are required"}});
                std::cout << "[SIGN_IN] Failed: Missing fields" << std::endl; // لاگ
                return;
            }
            if (db.insertUser(name, username_temp, password)) {
                sendResponse(socket, {{"status", "SUCCESS"}, {"message", "Your ID: " + username_temp}});
                std::cout << "[SIGN_IN] Success: " << username_temp << std::endl; // لاگ
                username = username_temp;
            } else {
                sendResponse(socket, {{"status", "FAIL"}, {"message", "Username exists"}});
                std::cout << "[SIGN_IN] Failed: Username exists" << std::endl; // لاگ
                return;
            }
        } else if (action == "LOG_IN") {
            std::string username_temp = request.value("username", "");
            std::string password = request.value("password", "");
            std::cout << "[LOG_IN] Attempt: username=" << username_temp << std::endl; // لاگ
            if (username_temp.empty() || password.empty()) {
                sendResponse(socket, {{"status", "FAIL"}, {"message", "Username and password required"}});
                std::cout << "[LOG_IN] Failed: Missing fields" << std::endl; // لاگ
                return;
            }
            if (db.verifyLogin(username_temp, password)) { // اصلاح نام تابع
                sendResponse(socket, {{"status", "SUCCESS"}, {"message", "Your ID: " + username_temp}});
                std::cout << "[LOG_IN] Success: " << username_temp << std::endl; // لاگ
                username = username_temp;
            } else {
                sendResponse(socket, {{"status", "FAIL"}, {"message", "Invalid credentials"}});
                std::cout << "[LOG_IN] Failed: Invalid credentials" << std::endl; // لاگ
                return;
            }
        } else {
            sendResponse(socket, {{"status", "FAIL"}, {"message", "Invalid action"}});
            std::cout << "[CLIENT] Invalid action: " << action << std::endl; // لاگ
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (clients.find(username) != clients.end()) {
                sendResponse(socket, {{"status", "FAIL"}, {"message", "User already logged in"}});
                std::cout << "[CLIENT] Failed: " << username << " already logged in" << std::endl; // لاگ
                return;
            }
            clients[username] = socket;
            std::cout << "[CLIENT] Registered client: " << username << std::endl; // لاگ
        }

        auto messages = db.getOfflineMessages(username);
        for (const auto& msg : messages) {
            json response = {{"type", "MESSAGE"}, {"sender", msg.sender}, {"content", msg.message}};
            sendResponse(socket, response);
        }
        db.clearOfflineMessages(username);

        while (socket->is_open()) {
            json message = receiveData(*socket);
            std::string msgType = message.value("type", "");
            std::cout << "[CLIENT] Message type: " << msgType << std::endl; // لاگ
            if (msgType == "EXIT") {
                break;
            } else if (msgType == "PRIVATE") {
                std::string receiver = message.value("receiver", "");
                std::string content = message.value("content", "");
                if (!receiver.empty() && !content.empty()) {
                    sendMessageToUser(username, receiver, content);
                    sendResponse(socket, {{"status", "SUCCESS"}, {"message", "Private message sent"}});
                } else {
                    sendResponse(socket, {{"status", "FAIL"}, {"message", "Invalid private message format"}});
                }
            } else if (msgType == "GROUP") {
                std::vector<std::string> receivers;
                for (const auto& r : message.value("receivers", json::array())) {
                    receivers.push_back(r.get<std::string>());
                }
                std::string content = message.value("content", "");
                if (!receivers.empty() && !content.empty()) {
                    for (const auto& receiver : receivers) {
                        sendMessageToUser(username, receiver, content);
                    }
                    sendResponse(socket, {{"status", "SUCCESS"}, {"message", "Group message sent"}});
                } else {
                    sendResponse(socket, {{"status", "FAIL"}, {"message", "Invalid group message format"}});
                }
            } else {
                std::string content = message.value("content", "");
                if (!content.empty()) {
                    broadcastMessage(username, content);
                    sendResponse(socket, {{"status", "SUCCESS"}, {"message", "Broadcast message sent"}});
                } else {
                    sendResponse(socket, {{"status", "FAIL"}, {"message", "Invalid broadcast message format"}});
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Client " << username << " error: " << e.what() << std::endl;
    }
    std::lock_guard<std::mutex> lock(mtx);
    clients.erase(username);
    if (socket->is_open()) {
        socket->close();
    }
    std::cout << "[CLIENT] Disconnected: " << username << std::endl;
}

void runServer(const std::string& ip, int port) {
    io_context io;
    try {
        tcp::acceptor acceptor(io, tcp::endpoint(address::from_string(ip), port));
        std::cout << "[Server] Running on " << ip << ":" << port << std::endl;
        while (true) {
            auto socket = std::make_shared<tcp::socket>(io);
            acceptor.accept(*socket);
            std::thread(handleClient, socket).detach();
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Server error: " << e.what() << std::endl;
    }
}

int main() {
    try {
        runServer("127.0.0.1", 1403);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Server error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
