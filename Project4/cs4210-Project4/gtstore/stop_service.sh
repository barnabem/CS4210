#!/bin/bash



PID_FILE="gtstore.pids"

if [ ! -f "$PID_FILE" ]; then
    echo "No PID file found. Service may not be running."
    exit 1
fi
echo "=== Stopping GTStore Service ==="
echo ""

while read -r line; do
    NAME=$(echo $line | awk '{print $1}')
    PID=$(echo $line | awk '{print $2}')
    
    if ps -p $PID > /dev/null 2>&1; then
        echo "Stopping $NAME (PID: $PID)..."
        kill -9 $PID 2>/dev/null
        echo "  Stopped."
    else
        echo "$NAME (PID: $PID) not running."
    fi
done < "$PID_FILE"
rm -f "$PID_FILE"

echo ""
echo "=== GTStore Service Stopped ==="