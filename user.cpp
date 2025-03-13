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
    connect(socket, resolver.resolve(host, port));
    std::cout << "Connected to server :)\n";

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
        request = "SIGN_IN:" + name + ":" + username + ":" + password;
    } else if (choice == "2") {
        std::string username, password;
        std::cout << "Enter username: ";
        std::getline(std::cin, username);
        std::cout << "Enter password: ";
        std::getline(std::cin, password);
        request = "LOG_IN:" + username + ":" + password;
    } else {
        std::cout << "Invalid choice\n";
        return;
    }

    boost::asio::write(socket, boost::asio::buffer(request + "\n"));
    boost::asio::streambuf buf;
    boost::asio::read_until(socket, buf, "\n");
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
        std::cout << "Enter message (e.g., 'PRIVATE:username:message', 'GROUP:user1,user2:message', or 'message'): ";
        std::getline(std::cin, message);
        if (message.empty()) continue;
        boost::asio::write(socket, boost::asio::buffer(message + "\n"));
        if (message == "the end") {
            running = false;
            if (socket.is_open()) {
                socket.close();
            }
            break;
        }
    }
    receiveThread.join();
}

int main() {
    runClient("192.168.57.10", "1403");
    return 0;
}
