#pragma once

#include <string>
#include <vector>
#include <stdint.h>

class NetSocket {
public:
    NetSocket();
    ~NetSocket();

    bool open(int port = 0, bool broadcast = true);
    void close();

    bool isOpened() const { return mSockFd >= 0; }
    int getPort() const { return mPort; }

    bool sendTo(const void* data, size_t size, const std::string& ip, int port);
    bool broadcast(const void* data, size_t size, int port);
    int receiveFrom(void* buffer, size_t maxLen, std::string& outIp, int& outPort);

    static std::string getLocalIpAddress();

private:
    int mSockFd;
    int mPort;
};
