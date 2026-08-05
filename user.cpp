#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <iostream>
#include <string>

using namespace boost::asio;
using namespace boost::asio::ip;
using json = nlohmann::json;

std::atomic<bool> running(true);

void receiveMessages(tcp::socket& socket) {
    try {
        while (running && socket.is_open()) {
            boost::asio::streambuf buf;
            boost::asio::read_until(socket, buf, "\n");
            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            std::cout << "[RECEIVE] Raw message: " << line << std::endl; // لاگ
            if (line.empty()) {
                std::cerr << "[ERROR] Empty message received" << std::endl;
                continue;
            }
            try {
                json message = json::parse(line);
                std::cout << "[RECEIVE] Parsed message: " << message.dump() << std::endl; // لاگ
                if (message.value("type", "") == "MESSAGE") {
                    std::cout << message.value("sender", "") << ": " << message.value("content", "") << std::endl;
                } else {
                    std::cout << "Server: " << message.value("message", "") << std::endl;
                }
            } catch (const json::parse_error& e) {
                std::cerr << "[ERROR] Invalid JSON: " << e.what() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Receive error: " << e.what() << std::endl;
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
        std::cout << "[CLIENT] Connecting to " << host << ":" << port << std::endl; // لاگ
        connect(socket, resolver.resolve(host, port));
        std::cout << "[CLIENT] Connected to server" << std::endl;
    } catch (const boost::system::system_error& e) {
        std::cerr << "[ERROR] Connection failed: " << e.what() << " (Code: " << e.code() << ")" << std::endl;
        return;
    }

    std::string choice;
    std::cout << "Enter 1 for sign in, 2 for log in: ";
    std::cin >> choice;
    std::cin.ignore();

    json request;
    if (choice == "1") {
        std::string name, username, password;
        std::cout << "Enter name: ";
        std::getline(std::cin, name);
        std::cout << "Enter username: ";
        std::getline(std::cin, username);
        std::cout << "Enter password: ";
        std::getline(std::cin, password);
        if (name.empty() || username.empty() || password.empty()) {
            std::cerr << "[ERROR] All fields must be filled" << std::endl;
            return;
        }
        request = {{"action", "SIGN_IN"}, {"name", name}, {"username", username}, {"password", password}};
    } else if (choice == "2") {
        std::string username, password;
        std::cout << "Enter username: ";
        std::getline(std::cin, username);
        std::cout << "Enter password: ";
        std::getline(std::cin, password);
        if (username.empty() || password.empty()) {
            std::cerr << "[ERROR] Username and password required" << std::endl;
            return;
        }
        request = {{"action", "LOG_IN"}, {"username", username}, {"password", password}};
    } else {
        std::cout << "[ERROR] Invalid choice" << std::endl;
        return;
    }

    try {
        std::cout << "[SEND] Sending request: " << request.dump() << std::endl; // لاگ
        boost::asio::write(socket, boost::asio::buffer(request.dump() + "\n"));
        std::cout << "[SEND] Request sent" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Send error: " << e.what() << std::endl;
        return;
    }

    boost::asio::streambuf buf;
    try {
        boost::asio::read_until(socket, buf, "\n");
        std::cout << "[RECEIVE] Response received" << std::endl; // لاگ
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Receive response error: " << e.what() << std::endl;
        return;
    }

    std::istream is(&buf);
    std::string response_str;
    std::getline(is, response_str);
    std::cout << "[RECEIVE] Raw response: " << response_str << std::endl; // لاگ
    json response;
    try {
        response = json::parse(response_str);
        std::cout << "[RECEIVE] Parsed response: " << response.dump() << std::endl; // لاگ
    } catch (const json::parse_error& e) {
        std::cerr << "[ERROR] Invalid response JSON: " << e.what() << std::endl;
        return;
    }

    if (response.value("status", "") != "SUCCESS") {
        std::cout << "Authentication failed: " << response.value("message", "") << std::endl;
        if (socket.is_open()) {
            socket.close();
        }
        return;
    }
    std::cout << "Authentication successful! " << response.value("message", "") << std::endl;

    std::thread receiveThread(receiveMessages, std::ref(socket));
    while (running && socket.is_open()) {
        std::string input;
        std::cout << "Enter message (or 'exit', 'PRIVATE:username:msg', 'GROUP:user1,user2:msg'): ";
        std::getline(std::cin, input);

        if (input.empty()) continue;

        json message;
        if (input == "exit") {
            message = {{"type", "EXIT"}};
            running = false;
        } else if (input.find("PRIVATE:") == 0) {
            std::istringstream iss(input.substr(8));
            std::string receiver, content;
            std::getline(iss, receiver, ':');
            std::getline(iss, content);
            if (!receiver.empty() && !content.empty()) {
                message = {{"type", "PRIVATE"}, {"receiver", receiver}, {"content", content}};
            } else {
                std::cout << "[ERROR] Invalid private message format" << std::endl;
                continue;
            }
        } else if (input.find("GROUP:") == 0) {
            std::istringstream iss(input.substr(6));
            std::string receivers, content;
            std::getline(iss, receivers, ':');
            std::getline(iss, content);
            std::vector<std::string> receiver_list;
            std::istringstream receiver_iss(receivers);
            std::string receiver;
            while (std::getline(receiver_iss, receiver, ',')) {
                receiver_list.push_back(receiver);
            }
            if (!receiver_list.empty() && !content.empty()) {
                message = {{"type", "GROUP"}, {"receivers", receiver_list}, {"content", content}};
            } else {
                std::cout << "[ERROR] Invalid group message format" << std::endl;
                continue;
            }
        } else {
            message = {{"type", "BROADCAST"}, {"content", input}};
        }

        try {
            boost::asio::write(socket, boost::asio::buffer(message.dump() + "\n"));
            std::cout << "[SEND] Message sent: " << message.dump() << std::endl; // لاگ
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Send message error: " << e.what() << std::endl;
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
    try {
        runClient("127.0.0.1", "1403");
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Client error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
