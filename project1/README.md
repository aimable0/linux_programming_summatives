# Project 1 — Dynamic Buffer Demo (C)


## Overview

This project contains a small C program that demonstrates:

- dynamic memory allocation with `malloc`
- looping and conditional logic
- searching in a generated character buffer
- use of standard library functions (`printf`, `strlen`)

Source file:

- `program.c`

---


## Compilation

From the `project1` directory:

```bash
gcc -Wall -Wextra -std=c99 -o program program.c
```

This produces the executable `program`.

---


## Execution

Run:

```bash
./program
```

---


## Inputs

There is no user input and no file input.

The program internally:

- allocates a 16-byte buffer
- fills it with letters `A..O` (`"ABCDEFGHIJKLMNO"`)
- searches for character `F`

---


## Expected Output

Typical output:

```text
Buffer  : ABCDEFGHIJKLMNO
Length  : 15
Position: 5
Calls   : 3
```


### Output Meaning

- `Buffer`: generated string
- `Length`: string length reported by `strlen`
- `Position`: index of `F` (0-based)
- `Calls`: number of helper-function calls tracked by the global counter
