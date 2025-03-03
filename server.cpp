#include <iostream>
#include <boost/asio.hpp>
#include <queue>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <random>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;

mutex mtx;
unordered_map<string, queue<string>> offline_messages;
unordered_map<string, shared_ptr<tcp::socket>> clients;

string generate_unique_id()
{
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(1000, 9999);
    return "client_" + to_string(dis(gen));
}

string receive_data(tcp::socket& socket)
{
    string buffer(1024, '\0');
    boost::system::error_code error;

    size_t len = socket.read_some(boost::asio::buffer(buffer), error);
    if (error == boost::asio::error::eof) throw runtime_error("Client disconnected");
    if (error) throw boost::system::system_error(error);

    buffer.resize(len);
    return buffer;
}
//shared_ptr<tcp::socket> its tell us which socket u must send.
void send_response(shared_ptr<tcp::socket> socket, const string& response)
{
    boost::system::error_code error;
    if (socket && socket->is_open())
    {
        boost::asio::write(*socket, boost::asio::buffer(response), error);
        if (error)
        {
            cerr << "Failed to send message: " << error.message() << endl;
        }
    }
}

void send_pending_messages(const string& client_id, shared_ptr<tcp::socket> socket)
{
    lock_guard<mutex> lock(mtx);
    auto& messages = offline_messages[client_id];
    while (!messages.empty() && socket && socket->is_open())
    {
        send_response(socket, messages.front());
        messages.pop();
    }
}

void broadcast_message(const string& sender_id, const string& message)
{
    lock_guard<mutex> lock(mtx);
    auto it = clients.begin();
    while (it != clients.end())
    {
        if (it->first != sender_id)
        {
            if (it->second && it->second->is_open())
            {
                send_response(it->second, sender_id + ": " + message);
                ++it;
            } else
            {
                offline_messages[it->first].push(sender_id + ": " + message);
                it = clients.erase(it); // حذف کلاینت غیرفعال
            }
        } else {
            ++it;
        }
    }
}

void handle_client(shared_ptr<tcp::socket> socket)
{
    string client_id;
    try {
        client_id = generate_unique_id();
        {
            lock_guard<mutex> lock(mtx);
            clients[client_id] = socket;
        }
        send_response(socket, "Your ID: " + client_id);
        send_pending_messages(client_id, socket);

        while (true) {
            string message = receive_data(*socket);
            cout << "Received from " << client_id << ": " << message << endl;

            if (message == "the end")
            {
                cout << "Client " << client_id << " disconnected.\n";
                break;
            }

            broadcast_message(client_id, message);
            send_response(socket, "Message received");
        }
    } catch (const exception& e)
    {
        cerr << "Client " << client_id << " error: " << e.what() << endl;
    }

    // حذف کلاینت از لیست بعد از قطع اتصال
    lock_guard<mutex> lock(mtx);
    clients.erase(client_id);
}

void run_server(const string& ip, int port)
{
    io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(ip::address::from_string(ip), port));
    cout << "Server running on " << ip << ":" << port << endl;

    while (true) {
        try {
            auto socket = make_shared<tcp::socket>(io);
            acceptor.accept(*socket);
            thread(handle_client, socket).detach();
        } catch (const exception& e)
        {
            cerr << "Accept error: " << e.what() << endl;
        }
    }
}

int main()
{
    try
    {
        run_server("192.168.57.10", 1403);
    } catch (const exception& e)
    {
        cerr << "Server error: " << e.what() << endl;
    }
    return 0;
}
