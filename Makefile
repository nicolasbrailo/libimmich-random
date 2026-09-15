CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread
CPPFLAGS += $(shell pkg-config --cflags libcurl json-c)
LDLIBS += $(shell pkg-config --libs libcurl json-c)

LIB := libimmich.a
LIB_OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
EXAMPLE := example/immich-example

all: $(LIB) $(EXAMPLE)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(EXAMPLE): example/main.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

example/main.o: CPPFLAGS += -I.

%.o: %.c $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB) $(LIB_OBJS) $(EXAMPLE) example/main.o

systemdeps:
	sudo apt install libcurl4-openssl-dev libjson-c-dev

.PHONY: all clean systemdeps
