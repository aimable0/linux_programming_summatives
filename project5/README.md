# Project 5 — Digital Library Reservation System (Client/Server)

## Overview

This project implements a TCP client/server reservation system in C.

Components:

- `server.c`: multithreaded reservation server
- `client.c`: interactive command-line client
- `protocol.h`: shared message protocol
- `Makefile`: build automation
- `books.dat`: persisted book reservation state
- `server.log`: server event log

---

## Compilation

From the `project5` directory:

```bash
make
```

Build targets created:

- `server`
- `client`

Optional maintenance commands:

```bash
make clean
make reset
```

---

## Execution

Use two terminals.

### Terminal 1 — start server

```bash
./server
```

### Terminal 2 — start client

```bash
./client
```

Or connect to a different host:

```bash
./client <server_ip>
```

Default server port is `8080`.

---

## Inputs

### Client user inputs

1. Library ID (example valid IDs: `LIB001` ... `LIB010`)
2. Book number to reserve (`1` to `8`)
3. `0` to disconnect and exit

### Server-side data inputs

- `books.dat` is loaded at startup if present
- if absent, the server starts with default catalogue entries

---

## Expected Outputs

### Server

- startup logs and status snapshots
- authentication success/failure logs
- reservation success/denial logs
- timeout or disconnect events
- graceful shutdown messages on `Ctrl+C`

### Client

Typical successful flow:

- connects to server
- authenticates with valid Library ID
- receives book catalogue
- reserves selected book
- receives updated catalogue after each attempt
- exits with disconnect message

Typical failure messages:

- invalid Library ID (`MSG_AUTH_FAIL`)
- server full (`MSG_SERVER_FULL`)
- reservation denied for already reserved book (`MSG_RESERVE_FAIL`)
- inactivity timeout (`MSG_SERVER_INFO`)

---

## Example Session (Conceptual)

1. Start `./server`
2. Start `./client`
3. Enter `LIB003`
4. Enter `1` to reserve book #1
5. Enter `0` to exit

Expected result:

- client sees reservation confirmation
- server updates and persists state in `books.dat`
- next clients see book #1 as reserved
