#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using namespace boost::asio;
using boost::asio::ip::tcp;




// تابعی برای راه‌اندازی سرور
auto start_server = [](io_context& io, const std::string& ip, int port) -> tcp::acceptor
{
    tcp::endpoint endpoint(boost::asio::ip::address::from_string(ip), port);
    tcp::acceptor acceptor(io, endpoint);
    acceptor.listen();
    return acceptor;
};




// تابعی برای دریافت داده از کلاینت
auto receive_data = [](tcp::socket& socket) -> std::string 
{
    std::string buffer(1024, '\0');
    std::size_t len = socket.read_some(boost::asio::buffer(&buffer[0], buffer.size()));
    buffer.resize(len);
    return buffer;
};



// تابعی برای ارسال پاسخ به کلاینت
auto send_response = [](tcp::socket& socket, const std::string& response)
{
    boost::asio::write(socket, boost::asio::buffer(response));
};



// تابع اصلی سرور
void run_server(const std::string& ip, int port) 
{
    io_context io;
    int i = 0;
    auto acceptor = start_server(io, ip, port);

    //برای قبول درخواست های مداوم کلاینت
    while (true) 
    {
        tcp::socket socket(io);
        acceptor.accept(socket);

        try {
            //برای مداوم در ارتباط بودن با کلاینت
            while (true) 
            { // ﺢﻠﻘﻫ ﻥﺎﻤﺣﺩﻭﺩ ﺏﺭﺎﯾ ﺩﺮﯾﺎﻔﺗ ﻭ ﺍﺮﺳﺎﻟ ﺩﺍﺪﻫ
                auto data = receive_data(socket);
                if (data.empty()) break; // ﺩﺭ ﺹﻭﺮﺗ ﺩﺮﯾﺎﻔﺗ ﺩﺍﺪﻫ ﺥﺎﻠﯾ، ﺢﻠﻘﻫ ﺭﺍ ﺐﺸﮑﻨﯾﺩ
                if (data == "the end")
                {
                    cout<<"\n nice chat bye ;)\n ";
                    exit(0);
                }
                
                cout << "Received " << data.size() << " bytes: " << data << endl;

                cout << "Anything else for client?\n";
                string willsend;
                getline(cin, willsend);

                send_response(socket, willsend);
            }
        } catch (const std::exception& e) 
        {
            cerr << "Error: " << e.what() << endl;
        }
    }

}

int main() 
{
    run_server("192.168.57.10", 1403);
    return 0;
}
