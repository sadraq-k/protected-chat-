#include <iostream>
#include <boost/asio.hpp>
#include <thread>
#include <atomic>

using namespace std;
using namespace boost::asio;

atomic<bool> running(true);

void receive_messages(ip::tcp::socket& socket) {
    try {
        while (running) {
            string response(1024, '\0');
            size_t len = socket.read_some(boost::asio::buffer(response));
            response.resize(len);
            cout << "Received: " << response << endl;
        }
    } catch (const std::exception& e) {
        if (running) cerr << "Connection lost: " << e.what() << endl;
    }
}

void run_client(const std::string& host, const std::string& port) {
    io_context io;
    ip::tcp::socket socket(io);
    ip::tcp::resolver resolver(io);
    connect(socket, resolver.resolve(host, port));
    cout << "Connected to server :)\n";

    thread receive_thread(receive_messages, ref(socket));

    while (running) {
        string message;
        getline(cin, message);
        if (message.empty()) continue; // نادیده گرفتن پیام خالی

        try {
            boost::asio::write(socket, boost::asio::buffer(message));
            if (message == "the end") {
                cout << "Goodbye server!\n";
                running = false;
                socket.close();
                break;
            }
        } catch (const std::exception& e) {
            cerr << "Error: " << e.what() << endl;
            running = false;
            break;
        }
    }

    receive_thread.join();
}

int main() {
    run_client("185.79.158.34", "1403");
    return 0;
}

