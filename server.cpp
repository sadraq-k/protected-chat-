//
// Created by User on 9/13/2024.
//
#include <iostream>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>

#pragma comment(lib, "Ws2_32.lib")
#include "EasySocket.hpp"
using namespace std;
using namespace masesk;

void handleData(const std::string &data) {
    cout << "Client sent: " + data << endl;
}

int main() {
    EasySocket socketManager;                      // 1
    socketManager.socketListen("test", 8080, &handleData); // 2
    return 0;
}
