CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -pthread -Icommon
BIN     = bin
COMMON  = common/net.c

all: dirs $(BIN)/simulator $(BIN)/node_c $(BIN)/stubs $(BIN)/node_p

dirs:
	@mkdir -p $(BIN) logs

$(BIN)/simulator: sim/simulator.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) sim/simulator.c $(COMMON) -o $@
$(BIN)/node_c: node_c/node_c.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) node_c/node_c.c $(COMMON) -o $@
$(BIN)/stubs: stubs/stubs.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) stubs/stubs.c $(COMMON) -o $@

test: dirs test_m4
	$(CC) $(CFLAGS) tests/test_protocol.c -o $(BIN)/test_protocol && ./$(BIN)/test_protocol
	$(CC) $(CFLAGS) tests/test_fallback.c -o $(BIN)/test_fallback && ./$(BIN)/test_fallback

# --- M4 : ressources partagees, noeud passagers ---
P_SRC = node_p/resources.c node_p/msg_queue.c

$(BIN)/node_p: node_p/node_p.c $(P_SRC) node_p/*.h $(COMMON) common/*.h
	$(CC) $(CFLAGS) node_p/node_p.c $(P_SRC) $(COMMON) -o $@

# tests M4 : le test de course compile resources.c avec une fenetre artificielle (voir race_demo.c)
test_m4: dirs
	$(CC) $(CFLAGS) tests/test_resources.c $(P_SRC) -o $(BIN)/test_resources && ./$(BIN)/test_resources
	$(CC) $(CFLAGS) -DRES_TEST_WINDOW_US=100 tests/race_demo.c node_p/resources.c -o $(BIN)/race_demo && ./$(BIN)/race_demo --csv logs/race_results.csv

# Chaque membre ajoute ici ses cibles : node_a (M6), node_b (M2), sched_sim (M5)

clean:
	rm -rf $(BIN) logs/*
.PHONY: all dirs test test_m4 clean
