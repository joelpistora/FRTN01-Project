#!/bin/bash

PID=$1
OUTFILE="thread_dump_SCHED_OTHER_${PID}.txt"

echo "Thread dump for PID: $PID" > "$OUTFILE"
echo "Generated: $(date)" >> "$OUTFILE"
echo "========================================" >> "$OUTFILE"

for TID in /proc/$PID/task/*; do
    TID_NUM=$(basename $TID)

    echo "" >> "$OUTFILE"
    echo "========================================" >> "$OUTFILE"
    echo "THREAD ID: $TID_NUM" >> "$OUTFILE"
    echo "========================================" >> "$OUTFILE"

    echo "" >> "$OUTFILE"
    echo "[STATUS]" >> "$OUTFILE"
    cat /proc/$PID/task/$TID_NUM/status >> "$OUTFILE" 2>/dev/null

    echo "" >> "$OUTFILE"
    echo "[SCHED]" >> "$OUTFILE"
    cat /proc/$PID/task/$TID_NUM/sched >> "$OUTFILE" 2>/dev/null

done

echo "Done. Output written to $OUTFILE"
