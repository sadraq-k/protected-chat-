#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;

// تابعی برای اتصال به سرور
auto connect_to_server = [](io_context& io, const std::string& host, const std::string& port) -> ip::tcp::socket {
    ip::tcp::socket socket(io);
    ip::tcp::resolver resolver(io);
    connect(socket, resolver.resolve(host, port));
    cout << "we are connected :)\n";
    return socket;
};

// تابعی برای ارسال پیام به سرور
auto send_message = [](ip::tcp::socket& socket, const std::string& message) {
    boost::asio::write(socket, boost::asio::buffer(message));
    cout << "Sent message: " << message << endl;
};

// تابع اصلی کاربر
void run_client(const std::string& host, const std::string& port) {
    io_context io;
    auto socket = connect_to_server(io, host, port);

    cout << "type your message\n";
    string message;
    cin >> message;

    try {
        send_message(socket, message);

        // ارسال پاسخ دوم (اختیاری)
        string response;
        getline(cin, response);
        send_message(socket, response);
    } catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
    }
}

int main() {
    run_client("192.168.155.161", "1403");
    return 0;
}
