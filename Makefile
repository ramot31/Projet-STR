CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -pthread -Icommon
BIN     = bin
COMMON  = common/net.c

all: dirs $(BIN)/simulator $(BIN)/node_c $(BIN)/stubs

dirs:
	@mkdir -p $(BIN) logs

$(BIN)/simulator: sim/simulator.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) sim/simulator.c $(COMMON) -o $@
$(BIN)/node_c: node_c/node_c.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) node_c/node_c.c $(COMMON) -o $@
$(BIN)/stubs: stubs/stubs.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) stubs/stubs.c $(COMMON) -o $@

test: dirs
	$(CC) $(CFLAGS) tests/test_protocol.c -o $(BIN)/test_protocol && ./$(BIN)/test_protocol
	$(CC) $(CFLAGS) tests/test_fallback.c -o $(BIN)/test_fallback && ./$(BIN)/test_fallback

# Chaque membre ajoute ici ses cibles : node_a (M6), node_b (M2), node_p (M4), sched_sim (M5)

clean:
	rm -rf $(BIN) logs/*
.PHONY: all dirs test clean
