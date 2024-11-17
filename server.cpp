#include <iostream>
#include <boost/asio.hpp>
#include <queue>
#include <unordered_map>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;

// تابعی برای راه‌اندازی سرور
auto start_server = [](io_context& io, const std::string& ip, int port) -> tcp::acceptor {
    tcp::endpoint endpoint(boost::asio::ip::address::from_string(ip), port);
    tcp::acceptor acceptor(io, endpoint);
    acceptor.listen();
    return acceptor;
};

// تابعی برای دریافت داده از کلاینت
auto receive_data = [](tcp::socket& socket) -> std::string {
    std::string buffer(1024, '\0');
    std::size_t len = socket.read_some(boost::asio::buffer(&buffer[0], buffer.size()));
    buffer.resize(len);
    return buffer;
};

// تابعی برای ارسال پاسخ به کلاینت
auto send_response = [](tcp::socket& socket, const std::string& response) {
    boost::asio::write(socket, boost::asio::buffer(response));
};

// تابع اصلی سرور
void run_server(const std::string& ip, int port) {
    io_context io;
    auto acceptor = start_server(io, ip, port);

    // اتصال به دو کلاینت و ذخیره سوکت‌ها
    tcp::socket client1(io);
    tcp::socket client2(io);
    bool client1_connected = false, client2_connected = false;

    queue<string> messages_for_client1;
    queue<string> messages_for_client2;

    // انتظار برای اتصال کلاینت‌ها
    cout << "Waiting for Client 1 to connect..." << endl;
    acceptor.async_accept(client1, [&](const boost::system::error_code& error) {
        if (!error) {
            client1_connected = true;
            cout << "Client 1 connected." << endl;
        }
    });

    cout << "Waiting for Client 2 to connect..." << endl;
    acceptor.async_accept(client2, [&](const boost::system::error_code& error) {
        if (!error) {
            client2_connected = true;
            cout << "Client 2 connected." << endl;
        }
    });

    io.run();

    try {
        while (true) {
            // دریافت پیام از کلاینت اول و ارسال به کلاینت دوم یا ذخیره‌سازی
            if (client1_connected && client1.available()) {
                std::string data1 = receive_data(client1);
                if (!data1.empty()) {
                    cout << "Client 1: " << data1 << endl;
                    if (client2_connected) {
                        send_response(client2, "Client 1: " + data1);
                    } else {
                        messages_for_client2.push("Client 1: " + data1);
                    }
                }
            }

            // دریافت پیام از کلاینت دوم و ارسال به کلاینت اول یا ذخیره‌سازی
            if (client2_connected && client2.available()) {
                std::string data2 = receive_data(client2);
                if (!data2.empty()) {
                    cout << "Client 2: " << data2 << endl;
                    if (client1_connected) {
                        send_response(client1, "Client 2: " + data2);
                    } else {
                        messages_for_client1.push("Client 2: " + data2);
                    }
                }
            }

            // ارسال پیام‌های ذخیره‌شده به کلاینت اول
            if (client1_connected && !messages_for_client1.empty()) {
                send_response(client1, messages_for_client1.front());
                messages_for_client1.pop();
            }

            // ارسال پیام‌های ذخیره‌شده به کلاینت دوم
            if (client2_connected && !messages_for_client2.empty()) {
                send_response(client2, messages_for_client2.front());
                messages_for_client2.pop();
            }
        }
    } catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
    }
}

int main() {
    run_server("192.168.57.10", 1403);
    return 0;
}
