#!/bin/bash

./start_service.sh --nodes 1 --rep 1

echo ""
sleep 5

echo "=== Test 1: Basic Single  Server GET/PUT ==="
echo ""

echo ">>> PUT key1 = value1"
./bin/client --put key1 --val value1
sleep 1

echo ""
echo ">>> GET key1 (expect: key1, value1, server...)"
./bin/client --get key1
sleep 1

echo ""
echo ">>> PUT key1 = value2 (overwrite)"
./bin/client --put key1 --val value2
sleep 1

echo ""
echo ">>> PUT key2 = value3 (expect: key2, value3, server...)"
./bin/client --put key2 --val value3
sleep 1

echo ""
echo ">>> PUT key3 = value4 (expect: key3, value4, server...)"
./bin/client --put key3 --val value4
sleep 1

echo ""
echo ">>> GET key1 (expect: key1, value2, server...)"
./bin/client --get key1
sleep 1

echo ""
echo ">>> GET key2 (expect: key2, value3, server...)"
./bin/client --get key2
sleep 1

echo ""
echo ">>> GET key3 (expect: key3, value4, server...)"
./bin/client --get key3
sleep 1


echo ""
echo "=== Test 1 Complete ==="
./stop_service.sh