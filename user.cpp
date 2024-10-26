#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;



// تابعی برای اتصال به سرور
auto connect_to_server = [](io_context& io, const std::string& host, const std::string& port) -> ip::tcp::socket 
{
    ip::tcp::socket socket(io);
    ip::tcp::resolver resolver(io);
    connect(socket, resolver.resolve(host, port));
    cout << "we are connected :)\n";
    return socket;
};




// تابعی برای ارسال پیام به سرور
auto send_message = [](ip::tcp::socket& socket, const std::string& message) 
{
    boost::asio::write(socket, boost::asio::buffer(message));
    cout << "Sent message: " << message << endl;
};




// تابع اصلی کاربر
void run_client(const std::string& host, const std::string& port)
 {
    io_context io;
    int i=0;
    auto socket = connect_to_server(io, host, port);

    while (i <= 10)
    {
        cout << "type your message\n";
        string message;
        getline(cin,message);

    try {
        send_message(socket, message);


        if (message == "the end")
        {
                cout<<"\ngoodbye server XD\n";
                exit(0);
        }

        // خواندن باسخ از سرور
        string response(1024, '\0'); // بافر برای ذخیره باسخ
        std::size_t len = socket.read_some(boost::asio::buffer(&response[0], response.size()));
        response.resize(len); 
        cout << "Received response: " << response << endl;
        
    } catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
    }
    }
     
    
}



int main() 
{
    run_client("127.0.0.1", "1403");
    return 0;
}
