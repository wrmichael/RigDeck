#include "netlog.h"
#include <cstring>
#include <cstdio>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static struct addrinfo* resolve(const std::string& host, int port, int type) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0) return nullptr;
    return res;
}

bool netSendUdp(const std::string& host, int port, const uint8_t* data, size_t len) {
    struct addrinfo* ai = resolve(host, port, SOCK_DGRAM);
    if (!ai) return false;
    int s = socket(ai->ai_family, SOCK_DGRAM, 0);
    bool ok = false;
    if (s >= 0) {
        ok = sendto(s, data, len, 0, ai->ai_addr, ai->ai_addrlen) == (ssize_t)len;
        close(s);
    }
    freeaddrinfo(ai);
    return ok;
}

bool netSendTcp(const std::string& host, int port, const uint8_t* data, size_t len, int timeoutMs) {
    struct addrinfo* ai = resolve(host, port, SOCK_STREAM);
    if (!ai) return false;
    int s = socket(ai->ai_family, SOCK_STREAM, 0);
    bool ok = false;
    if (s >= 0) {
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int r = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select(s + 1, nullptr, &wf, nullptr, &tv) == 1) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
                r = err ? -1 : 0;
            }
        }
        if (r == 0) {
            fcntl(s, F_SETFL, fl);
            struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            size_t sent = 0;
            while (sent < len) {
                ssize_t n = send(s, data + sent, len - sent, 0);
                if (n <= 0) break;
                sent += (size_t)n;
            }
            ok = sent == len;
        }
        close(s);
    }
    freeaddrinfo(ai);
    return ok;
}
