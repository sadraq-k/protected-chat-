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
