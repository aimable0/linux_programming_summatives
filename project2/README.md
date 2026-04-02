# Project 2 — Temperature File Processor (x86-64 NASM)

## Overview

This project is a Linux x86-64 assembly program that:

- opens `temperature_data.txt`
- reads the file into memory
- counts total lines (readings)
- counts valid (non-empty) lines
- prints both counts

Source file:

- `temperature_processor.asm`

Input data file:

- `temperature_data.txt`

---

## Compilation

From the `project2` directory:

```bash
nasm -f elf64 temperature_processor.asm -o temperature_processor.o
ld temperature_processor.o -o temperature_processor
```

---

## Execution

Run from inside `project2` so the program can find `temperature_data.txt`:

```bash
./temperature_processor
```

---

## Inputs

The program reads one local text file:

- `temperature_data.txt`

The provided file contains values and empty lines.

---

## Expected Output

With the current `temperature_data.txt`, expected output is:

```text
Total readings: 9
Valid readings: 7
```

### Output Meaning

- `Total readings`: all lines, including empty lines
- `Valid readings`: non-empty lines only

---

## Notes

- File-not-found or open failure prints:

```text
Error: could not open temperature_data.txt
```

- The implementation handles both Unix (`\n`) and Windows (`\r\n`) line endings.
