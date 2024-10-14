#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;

int main()
{
        io_context io_context;

        boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string("192.168.155.161"), 1403);
        tcp::acceptor acceptor(io_context, endpoint);

        while (true) {
        // ایجاد یک سوکت برای اتصال جدید
        tcp::socket socket(io_context);
        acceptor.accept(socket); // اتصال را می‌پذیرد

        // بافر برای ذخیره داده‌های خوانده‌شده
        std::string buffer(1024, '\0'); // یک رشته با طول 1024 و پر شده با '\0'

        try {
            // خواندن داده‌ها
            std::size_t len = socket.read_some(boost::asio::buffer(&buffer[0], buffer.size()));
            buffer.resize(len); // تغییر اندازه رشته به طول واقعی خوانده شده
            
            std::cout << "Received " << len << " bytes: " << buffer << std::endl;

            // ارسال پاسخ به کلاینت (اختیاری)
            std::string response = "Data received!";
            boost::asio::write(socket, boost::asio::buffer(response));
        } catch (std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }

    return 0;
}
