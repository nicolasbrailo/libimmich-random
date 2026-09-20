CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread
CPPFLAGS += $(shell pkg-config --cflags libcurl json-c)
LDLIBS += $(shell pkg-config --libs libcurl json-c)

LIB := libimmich.a
LIB_OBJS := $(patsubst %.c,%.o,$(wildcard *.c))
EXAMPLE := example/immich-example
# album_filter.c is pure logic: no curl, no json-c, so its test links just it
TEST := test/album_filter_test

all: $(LIB) $(EXAMPLE)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(EXAMPLE): example/main.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

example/main.o: CPPFLAGS += -I.

test/album_filter_test.o: CPPFLAGS += -I.

$(TEST): test/album_filter_test.o album_filter.o
	$(CC) $(LDFLAGS) -o $@ $^

test: $(TEST)
	./$(TEST)

%.o: %.c $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB) $(LIB_OBJS) $(EXAMPLE) example/main.o $(TEST) \
	      test/album_filter_test.o

systemdeps:
	sudo apt install libcurl4-openssl-dev libjson-c-dev

.PHONY: all clean test systemdeps
