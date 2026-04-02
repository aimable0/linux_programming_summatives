# Project 4 — Airport Baggage Handling (Pthreads)

## Overview

This project simulates an airport baggage system using multithreading.

Threads used:

- Producer: loads luggage to conveyor belt
- Consumer: dispatches luggage from conveyor to aircraft
- Monitor: periodically prints system statistics

Source file:

- `airport_baggage.c`

---

## Compilation

From the `project4` directory:

```bash
gcc -Wall -Wextra -pthread -o airport_baggage airport_baggage.c
```

---

## Execution

Run:

```bash
./airport_baggage
```

(You may also have an existing binary `airport_baggage_v1`; compiling creates `airport_baggage`.)

---

## Inputs

No keyboard or file input is required.

Simulation parameters are constants in source:

- Belt capacity: 5
- Total luggage target: 10
- Producer interval: 2 seconds
- Consumer interval: 4 seconds
- Monitor interval: 5 seconds

You can stop early with `Ctrl+C` (graceful shutdown).

---

## Expected Output

The program prints:

- startup banner with configured timings
- timestamped producer/consumer events
- periodic monitor reports
- final statistics block when finished

Typical final summary:

```text
FINAL STATISTICS
Total items loaded onto belt : 10
Total items dispatched       : 10
Items remaining on belt      : 0
```

Exact timestamps and event order can vary due to scheduling.
