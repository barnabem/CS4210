#!/bin/bash

./start_service.sh --nodes 7 --rep 3

echo ""
sleep 5

echo "=== Test 4: Availability through multiple node failures ==="
echo ""

echo "Inserting 20 key-value pairs..."
for i in {1..20}; do
    echo ">>> PUT key$i = value$i"
    ./bin/client --put key$i --val value$i
    sleep 1
done

echo ""
echo "Overwriting keys 5, 10, 15..."
for i in 5 10 15; do
    echo ">>> PUT key$i = newvalue$i"
    ./bin/client --put key$i --val newvalue$i
    sleep 1
done


echo "Manually kill storage nodes via kill -9 <PID>"
echo "gtstore.pids contains the PIDs of the running nodes. (Ex: storageX <PID> <PORT>)"
echo "Try to kill nodes that hold keys 5, 10, or 15 if possible."
echo "Press Enter once you have killed 2 storage nodes..."
read

echo "Waiting 20 seconds for system to stabilize..."
sleep 20

echo ""

for i in {1..20}; do
    echo ""
    echo ">>> GET key$i (expect: key$i, value$i, server...)"
    ./bin/client --get key$i
    sleep 1
done


./stop_service.sh