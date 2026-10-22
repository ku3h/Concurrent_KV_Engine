CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2 -Iinclude
SANITIZE_FLAGS = -Wall -Wextra -pthread -g -fsanitize=thread -Iinclude
ASAN_FLAGS = -Wall -Wextra -pthread -g -fsanitize=address -Iinclude

# Core source files shared across all targets
ENGINE_SRCS = src/hash_table.c src/wal.c src/checkpoint.c src/main.c
SERVER_SRCS = src/hash_table.c src/wal.c src/checkpoint.c src/server.c src/server_main.c
BENCH_SRCS = src/hash_table.c src/wal.c benchmarks/benchmark_throughput.c

all: engine server bin/benchmark

engine: $(ENGINE_SRCS)
	$(CC) $(CFLAGS) $(ENGINE_SRCS) -o bin/engine

server: $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o bin/server

bin/benchmark: $(BENCH_SRCS)
	$(CC) $(CFLAGS) $(BENCH_SRCS) -o bin/benchmark -lm

benchmark: bin/benchmark
	./bin/benchmark

test: bin/test-concurrency bin/test-wal bin/test-eviction bin/test-checkpoint
	./bin/test-concurrency
	./bin/test-wal
	./bin/test-eviction
	./bin/test-checkpoint

bin/test-concurrency: src/hash_table.c src/thread_pool.c tests/test_concurrency.c
	$(CC) $(CFLAGS) src/hash_table.c src/thread_pool.c tests/test_concurrency.c -o bin/test-concurrency

bin/test-wal: src/hash_table.c src/wal.c tests/test_wal.c
	$(CC) $(CFLAGS) src/hash_table.c src/wal.c tests/test_wal.c -o bin/test-wal

bin/test-eviction: src/hash_table.c tests/test_eviction.c
	$(CC) $(CFLAGS) src/hash_table.c tests/test_eviction.c -o bin/test-eviction

bin/test-checkpoint: src/hash_table.c src/wal.c src/checkpoint.c tests/test_checkpoint.c
	$(CC) $(CFLAGS) src/hash_table.c src/wal.c src/checkpoint.c tests/test_checkpoint.c -o bin/test-checkpoint

test-tsan: src/hash_table.c src/thread_pool.c tests/test_concurrency.c
	$(CC) $(SANITIZE_FLAGS) src/hash_table.c src/thread_pool.c tests/test_concurrency.c -o bin/test-tsan
	./bin/test-tsan

test-asan: src/hash_table.c src/wal.c src/checkpoint.c tests/test_wal.c tests/test_eviction.c tests/test_checkpoint.c
	$(CC) $(ASAN_FLAGS) src/hash_table.c src/wal.c src/checkpoint.c tests/test_wal.c -o bin/test-asan-wal
	$(CC) $(ASAN_FLAGS) src/hash_table.c tests/test_eviction.c -o bin/test-asan-eviction
	$(CC) $(ASAN_FLAGS) src/hash_table.c src/wal.c src/checkpoint.c tests/test_checkpoint.c -o bin/test-asan-checkpoint
	./bin/test-asan-wal
	./bin/test-asan-eviction
	./bin/test-asan-checkpoint

clean:
	rm -rf bin/* *.wal *.log *.bin *.tmp
