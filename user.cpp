#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;
int main()
{
    io_context io_context;

    ip::tcp::socket socket(io_context);
    ip::tcp::resolver resolver(io_context);
    
    connect(socket, resolver.resolve("127.0.0.1", "1403"));
    cout<<"we are connected:)\n";
    cout<<"type your massage\n";

    string message ; // پیام برای ارسال به سرور
    cin>>message;

    try {
        // ارسال داده به سرور
        write(socket, boost::asio::buffer(message));
        cout << "Sent message: " << message << endl;

        // ارسال پاسخ (اختیاری)
        string response;
        getline(std::cin, response);
        write(socket, boost::asio::buffer(response));
    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;

}