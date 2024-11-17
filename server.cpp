#include <iostream>
#include <boost/asio.hpp>
#include <queue>
#include <unordered_map>
#include <memory>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;

class ChatServer {
    io_context& io_;
    tcp::acceptor acceptor_;
    std::shared_ptr<tcp::socket> client1_;
    std::shared_ptr<tcp::socket> client2_;
    queue<string> messages_for_client1_;
    queue<string> messages_for_client2_;

    void async_read_from_client(std::shared_ptr<tcp::socket> client, queue<string>& message_queue, const string& client_name, std::shared_ptr<tcp::socket> target_client) {
        auto buffer = make_shared<std::vector<char>>(1024);
        client->async_read_some(boost::asio::buffer(*buffer), 
            [this, buffer, client, &message_queue, client_name, target_client](const boost::system::error_code& ec, size_t bytes_transferred) {
                if (!ec) {
                    string message(buffer->data(), bytes_transferred);
                    cout << client_name << ": " << message << endl;

                    if (target_client && target_client->is_open()) {
                        async_write_to_client(target_client, client_name + ": " + message);
                    } else {
                        message_queue.push(client_name + ": " + message);
                    }

                    async_read_from_client(client, message_queue, client_name, target_client);
                } else {
                    cout << client_name << " disconnected: " << ec.message() << endl;
                }
            });
    }

    void async_write_to_client(std::shared_ptr<tcp::socket> client, const string& message) {
        auto buffer = make_shared<string>(message);
        async_write(*client, boost::asio::buffer(*buffer), 
            [buffer](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    cerr << "Error sending message: " << ec.message() << endl;
                }
            });
    }

    void send_pending_messages(std::shared_ptr<tcp::socket> client, queue<string>& message_queue) {
        while (!message_queue.empty() && client->is_open()) {
            async_write_to_client(client, message_queue.front());
            message_queue.pop();
        }
    }

public:
    ChatServer(io_context& io, const string& ip, int port) 
        : io_(io), acceptor_(io, tcp::endpoint(boost::asio::ip::address::from_string(ip), port)) {}

    void run() {
        client1_ = make_shared<tcp::socket>(io_);
        client2_ = make_shared<tcp::socket>(io_);

        cout << "Waiting for Client 1 to connect..." << endl;
        acceptor_.async_accept(*client1_, [this](const boost::system::error_code& ec) {
            if (!ec) {
                cout << "Client 1 connected." << endl;
                async_read_from_client(client1_, messages_for_client1_, "Client 1", client2_);
                send_pending_messages(client1_, messages_for_client1_);
            } else {
                cerr << "Error accepting Client 1: " << ec.message() << endl;
            }
        });

        cout << "Waiting for Client 2 to connect..." << endl;
        acceptor_.async_accept(*client2_, [this](const boost::system::error_code& ec) {
            if (!ec) {
                cout << "Client 2 connected." << endl;
                async_read_from_client(client2_, messages_for_client2_, "Client 2", client1_);
                send_pending_messages(client2_, messages_for_client2_);
            } else {
                cerr << "Error accepting Client 2: " << ec.message() << endl;
            }
        });

        io_.run();
    }
};

int main() {
    try {
        io_context io;
        ChatServer server(io, "192.168.57.10", 1403);
        server.run();
    } catch (const std::exception& e) {
        cerr << "Server error: " << e.what() << endl;
    }
    return 0;
}
