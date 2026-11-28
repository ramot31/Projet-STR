CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -pthread -Icommon
BIN     = bin
COMMON  = common/net.c

all: dirs $(BIN)/simulator $(BIN)/node_c $(BIN)/stubs $(BIN)/node_p $(BIN)/rt_engine

dirs:
	@mkdir -p $(BIN) logs

$(BIN)/simulator: sim/simulator.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) sim/simulator.c $(COMMON) -o $@
$(BIN)/node_c: node_c/node_c.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) node_c/node_c.c $(COMMON) -o $@
$(BIN)/stubs: stubs/stubs.c $(COMMON) common/*.h
	$(CC) $(CFLAGS) stubs/stubs.c $(COMMON) -o $@

test: dirs test_m4 test_rt
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

# --- M3 : threads POSIX, taches periodiques, ordonnancement intelligent ---
RT_SRC = rt/main.c rt/acquisition.c rt/tasks.c rt/flights.c rt/rt_task.c rt/ai_policy.c
RT_FLAGS = $(CFLAGS) -Inode_p -Irt

$(BIN)/rt_engine: $(RT_SRC) rt/*.h $(P_SRC) node_p/*.h $(COMMON) common/*.h
	$(CC) $(RT_FLAGS) $(RT_SRC) $(P_SRC) $(COMMON) -o $@

test_rt: dirs
	$(CC) $(RT_FLAGS) tests/test_rt.c rt/rt_task.c rt/ai_policy.c -o $(BIN)/test_rt && ./$(BIN)/test_rt

# comparaison priorites fixes / priorites IA sur TU701 (87 %), TU703 (65 %), TU702 (20 %).
# SCHED_FIFO demande les droits root : `sudo make rt-compare` (sinon repli nice, signale dans la sortie)
rt-compare: dirs $(BIN)/rt_engine
	./$(BIN)/rt_engine --scenario scenarios/multi_flights.txt --policy fixed --csv-prefix logs/rt_fixed
	./$(BIN)/rt_engine --scenario scenarios/multi_flights.txt --policy ai --csv-prefix logs/rt_ai

rt-demo: dirs $(BIN)/rt_engine
	./$(BIN)/rt_engine --scenario scenarios/demo_tu701.txt --csv-prefix logs/rt_demo

# Chaque membre ajoute ici ses cibles : node_a (M6), node_b (M2), sched_sim (M5)

clean:
	rm -rf $(BIN) logs/*
.PHONY: all dirs test test_m4 test_rt rt-compare rt-demo clean
