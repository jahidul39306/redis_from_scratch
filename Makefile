# Makefile for redis_from_scratch
# Simple build and run helpers

CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall

all: server client

server:
	$(CXX) $(CXXFLAGS) server.cpp -o server

client:
	$(CXX) $(CXXFLAGS) client.cpp -o client

clean:
	rm -f server client server.log server.pid

# Run server in foreground (use a separate terminal)
run-server: server
	./server

# Start server in background and save pid
start-server: server
	@nohup ./server &> server.log & echo $$! > server.pid && echo "server started (pid=`cat server.pid`)"

# Run client with arguments. Example: make run-client ARGS="get mykey"
run-client: client
	./client $(ARGS)
