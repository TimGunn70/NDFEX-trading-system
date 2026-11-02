#include "multicast_socket.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <iostream>

// Changed to return -1 instead of exiting when a system call fails, also logged setsockopt failure
int create_multicast_socket(const char* mcast_addr, int port, const char* local_ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << "\n";
        return -1;
    }

    int reuse = 1;
 
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0){
        std::cerr << "setsockopt - SO_REUSEADDR failed: " << strerror(errno) << "\n";
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "setsockopt - SO_REUSEPORT failed: " << strerror(errno) << "\n";
    }

    int rcvbuf = 8 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        std::cerr << "setsockopt - SO_RCVBUF failed: " << strerror(errno) << "\n";
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "Failed to get file flags: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::cerr << "Error setting non-blocking: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (const sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind: " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr);
    mreq.imr_interface.s_addr = inet_addr(local_ip);

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::cerr << "Failed to join multicast group " << mcast_addr
                  << ": " << strerror(errno) << "\n";
        close(fd);
        return -1;
    }

    std::cout << "[Listener] Joined multicast " << mcast_addr << ":" << port << "\n";
    return fd;
}