#include <iostream>
#include <boost/asio.hpp>

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

    cout << "Waiting for Client 1 to connect..." << endl;
    acceptor.accept(client1);
    cout << "Client 1 connected." << endl;

    cout << "Waiting for Client 2 to connect..." << endl;
    acceptor.accept(client2);
    cout << "Client 2 connected." << endl;

    try {
        while (true) {
            // دریافت پیام از کلاینت اول و ارسال به کلاینت دوم
            if (client1.available()) {
                std::string data1 = receive_data(client1);
                if (!data1.empty()) {
                    cout << "Client 1: " << data1 << endl;
                    send_response(client2, "Client 1: " + data1);
                }
            }

            // دریافت پیام از کلاینت دوم و ارسال به کلاینت اول
            if (client2.available()) {
                std::string data2 = receive_data(client2);
                if (!data2.empty()) {
                    cout << "Client 2: " << data2 << endl;
                    send_response(client1, "Client 2: " + data2);
                }
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
