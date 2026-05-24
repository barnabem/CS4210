#!/bin/bash
NODES=1
REP=1
MANAGER_IP="127.0.0.1"
MANAGER_PORT=50051
BASE_STORAGE_PORT=50052
PID_FILE="gtstore.pids"

while [[ $# -gt 0 ]]; do
    case $1 in
        --nodes)
            NODES="$2"
            shift 2
            ;;
        --rep)
            REP="$2"
            shift 2
            ;;
        --manager-ip)
            MANAGER_IP="$2"
            shift 2
            ;;
        --manager-port)
            MANAGER_PORT="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--nodes N] [--rep K] [--manager-ip IP] [--manager-port PORT]"
            exit 1
            ;;
    esac
done

echo "=== Starting GTStore Service ==="
echo "Configuration:"
echo "  Nodes: $NODES"
echo "  Replication Factor: $REP"
echo "  Manager: $MANAGER_IP:$MANAGER_PORT"
echo ""
rm -f $PID_FILE

#start manager
./bin/manager $MANAGER_IP $MANAGER_PORT $REP &
MANAGER_PID=$!
echo "manager $MANAGER_PID" >> $PID_FILE
sleep 3

#start storages
echo ""
echo "Starting $NODES storage node(s)..."
for ((i=0; i<$NODES; i++)); do
    STORAGE_PORT=$((BASE_STORAGE_PORT + i))
    ./bin/storage $MANAGER_IP $STORAGE_PORT $MANAGER_IP $MANAGER_PORT &
    STORAGE_PID=$!
    echo "storage$i $STORAGE_PID $STORAGE_PORT" >> $PID_FILE
    echo ""
    sleep 2
done

echo ""
echo "=== GTStore Service Started ==="
echo "Process IDs saved to $PID_FILE"
echo ""
echo "To stop the service, run ./stop_service.sh"
echo "To kill a specific storage node, use kill -9 <PID>"
echo ""