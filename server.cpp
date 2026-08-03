#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <poll.h>

#include <vector>
#include <string>
#include <map>


const size_t k_max_msg = 32 << 20;
const size_t k_max_args = 3;

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX  = 2,
};

struct Buffer {
    uint8_t *buffer_begin;
    uint8_t *buffer_end;

    uint8_t *data_begin;
    uint8_t *data_end;
};

static size_t buf_size(const Buffer &buf) {
    return buf.data_end - buf.data_begin;
}

void buffer_init(Buffer &buf, size_t cap) {
    buf.buffer_begin = new uint8_t[cap];
    buf.buffer_end = buf.buffer_begin + cap;

    buf.data_begin = buf.buffer_begin;
    buf.data_end = buf.buffer_begin;
}

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

static bool buf_append(Buffer *buf, const uint8_t *data, size_t len) {
    size_t free = buf->buffer_end - buf->data_end;

    if (free < len) {
        size_t used = buf->data_end - buf->data_begin;

        memmove(buf->buffer_begin, buf->data_begin, used);

        buf->data_begin = buf->buffer_begin;
        buf->data_end = buf->buffer_begin + used;

        free = buf->buffer_end - buf->data_end;
        if (free < len) {
            msg("buffer overflow");
            return false;
        }
    }
    memcpy(buf->data_end, data, len);
    buf->data_end += len;
    return true;
}

static void buf_consume(Buffer *buf, size_t n) {
    assert(buf->data_begin + n <= buf->data_end);

    buf->data_begin += n;

    if (buf->data_begin == buf->data_end) {
        buf->data_begin = buf->buffer_begin;
        buf->data_end   = buf->buffer_begin;
    }
}

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) {
        return false;
    }
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out) {
    if (cur + n > end) {
        return false;
    }
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    const uint8_t *end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr)) {
        return -1;
    }
    if (nstr > k_max_args) {
        return -1;  // limit to 3 arguments
    } 

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) {
            return -1;
        }
        out.push_back(std::string());
        if (!read_str(data, end, len, out.back())) {
            return -1;
        }
    }
    if (data != end) {
        return -1;
    }
    return 0;
}

struct Response {
    uint32_t status = 0;
    std::vector<uint8_t> data;
};

static std::map<std::string, std::string> g_data;

static void do_request(std::vector<std::string> &cmd, Response &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        auto it = g_data.find(cmd[1]);
        if (it == g_data.end()) {
            out.status = RES_NX;
            return;
        }
        const std::string &val = it->second;
        out.data.assign(val.begin(), val.end());
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        g_data[cmd[1]].swap(cmd[2]);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        g_data.erase(cmd[1]);
    } else {
        out.status = RES_ERR;
    }
}

static void make_response(const Response &resp, Buffer &out) {
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    buf_append(&out, (const uint8_t *)&resp_len, 4);
    buf_append(&out, (const uint8_t *)&resp.status, 4);
    buf_append(&out, resp.data.data(), resp.data.size());
}

struct Conn {
    int fd = -1;

    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    
    Buffer incoming;
    Buffer outgoing;
};

static bool try_one_request(Conn *conn) {
    if (buf_size(conn->incoming) < 4) {
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data_begin, 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    // check if it has all the data or there is some pending data
    if (4 + len > buf_size(conn->incoming)) {
        return false;
    }
    // get to the address of where the actual message is 
    const uint8_t *request = conn->incoming.data_begin + 4;

    printf("client says: len:%d data:%.*s\n",
        len, len < 100 ? len : 100, request);
    
    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        conn->want_close = true;
        return false;
    }
    Response resp;
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);

    // only remove the req, which has been handled
    buf_consume(&conn->incoming, 4 + len);
    
    return true;
}

static void handle_write(Conn *conn) {
    assert(buf_size(conn->outgoing) > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data_begin, buf_size(conn->outgoing));
    if (rv < 0 && errno == EAGAIN) {
        return;
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }

    buf_consume(&conn->outgoing, (size_t)rv);

    if (buf_size(conn->outgoing) == 0) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn) {
    char tmp[4096];
    ssize_t rv = read(conn->fd, tmp, sizeof(tmp));
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
        if (buf_size(conn->incoming) == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return;
    }

    if (!buf_append(&conn->incoming, (const uint8_t *)tmp, (size_t)rv)) {
        conn->want_close = true;
        return;
    }

    while (try_one_request(conn)) {}

    if (buf_size(conn->outgoing) > 0) {
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
    buffer_init(conn->incoming, k_max_msg);
    buffer_init(conn->outgoing, k_max_msg);
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
            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete[] conn->incoming.buffer_begin;
                delete[] conn->outgoing.buffer_begin;
                delete conn;
            }
        }
    }
    return 0;
}