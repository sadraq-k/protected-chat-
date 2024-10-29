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
    auto acceptor1 = start_server(io, ip, port);
     auto acceptor2 = start_server(io, ip, port);
    //برای قبول درخواست های مداوم کلاینت
    while (true) 
    {
        tcp::socket socket1(io);
        acceptor1.accept(socket1);

        tcp::socket socket2(io);
        acceptor2.accept(socket2);

        try {
            //برای مداوم در ارتباط بودن با کلاینت
            while (true) 
            { // ﺢﻠﻘﻫ ﻥﺎﻤﺣﺩﻭﺩ ﺏﺭﺎﯾ ﺩﺮﯾﺎﻔﺗ ﻭ ﺍﺮﺳﺎﻟ ﺩﺍﺪﻫ
                auto data1 = receive_data(socket1);
                auto data2 = receive_data(socket2);

                if (data1.empty()) break; // ﺩﺭ ﺹﻭﺮﺗ ﺩﺮﯾﺎﻔﺗ ﺩﺍﺪﻫ ﺥﺎﻠﯾ، ﺢﻠﻘﻫ ﺭﺍ ﺐﺸﮑﻨﯾﺩ
                if (data2.empty()) break;



                if (data1 == "the end")
                {
                    cout<<"\n nice chat bye ;)\n ";
                    exit(0);
                }
                if (data2 == "the end")
                {
                    cout<<"\n nice chat bye ;)\n ";
                    exit(0);
                }


                
                cout << "Received from client1" << data1.size() << " bytes: " << data1 << endl;
                cout << "Received from client2" << data2.size() << " bytes: " << data2 << endl;

                cout << "Anything else for client?\n";

                string willsend1;
                string willsend2;

                willsend2 = data1;
                willsend1 = data2;
               // getline(cin, willsend1);
               // getline(cin, willsend2);

                send_response(socket1, willsend1);
                send_response(socket1, willsend2);
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
