# redis_from_scratch

Minimal educational Redis-like key-value server and client implemented in C++.

I originally started this project with the goal of building a Redis clone from scratch. In retrospect, that goal was overly ambitious given my current understanding of systems programming. While working on the project, I ended up learning much more than I expected about network programming, TCP sockets, non-blocking I/O, and how a single-threaded server can manage multiple client connections. I also gained a better understanding of how the Linux kernel and mechanisms such as `poll()` make it possible for a single-threaded event-driven server to efficiently handle many concurrent connections.

Another important lesson was that the choice of data structures and algorithms matters significantly in systems programming. Using an STL container such as `std::map` is convenient and appropriate for this educational implementation, but high-performance systems often require data structures and memory-management strategies chosen specifically for their workload and performance requirements.

At this point, I am postponing the goal of building a full Redis clone because I realized that I have some gaps in my systems-programming fundamentals that I need to strengthen first. I plan to come back to this project later and continue building it with a stronger understanding of the underlying concepts.

For now, this repository is simply a small educational networked key-value server. It can handle multiple client connections using a single-threaded event-driven architecture, but it is **nowhere near being a Redis implementation**. The project is therefore less about recreating Redis and more about documenting what I learned while trying to understand how a networked server works from the ground up.


Repository structure

- [server.cpp](/home/jahid/code_space/redis.worktrees/generate-readme-md-file/server.cpp) — Minimal Redis-like server implementation (C++)
- [client.cpp](/home/jahid/code_space/redis.worktrees/generate-readme-md-file/client.cpp) — Simple client to connect to the server (C++)
- [Makefile](/home/jahid/code_space/redis.worktrees/generate-readme-md-file/Makefile) — Convenience build/run targets
- [LICENSE](/home/jahid/code_space/redis.worktrees/generate-readme-md-file/LICENSE)
- .gitignore

Requirements

- GNU/Linux or macOS development environment
- A C++ compiler with C++17 support (g++ recommended)
- make (optional, provided Makefile)

No external libraries are required.

Build

Recommended: use the provided Makefile.

```bash
# build both server and client
make
```

Manual build (g++):

```bash
# compile server
g++ -std=c++17 -O2 -Wall server.cpp -o server

# compile client
g++ -std=c++17 -O2 -Wall client.cpp -o client
```

The sources do not require -pthread for the current implementation.

What the programs do (exact behavior)

- server.cpp
  - Listens on TCP port 1234 (bound to 0.0.0.0) and accepts multiple clients using non-blocking sockets and poll(2).
  - Implements a tiny command set: `get <key>`, `set <key> <value>`, and `del <key>`.
  - Uses an in-memory std::map<std::string,std::string> as the key-value store (no persistence).
  - Limits: message size up to 32 MiB and at most 3 arguments per request.

- client.cpp
  - Connects to 127.0.0.1 (loopback) port 1234 and sends a single request composed from its command-line arguments.
  - Usage: ./client <command> [args...]
  - Example: `./client set mykey hello` or `./client get mykey`.

Wire protocol (native binary framing)

Request (from client to server):
- 4 bytes (uint32_t little-endian) total payload length L (not counting these initial 4 bytes). L includes the next 4 bytes and all argument length+data fields.
- 4 bytes (uint32_t) number of string arguments N (N <= 3)
- For each argument i in 1..N:
  - 4 bytes (uint32_t) length of argument i (M_i)
  - M_i bytes of argument i data (not NUL-terminated)

Examples of arguments used by this project:
- `get key` -> N=2, args[0]="get", args[1]="key"
- `set key value` -> N=3, args[0]="set", args[1]="key", args[2]="value"

Response (from server to client):
- 4 bytes (uint32_t) response length R (equals 4 + payload-data-bytes)
- 4 bytes (uint32_t) status code
- (R - 4) bytes of optional data (e.g., value for GET)

Status codes (server.cpp):
- 0 — RES_OK (success)
- 1 — RES_ERR (general error / unknown command)
- 2 — RES_NX (not exists / key not found)

Usage

Start the server (foreground):

```bash
./server
```

Start the server in background (recommended for local testing):

```bash
nohup ./server &> server.log &
# or use the provided Makefile target:
make start-server
```

Send requests with the client (single command per invocation):

```bash
# set a key
./client set mykey "hello world"

# get a key
./client get mykey
# expected output when present: server says: [0] hello world
# expected output when absent:  server says: [2] 

# delete a key
./client del mykey
```

You can also use the Makefile helper to run the client (pass ARGS):

```bash
make run-client ARGS="get mykey"
```

Examples and expected output

1) Set and get

Terminal A:
$ ./server
[server logs...]

Terminal B:
$ ./client set x 123
server says: [0]

$ ./client get x
server says: [0] 123

2) Get a non-existent key

$ ./client get unknown
server says: [2]

Notes and limitations

- This is an educational prototype. It intentionally omits authentication, persistence, replication, and many safety checks.
- Message framing is binary and little-endian (native uint32_t memcpy). If using this protocol from other languages, ensure consistent endianness and uint32_t sizes.
- The server binds to port 1234 and the client connects to loopback: change the source if you need different addresses/ports.

Testing

Manual test steps:
1. Build (`make`)
2. Start server (`make start-server` or `./server`)
3. Use the client to issue commands as shown above
4. Inspect `server.log` (if started with nohup) for server-side prints

Contributing

Contributions are welcome. Suggested improvements:
- Add command parser and ASCII protocol compatibility (RESP-like)
- Add persistence (snapshotting or append-only log)
- Add configurable host/port via command-line flags
- Add unit and integration tests

License

This project is available under the MIT License — see [LICENSE](/home/jahid/code_space/redis.worktrees/generate-readme-md-file/LICENSE).

**Note:** This README was generated with the assistance of an AI coding agent based on my experience and understanding of the project.
