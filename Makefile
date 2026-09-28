# Makefile -- Friend Recommendation Engine (DSA capstone)
#
#   make        build ./friend_recs (CLI) and ./friend_server (HTTP API)
#   make test   build + run the CLI once, auto-running the self-test suite
#   make serve  build + start the HTTP server (http://localhost:8080)
#   make clean  remove build artifacts
#
# -Wall -Wextra -Werror: the grader should see zero warnings.

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -Werror -O2
LDFLAGS :=
BIN     := friend_recs
SERVER  := friend_server

SRCS := main.c graph.c recommend.c tests.c
SERVER_SRCS := server.c graph.c recommend.c
HDRS := graph.h recommend.h tests.h

# winsock2 is only needed for the Windows socket build of the server.
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows_NT)
ifeq ($(OS),Windows_NT)
  SERVER_LIBS := -lws2_32
else
  SERVER_LIBS :=
endif

.PHONY: all test serve clean

all: $(BIN) $(SERVER)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(SERVER): $(SERVER_SRCS) graph.h recommend.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(LDFLAGS) $(SERVER_LIBS)

# Feeds "13\n0\n" to the running CLI: run the self-tests, then exit.
test: $(BIN)
	@printf '13\n0\n' | ./$(BIN) | grep -E '\[(1|2|3|4|5|6|7|8|9)\]|RESULT'

# Build the server, then start it (Ctrl+C to stop).
serve: $(SERVER)
	./$(SERVER)

clean:
	rm -f $(BIN) $(SERVER)
