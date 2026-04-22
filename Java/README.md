# ThreadMappingInvestigation

This program creates five Java platform threads with different Java priorities so you can observe their mapping to Linux kernel threads (visible with `top -H` or `ps -eLf`).

Compile and run from the workspace root:

```bash
javac Java/ThreadMappingInvestigation.java
java -cp Java ThreadMappingInvestigation
```

Notes:
- The program prints the main PID at startup to help locate the process in `top` or `ps`.
- Each worker thread prints a start message and periodic heartbeats so you can correlate Java thread names/IDs with native threads.
- Use `ps -eLf | grep <PID>` or `top -p <PID> -H` to view native threads (look for LWP/TID columns).
