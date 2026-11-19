#!/bin/bash
# Lance la demo complete : noeuds factices (ou vrais) + noeud C + simulateur.
#   ./run_demo.sh                                   scenario TU701, politique IA
#   POLICY=fixed SCENARIO=scenarios/multi_flights.txt ./run_demo.sh
#   NO_B=1 ./run_demo.sh                            panne de B -> mode degrade
#   SIM_ARGS="--set TU701.delay=25 --set queue=150" ./run_demo.sh
#   SIM_ARGS="--load 5" SCENARIO=scenarios/surcharge.txt ./run_demo.sh
cd "$(dirname "$0")"; make -s all || exit 1
SCENARIO=${SCENARIO:-scenarios/demo_tu701.txt}; POLICY=${POLICY:-ai}; SPEED=${SPEED:-1}
pkill -x stubs 2>/dev/null; pkill -x node_c 2>/dev/null; sleep 0.3
bin/stubs p > logs/p.log 2>&1 &
bin/stubs a > logs/a.log 2>&1 &
[ -z "$NO_B" ] && bin/stubs b > logs/b.log 2>&1 &
sleep 0.4
bin/node_c --policy "$POLICY" --idle-exit 1.5 --csv "logs/result_${POLICY}.csv" > logs/c.log 2>&1 &
CPID=$!; sleep 0.4
bin/simulator --scenario "$SCENARIO" --speed "$SPEED" $SIM_ARGS
wait $CPID
echo; echo "----- Chronologie du noeud C -----"; cat logs/c.log
pkill -x stubs 2>/dev/null
