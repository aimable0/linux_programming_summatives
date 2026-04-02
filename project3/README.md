# Project 3 — Python C Extension for Vibration Analysis

## Overview

This project builds a Python extension module named `vibration` from C code.

It provides these functions:

- `peak_to_peak(data)`
- `rms(data)`
- `std_dev(data)`
- `above_threshold(data, threshold)`
- `summary(data)`

Main files:

- `vibration.c` (C extension source)
- `setup.py` (build script)
- `test_vibration.py` (comprehensive test suite)

---

## Prerequisites

- Python 3.10+ (matching your environment)
- C compiler toolchain (e.g., `gcc`)
- Python headers/dev package available in your environment

---

## Build / Compilation

From the `project3` directory:

```bash
python setup.py build_ext --inplace
```

This generates a shared library similar to:

- `vibration.cpython-310-aarch64-linux-gnu.so`

---

## Execution

### 1) Run tests

```bash
python test_vibration.py
```

### 2) Quick interactive usage

```bash
python -c "import vibration; print(vibration.rms([1.0,2.0,3.0]))"
```

---

## Inputs

For module functions:

- `data`: Python `list` or `tuple` of numeric values
- `threshold`: numeric value for `above_threshold`

Example input dataset in tests:

- `[1.2, -0.5, 3.7, 2.1, -1.8, 0.9, 4.5, -2.3, 1.6, 15.3]`

---

## Expected Outputs

### Functional outputs (examples)

- `peak_to_peak([1,2,3])` returns `2.0`
- `rms([3,4])` returns approximately `3.5355339059`
- `std_dev([2.0,4.0])` returns approximately `1.4142135624`
- `above_threshold([1.0,2.0,3.0], 2.0)` returns `1`
- `summary([1.0,2.0,3.0])` returns dictionary with keys:
  - `count`
  - `mean`
  - `min`
  - `max`

### Test script output

`test_vibration.py` prints many PASS/FAIL lines and ends with a summary like:

```text
Tests passed: <passed> / <total>
All tests passed.
```

---

## Error Behavior

Type errors are expected for invalid inputs, for example:

- passing a string instead of list/tuple
- list containing non-numeric values
- missing required threshold argument
