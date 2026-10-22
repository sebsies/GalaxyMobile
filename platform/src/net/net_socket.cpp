#include "net/net_socket.hpp"
#include "port/port.h"

#include <cstring>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <ifaddrs.h>
#define closesocket close
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

namespace {
#ifdef _WIN32
    struct WsaInit {
        WsaInit() {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
        }
        ~WsaInit() {
            WSACleanup();
        }
    };
    static WsaInit sWsaInit;
#endif
}

NetSocket::NetSocket() : mSockFd(-1), mPort(0) {}

NetSocket::~NetSocket() {
    close();
}

bool NetSocket::open(int port, bool broadcast) {
    close();

    int fd = (int)::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        PORT_LOG("NetSocket: socket creation failed\n");
        return false;
    }

    // Reuse address
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Enable broadcast if requested
    if (broadcast) {
        int bOpt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&bOpt, sizeof(bOpt));
    }

    // Set non-blocking mode
#ifdef _WIN32
    u_long mode = 1;
    ::ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif

    // Bind if port is specified (or 0 for ephemeral system port)
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        PORT_LOG("NetSocket: bind to port %d failed\n", port);
        ::closesocket(fd);
        return false;
    }

    // Determine actual bound port
    socklen_t addrLen = sizeof(addr);
    if (::getsockname(fd, (struct sockaddr*)&addr, &addrLen) == 0) {
        mPort = ntohs(addr.sin_port);
    } else {
        mPort = port;
    }

    mSockFd = fd;
    PORT_LOG("NetSocket: opened UDP socket on port %d\n", mPort);
    return true;
}

void NetSocket::close() {
    if (mSockFd >= 0) {
        ::closesocket(mSockFd);
        mSockFd = -1;
        mPort = 0;
    }
}

bool NetSocket::sendTo(const void* data, size_t size, const std::string& ip, int port) {
    if (mSockFd < 0 || !data || size == 0) return false;

    sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);

    if (::inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) <= 0) {
        // Try resolving hostname
        struct hostent* he = ::gethostbyname(ip.c_str());
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            std::memcpy(&dest.sin_addr, he->h_addr_list[0], sizeof(dest.sin_addr));
        } else {
            return false;
        }
    }

    int sent = ::sendto(mSockFd, (const char*)data, (int)size, 0, (struct sockaddr*)&dest, sizeof(dest));
    return sent == (int)size;
}

bool NetSocket::broadcast(const void* data, size_t size, int port) {
    return sendTo(data, size, "255.255.255.255", port);
}

int NetSocket::receiveFrom(void* buffer, size_t maxLen, std::string& outIp, int& outPort) {
    if (mSockFd < 0 || !buffer || maxLen == 0) return -1;

    sockaddr_in src;
    socklen_t srcLen = sizeof(src);
    int bytes = ::recvfrom(mSockFd, (char*)buffer, (int)maxLen, 0, (struct sockaddr*)&src, &srcLen);

    if (bytes > 0) {
        char ipBuf[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &src.sin_addr, ipBuf, sizeof(ipBuf))) {
            outIp = ipBuf;
        } else {
            outIp = "";
        }
        outPort = ntohs(src.sin_port);
        return bytes;
    }

    return -1;
}

std::string NetSocket::getLocalIpAddress() {
    // Probing by creating a temporary UDP socket connecting to a non-routable address
    int fd = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "127.0.0.1";

    sockaddr_in probe;
    std::memset(&probe, 0, sizeof(probe));
    probe.sin_family = AF_INET;
    probe.sin_port = htons(9); // Discard protocol
    probe.sin_addr.s_addr = inet_addr("8.8.8.8");

    std::string result = "127.0.0.1";
    if (::connect(fd, (struct sockaddr*)&probe, sizeof(probe)) == 0) {
        sockaddr_in local;
        socklen_t len = sizeof(local);
        if (::getsockname(fd, (struct sockaddr*)&local, &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
                result = buf;
            }
        }
    }
    ::closesocket(fd);
    return result;
}
