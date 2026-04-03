#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <sstream>
#include <vector>
#include <iostream>
#include <set>

#include <boost/asio/ssl.hpp>
#include <string>

#include "database.h"
#include "socketHelperFunctions.hpp"

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using json = nlohmann::json;
/*
std::mutex mtx;
std::unordered_map<std::string, std::shared_ptr<tcp::socket>> clients;
Database db("chat.db");
*/
//old code
/*
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
    }
    catch (const std::exception& e) {
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
    }
    catch (const json::parse_error& e) {
        std::cerr << "[ERROR] Invalid JSON: " << e.what() << std::endl;
        throw;
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Receive error: " << e.what() << std::endl;
        throw;
    }
}

void sendMessageToUser(const std::string& sender, const std::string& receiver, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = clients.find(receiver);
    json response = { {"type", "MESSAGE"}, {"sender", sender}, {"content", message} };
    if (it != clients.end() && it->second && it->second->is_open()) {
        std::cout << "[MESSAGE] Sending to " << receiver << std::endl; // لاگ
        sendResponse(it->second, response);
    }
    else {
        std::cout << "[MESSAGE] Storing offline message for " << receiver << std::endl; // لاگ
        db.storeOfflineMessage(sender, receiver, message); // اصلاح نام تابع
    }
}

void broadcastMessage(const std::string& sender, const std::string& message) {
    std::lock_guard<std::mutex> lock(mtx);
    json response = { {"type", "MESSAGE"}, {"sender", sender}, {"content", message} };
    for (auto it = clients.begin(); it != clients.end();) {
        if (it->first != sender) {
            if (it->second && it->second->is_open()) {
                sendResponse(it->second, response);
                ++it;
            }
            else {
                db.storeOfflineMessage(sender, it->first, message); // اصلاح نام تابع
                it = clients.erase(it);
            }
        }
        else {
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
                sendResponse(socket, { {"status", "FAIL"}, {"message", "All fields are required"} });
                std::cout << "[SIGN_IN] Failed: Missing fields" << std::endl; // لاگ
                return;
            }
            if (db.insertUser(name, username_temp, password)) {
                sendResponse(socket, { {"status", "SUCCESS"}, {"message", "Your ID: " + username_temp} });
                std::cout << "[SIGN_IN] Success: " << username_temp << std::endl; // لاگ
                username = username_temp;
            }
            else {
                sendResponse(socket, { {"status", "FAIL"}, {"message", "Username exists"} });
                std::cout << "[SIGN_IN] Failed: Username exists" << std::endl; // لاگ
                return;
            }
        }
        else if (action == "LOG_IN") {
            std::string username_temp = request.value("username", "");
            std::string password = request.value("password", "");
            std::cout << "[LOG_IN] Attempt: username=" << username_temp << std::endl; // لاگ
            if (username_temp.empty() || password.empty()) {
                sendResponse(socket, { {"status", "FAIL"}, {"message", "Username and password required"} });
                std::cout << "[LOG_IN] Failed: Missing fields" << std::endl; // لاگ
                return;
            }
            if (db.validateLogin(username_temp, password)) { // اصلاح نام تابع
                sendResponse(socket, { {"status", "SUCCESS"}, {"message", "Your ID: " + username_temp} });
                std::cout << "[LOG_IN] Success: " << username_temp << std::endl; // لاگ
                username = username_temp;
            }
            else {
                sendResponse(socket, { {"status", "FAIL"}, {"message", "Invalid credentials"} });
                std::cout << "[LOG_IN] Failed: Invalid credentials" << std::endl; // لاگ
                return;
            }
        }
        else {
            sendResponse(socket, { {"status", "FAIL"}, {"message", "Invalid action"} });
            std::cout << "[CLIENT] Invalid action: " << action << std::endl; // لاگ
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (clients.find(username) != clients.end()) {
                sendResponse(socket, { {"status", "FAIL"}, {"message", "User already logged in"} });
                std::cout << "[CLIENT] Failed: " << username << " already logged in" << std::endl; // لاگ
                return;
            }
            clients[username] = socket;
            std::cout << "[CLIENT] Registered client: " << username << std::endl; // لاگ
        }

        auto messages = db.getOfflineMessages(username);
        for (const auto& msg : messages) {
            json response = { {"type", "MESSAGE"}, {"sender", msg.se}, {"content", msg.message} };
            sendResponse(socket, response);
        }
        db.clearOfflineMessages(username);

        while (socket->is_open()) {
            json message = receiveData(*socket);
            std::string msgType = message.value("type", "");
            std::cout << "[CLIENT] Message type: " << msgType << std::endl; // لاگ
            if (msgType == "EXIT") {
                break;
            }
            else if (msgType == "PRIVATE") {
                std::string receiver = message.value("receiver", "");
                std::string content = message.value("content", "");
                if (!receiver.empty() && !content.empty()) {
                    sendMessageToUser(username, receiver, content);
                    sendResponse(socket, { {"status", "SUCCESS"}, {"message", "Private message sent"} });
                }
                else {
                    sendResponse(socket, { {"status", "FAIL"}, {"message", "Invalid private message format"} });
                }
            }
            else if (msgType == "GROUP") {
                std::vector<std::string> receivers;
                for (const auto& r : message.value("receivers", json::array())) {
                    receivers.push_back(r.get<std::string>());
                }
                std::string content = message.value("content", "");
                if (!receivers.empty() && !content.empty()) {
                    for (const auto& receiver : receivers) {
                        sendMessageToUser(username, receiver, content);
                    }
                    sendResponse(socket, { {"status", "SUCCESS"}, {"message", "Group message sent"} });
                }
                else {
                    sendResponse(socket, { {"status", "FAIL"}, {"message", "Invalid group message format"} });
                }
            }
            else {
                std::string content = message.value("content", "");
                if (!content.empty()) {
                    broadcastMessage(username, content);
                    sendResponse(socket, { {"status", "SUCCESS"}, {"message", "Broadcast message sent"} });
                }
                else {
                    sendResponse(socket, { {"status", "FAIL"}, {"message", "Invalid broadcast message format"} });
                }
            }
        }
    }
    catch (const std::exception& e) {
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
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Server error: " << e.what() << std::endl;
    }
}

int main() {
    try {
        runServer("192.168.57.10", 1403);
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Server error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

*/
/*
namespace asio = boost::asio;
using asio::ip::tcp;
using ssl_socket = asio::ssl::stream<tcp::socket>;
*/
// Helper to manage the SSL context for the server
class tls_server_context {
public:
    tls_server_context(boost::asio::ssl::context& ssl_ctx) : ssl_ctx_(ssl_ctx) {}

