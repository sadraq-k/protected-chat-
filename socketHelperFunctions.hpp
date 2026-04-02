#ifndef SOCKET_HELPER_FUCTION_H
#define SOCKET_HELPER_FUCTION_H

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <sstream>
#include <vector>
#include <iostream>

#include <boost/asio/ssl.hpp>
#include <string>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using json = nlohmann::json;
// --- Helper Functions ---
// Read exactly N bytes
const uint32_t MAX_MESSAGE_SIZE = 1024 * 10; // Max 10KB messages

awaitable<void> read_exactly
(boost::asio::ssl::stream<tcp::socket>& socket,
    boost::asio::mutable_buffer buffer) {
    co_await boost::asio::async_read(socket, buffer, use_awaitable);
}

// Write data with length prefix (handling endianness)
awaitable<void> write_message
(boost::asio::ssl::stream<tcp::socket>& socket, const std::string& message) {
    uint32_t message_length = static_cast<uint32_t>(message.length());
    if (message_length > MAX_MESSAGE_SIZE) {
        throw std::runtime_error("Message too large");
    }

    // Convert length to network byte order
    uint32_t network_length = htonl(message_length); // htonl for host to network long

    std::vector<boost::asio::const_buffer> buffers;
    buffers.push_back(boost::asio::buffer(&network_length, sizeof(network_length)));
    buffers.push_back(boost::asio::buffer(message));

    co_await boost::asio::async_write(socket, buffers, use_awaitable);
}
//
awaitable<void> readFromSocket(boost::asio::ssl::stream<tcp::socket>& stream_,
    std::vector<char>& message_buffer, std::string& return_message)
{
    // 1. Read message length (network byte order)
    uint32_t message_length_net = 0;
    boost::asio::mutable_buffer length_buffer
    (&message_length_net, sizeof(message_length_net));

    co_await read_exactly(stream_, length_buffer);

    // Convert length from network to host byte order
    uint32_t message_length = ntohl(message_length_net); // ntohl for network to host long

    if (message_length > MAX_MESSAGE_SIZE) {
        throw std::runtime_error("Received message too large");
    }

    // 2. Read message body
    message_buffer.resize(message_length);
    co_await read_exactly(stream_, boost::asio::buffer(message_buffer));

    return_message = std::string(message_buffer.begin(), message_buffer.end());
    std::cout << "Received: " << return_message; // Log to server console
}
#endif