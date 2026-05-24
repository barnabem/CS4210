#!/bin/bash



./start_service.sh --nodes 3 --rep 2

echo ""
sleep 5

echo "=== Test 3: Availability thorugh Single Node Failure ==="
echo ""

for i in {1..15}; do
    echo ">>> PUT key$i = value$i"
    ./bin/client --put key$i --val value$i
    sleep 1
done

echo "Manually kill storage nodes via kill -9 <PID>"
echo "gtstore.pids contains the PIDs of the running nodes. (Ex: storageX <PID> <PORT>)"
echo "Press Enter once you have killed 1 storage node..."
read

echo "Waiting 20 seconds for system to stabilize..."
sleep 20

echo ""

for i in {1..15}; do
    echo ""
    echo ">>> GET key$i (expect: key$i, value$i, server...)"
    ./bin/client --get key$i
    sleep 1
done


./stop_service.sh
