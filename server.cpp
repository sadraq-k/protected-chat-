#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;

// تابعی برای راه‌اندازی سرور
auto start_server = [](io_context& io, const std::string& ip, int port) -> tcp::acceptor {
    tcp::endpoint endpoint(boost::asio::ip::address::from_string(ip), port);
    tcp::acceptor acceptor(io, endpoint);
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

    while (true) {
        tcp::socket socket(io);
        acceptor.accept(socket);

        try {
            auto data = receive_data(socket);
            cout << "Received " << data.size() << " bytes: " << data << endl;
            send_response(socket, "Data received!");
        } catch (const std::exception& e) {
            cerr << "Error: " << e.what() << endl;
        }
    }
}

int main() {
    run_server("192.168.155.161", 1403);
    return 0;
}
