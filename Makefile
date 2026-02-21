CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Iinclude
CFLAGS += -DSLAB_DEBUG=1
CFLAGS += -DSLAB_ENABLE_THREAD_CACHE=1

# pthread (멀티스레드 테스트/벤치용)
LDFLAGS := -pthread

SRC := src/slab.c

all: example test_suite test_doublefree test_overrun bench

example:
	$(CC) $(CFLAGS) $(SRC) examples/example.c -o build_example $(LDFLAGS)

test_suite:
	$(CC) $(CFLAGS) $(SRC) tests/test_suite.c -o build_test_suite $(LDFLAGS)

# 아래 두 개는 "의도적으로 abort"가 정상인 크래시 테스트
test_doublefree:
	$(CC) $(CFLAGS) $(SRC) tests/test_doublefree.c -o build_test_doublefree $(LDFLAGS)

test_overrun:
	$(CC) $(CFLAGS) $(SRC) tests/test_overrun.c -o build_test_overrun $(LDFLAGS)

bench:
	$(CC) $(CFLAGS) $(SRC) tests/bench.c -o build_bench $(LDFLAGS)

clean:
	rm -f build_example build_test_suite build_test_doublefree build_test_overrun build_bench
