CC := gcc
CFLAGS := -Wall -Wextra -Iinclude -g
LDFLAGS := -lpthread

SRC := src/heap.c src/memory_source.c src/numa_topology.c
OBJ := $(SRC:.c=.o)

TEST_SRCS := $(wildcard test/test_*.c)
TEST_BINS := $(TEST_SRCS:.c=)

.PHONY: all test valgrind debug clean

all: $(TEST_BINS)

test/%: test/%.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "==== Running $$bin ===="; \
		./$$bin || exit 1; \
	done

valgrind: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "==== Valgrind $$bin ===="; \
		valgrind --leak-check=full --show-leak-kinds=all ./$$bin || exit 1; \
	done

debug: $(TEST_BINS)
	@if [ -z "$(TEST)" ]; then \
		echo "Usage: make debug TEST=test/test_name"; \
		echo "Available tests:"; \
		for bin in $(TEST_BINS); do echo "  $$bin"; done; \
		exit 1; \
	fi
	gdb -q --args $(TEST) $(ARGS)

clean:
	rm -f $(OBJ) $(TEST_BINS)