    // Load certificate and key
    void load_files(const std::string& cert_chain_file, const std::string& private_key_file) {
        ssl_ctx_.use_certificate_chain_file(cert_chain_file);
        ssl_ctx_.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

private:
    boost::asio::ssl::context& ssl_ctx_;
};

// Handles individual client connections
std::mutex clientSessionsMutex;
class clientSession;
std::set<std::shared_ptr<clientSession>> clientSessions;
void BroadcastMessege(std::string messege);
class clientSession : public std::enable_shared_from_this<clientSession> {
public:
    // The constructor now takes the actual tcp::socket to wrap
    clientSession(tcp::socket socket, boost::asio::ssl::context& ssl_ctx)
        : stream_(std::move(socket), ssl_ctx) {} // Initialize stream_ by moving the socket into it

    // Use this socket for the connection
    boost::asio::ssl::stream<tcp::socket>::next_layer_type& socket() {
        return stream_.next_layer();
    }
    bool operator<(const clientSession& other)
    {
        return (stream_.lowest_layer().local_endpoint() < other.stream_.lowest_layer().local_endpoint());
    }
    // Start the TLS handshake and then begin reading/writing
    void start() {
        boost::asio::co_spawn(
            stream_.get_executor(), // Use the executor from the stream
            [self = shared_from_this()]() -> boost::asio::awaitable<void> {
                try {
                    // Perform the TLS handshake
                    co_await self->stream_.async_handshake
                    (boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);
                    std::cout << "Client connected and TLS handshake successful." << std::endl;

                    // Now, start echoing data
                    co_await self->do_read_write();
                }
                catch (const boost::system::system_error& ec) {
                    if (ec.code() == boost::asio::error::eof ||
                        ec.code() == boost::asio::ssl::error::stream_truncated) {
                        std::cout << "Client disconnected gracefully." << std::endl;
                    }
                    else {
                        std::cerr << "TLS handshake or stream error: " << ec.what() << std::endl;
                    }
                }
                catch (const std::exception& e) {
                    std::cerr << "Error in session: " << e.what() << std::endl;
                }
                {
                    std::lock_guard<std::mutex> lock(clientSessionsMutex);
                    clientSessions.erase(self);
                }
            },
            boost::asio::detached // Detach the coroutine, it runs independently
        );
    }
    boost::asio::ssl::stream<tcp::socket> stream_; // The TLS-wrapped socket   
private:
    // Coroutine to read from the client and write back
    boost::asio::awaitable<void> do_read_write() {
        std::vector<char> message_buffer;
        for (;;) {
            // Read data from the client  
            std::string message;
            co_await readFromSocket(stream_, message_buffer, message);
            // Echo the data back to the client
            //co_await write_message(stream_, message);
            BroadcastMessege(message);
        }
    }


};
struct clientSessionSharedPtr
{
    bool operator()(std::shared_ptr<clientSession> const& x,
        std::shared_ptr<clientSession> const& y)
    {
        return *x < *y;
    }
};
void BroadcastMessege(std::string messege)
{
    std::set<std::shared_ptr<clientSession>> clientSessionsSnapshot;
    {
        std::lock_guard<std::mutex> lock(clientSessionsMutex);
        clientSessionsSnapshot = clientSessions;
    }
    for (auto session : clientSessionsSnapshot)
    {
        try
        {
            boost::asio::co_spawn(
                session->stream_.get_executor(), // Use the executor from the stream
                [self = session, messege]() -> boost::asio::awaitable<void> {
                    try {
                        co_await write_message(self->stream_, messege);
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Error in session: " << e.what() << std::endl;
                        {
                            std::lock_guard<std::mutex> lock(clientSessionsMutex);
                            clientSessions.erase(self);
                        }
                    }

                },
                boost::asio::detached // Detach the coroutine, it runs independently
            );
        }
        catch (const std::exception& e)
        {
            std::cerr << e.what() << std::endl;
        }
    }
}
// The main server class that accepts connections
class Server {
public:
    Server(boost::asio::io_context& io_context,
        boost::asio::ssl::context& ssl_ctx, short port)
        : acceptor_(io_context, boost::asio::ip::tcp::endpoint
        (boost::asio::ip::tcp::v4(), port)),
        ssl_ctx_(ssl_ctx) {
        // Configure the server's SSL context (certificate and key)
        tls_server_context server_ctx(ssl_ctx_);
        // IMPORTANT: Replace "server.pem" and "server.key" with your actual files
        // For testing, you can generate self-signed certificates.
        try {
            server_ctx.load_files("server.pem", "server.key");
            std::cout << "Server certificate and key loaded." << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to load server certificate/key: " << e.what() << std::endl;
            exit(1);
        }
        do_accept(); // Start accepting connections
    }
private:
    // Asynchronously accept new connections
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec,
                boost::asio::ip::tcp::socket socket) {
                    if (!ec) {
                        // Create a new session for the accepted client
                        std::shared_ptr<clientSession> session =
                            std::make_shared<clientSession>
                            (std::move(socket), ssl_ctx_);
                        {
                            std::lock_guard<std::mutex> lock(clientSessionsMutex);
                            clientSessions.insert(session);
                        }
                        session->start();
                    }
                    else {
                        std::cerr << "Accept error: " << ec.message() << std::endl;
                    }
                    // Continue accepting more connections
                    do_accept();
            });
    }

    boost::asio::ip::tcp::acceptor acceptor_;       // Listens for incoming connections
    boost::asio::ssl::context& ssl_ctx_;  // SSL context for the server
};

int main() {
    try {

        boost::asio::io_context io_context;

        // Create an SSL context for the server.
        // We use tlsv12 for better compatibility, but you might want tlsv13.
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12);

        // Configure SSL options: disable older, less secure protocols
        ssl_ctx.set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1);

        // IMPORTANT: Make sure server.pem and server.key are in the same directory
        // or provide the full path.
        Server server(io_context, ssl_ctx, 1403);
        //runServer("192.168.57.10", 1403)
        std::cout << "TLS Echo Server started " << std::endl;

        // Run the io_context to process asynchronous operations
        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 2; // Default to 2 threads if hardware_concurrency is not available

        // Launch threads that run the io_context
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() {
                io_context.run(); // Each thread runs the io_context event loop
                });
        }
        // Join all threads to keep the main thread alive
        for (auto& t : threads) {
            t.join();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Server exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}