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
