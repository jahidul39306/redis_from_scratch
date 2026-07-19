#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <vector>

const size_t k_max_msg = 4096;

static void die(const char *msg){
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t do_something(int connfd, char *rbuf, uint32_t len) {
    printf("client says: %.*s\n", len, &rbuf[4]);

    const char reply[] = "world";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) {
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    // check if it has all the data or there is some pending data
    if (4 + len > conn->incoming.size()) {
        return false;
    }
    // get the address of where the actual message is 
    const uint8_t *request = &conn->incoming[4];

    printf("client says: len:%d data:%.*s\n",
        len, len < 100 ? len : 100, request);
    
    // sending back what it got
    // set the length of the msg
    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    // set the msg 
    buf_append(conn->outgoing, request, len);

    // only remove the req, which has been handled
    buf_consume(conn->incoming, 4 + len);
    
    return true;
}

static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN) {
        return;
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }

    buf_consume(conn->outgoing, (size_t)rv);

    if (conn->outgoing.size() == 0) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    // not err but interupted by another signal
    if (rv < 0 && errno == EAGAIN) {
        return;
    }
    // error
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }
    // EOF
    if (rv == 0) {
        if (conn->incoming.size() == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while (try_one_request(conn)) {}

    if (conn->outgoing.size() > 0) {
        conn->want_read = false;
        conn->want_write = true;

        return handle_write(conn);
    }
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        die("fcnl error");
    }

    flags |= O_NONBLOCK;

    errno = 0;
    if(fcntl(fd, F_SETFL, flags) == -1) {
        die("fcntl error");
    }
}

struct Conn {
    int fd = -1;

    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
}

static Conn *handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        
    if (connfd < 0) { 
        msg_errno("accept() error");
        return NULL;
    }
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port)
    );

    // making blocking fd to non-blocking fd
    fd_set_nb(connfd);

    // creating Conn for new fd
    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

int main(){
    // creating the fd
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) { die("socket()"); }

    // setting sock option to reuse the same port 
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind to an address
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) { die("bind()"); }

    // setting listening socket to non-blocking
    fd_set_nb(fd);

    // listen
    rv = listen(fd, SOMAXCONN); // for linux SOMAXCONN is 4096
    if (rv) { die("listen()"); }

    // list of client connections
    std::vector<Conn *> fd2conn;

    // keep accepting new connections
    std::vector<struct pollfd> poll_args;
    while (true) {
        poll_args.clear();
        // listening socket in the first position
        struct pollfd pfd = {fd, POLLIN, 0}; // just concern about new clinet
        poll_args.push_back(pfd);

        //remaining connection sockets
        for (Conn *conn : fd2conn) {
            if(!conn) {
                continue;
            }

            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read) {
                pfd.events |= POLLIN;
            }
            if (conn->want_write) {
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        // from another signal or interruption not err
        if (rv < 0 && errno == EINTR) {
            continue;
        }
        // actual err
        if (rv < 0) {
            die("poll");
        }

        // check listening socket, if there is any new client
        if (poll_args[0].revents & POLLIN) {
            if (Conn *conn = handle_accept(fd)) {
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                // checking the fd index's value should be null
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // handle connection sockets
        for (size_t i = 1; i < poll_args.size(); i++) {
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN) {
                handle_read(conn);
            }
            if (ready & POLLOUT) {
                handle_write(conn);
            }
        }

        // do_something(connfd);
        while (true) {
            int32_t err = one_request(connfd);
            if (err) {
                break;
            }
        }
        close(connfd);
    }
}