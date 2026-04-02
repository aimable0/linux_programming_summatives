# test_vibration.py
# ===========================================================================
# Test suite for the vibration C extension module.
#
# Run after building:
#   python setup.py build_ext --inplace
#   python test_vibration.py
# ===========================================================================

import vibration
import math

# ── ANSI colour codes for readable terminal output ───────────────────────────
GREEN  = "\033[92m"
RED    = "\033[91m"
CYAN   = "\033[96m"
YELLOW = "\033[93m"
RESET  = "\033[0m"
BOLD   = "\033[1m"

# ── Simple assertion helper ───────────────────────────────────────────────────
passed = 0
failed = 0

def check(label, got, expected, tol=1e-9):
    """
    Assert that `got` is close to `expected` within tolerance `tol`.
    For integer comparisons, pass tol=0.
    Prints a coloured PASS / FAIL line and updates global counters.
    """
    global passed, failed
    if tol == 0:
        ok = (got == expected)
    else:
        ok = abs(got - expected) < tol
    status = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
    print(f"  [{status}] {label}")
    if not ok:
        print(f"         expected: {expected}")
        print(f"         got:      {got}")
        failed += 1
    else:
        passed += 1

def check_raises(label, fn, exc_type):
    """Assert that calling fn() raises exc_type."""
    global passed, failed
    try:
        fn()
        print(f"  [{RED}FAIL{RESET}] {label}  (no exception raised)")
        failed += 1
    except exc_type:
        print(f"  [{GREEN}PASS{RESET}] {label}")
        passed += 1
    except Exception as e:
        print(f"  [{RED}FAIL{RESET}] {label}  (wrong exception: {type(e).__name__}: {e})")
        failed += 1

def section(title):
    print(f"\n{BOLD}{CYAN}{'─'*60}{RESET}")
    print(f"{BOLD}{CYAN}  {title}{RESET}")
    print(f"{BOLD}{CYAN}{'─'*60}{RESET}")


# ============================================================================
# SAMPLE DATA
# ============================================================================
#
# Simulated vibration readings in m/s² from an industrial sensor.
# The values include both positive and negative accelerations (oscillation),
# a clear outlier spike (15.3), and a below-threshold cluster.
#
DATA_LIST  = [1.2, -0.5, 3.7, 2.1, -1.8, 0.9, 4.5, -2.3, 1.6, 15.3]
DATA_TUPLE = tuple(DATA_LIST)           # same data as a tuple
N          = len(DATA_LIST)             # 10 readings

# Pre-computed reference values (Python's math library as ground truth)
REF_MIN    = min(DATA_LIST)             # -2.3
REF_MAX    = max(DATA_LIST)             # 15.3
REF_P2P    = REF_MAX - REF_MIN         # 17.6
REF_MEAN   = sum(DATA_LIST) / N        # 2.47
REF_RMS    = math.sqrt(sum(x*x for x in DATA_LIST) / N)
REF_STDDEV = math.sqrt(
    sum((x - REF_MEAN)**2 for x in DATA_LIST) / (N - 1)
)                                       # sample std dev (Bessel n-1)
REF_ABOVE  = sum(1 for x in DATA_LIST if x > 2.0)  # threshold = 2.0


# ============================================================================
# 1.  peak_to_peak
# ============================================================================
section("1. peak_to_peak(data)")

print(f"  Data:     {DATA_LIST}")
print(f"  min={REF_MIN},  max={REF_MAX},  expected p2p={REF_P2P}")
print()

check("peak_to_peak — list input",
      vibration.peak_to_peak(DATA_LIST), REF_P2P)

check("peak_to_peak — tuple input",
      vibration.peak_to_peak(DATA_TUPLE), REF_P2P)

check("peak_to_peak — single element [5.0]",
      vibration.peak_to_peak([5.0]), 0.0)

check("peak_to_peak — all equal values",
      vibration.peak_to_peak([3.0, 3.0, 3.0]), 0.0)

check("peak_to_peak — negative values only",
      vibration.peak_to_peak([-5.0, -1.0, -3.0]), 4.0)


# ============================================================================
# 2.  rms
# ============================================================================
section("2. rms(data)")

print(f"  Expected RMS = {REF_RMS:.6f}")
print()

check("rms — list input",
      vibration.rms(DATA_LIST), REF_RMS)

check("rms — tuple input",
      vibration.rms(DATA_TUPLE), REF_RMS)

check("rms — all zeros [0, 0, 0]",
      vibration.rms([0.0, 0.0, 0.0]), 0.0)

check("rms — single value [4.0]  (sqrt(16/1) = 4.0)",
      vibration.rms([4.0]), 4.0)

# Known value: rms([3, 4]) = sqrt((9+16)/2) = sqrt(12.5) ≈ 3.535533
check("rms — [3.0, 4.0] = sqrt(12.5)",
      vibration.rms([3.0, 4.0]), math.sqrt(12.5))


# ============================================================================
# 3.  std_dev
# ============================================================================
section("3. std_dev(data)")

print(f"  Expected sample std dev = {REF_STDDEV:.6f}")
print()

check("std_dev — list input",
      vibration.std_dev(DATA_LIST), REF_STDDEV)

check("std_dev — tuple input",
      vibration.std_dev(DATA_TUPLE), REF_STDDEV)

# Numerical stability test: values large, variance tiny.
# One-pass formula would fail here; two-pass should give near-exact result.
BIG = [1_000_000.001, 1_000_000.002, 1_000_000.003]
ref_big = math.sqrt(sum((x - sum(BIG)/3)**2 for x in BIG) / 2)
check("std_dev — numerical stability (large values, tiny variance)",
      vibration.std_dev(BIG), ref_big, tol=1e-6)

check("std_dev — single element → 0.0",
      vibration.std_dev([42.0]), 0.0)

check("std_dev — two elements [2.0, 4.0] = 1.41421...",
      vibration.std_dev([2.0, 4.0]), math.sqrt(2.0))


# ============================================================================
# 4.  above_threshold
# ============================================================================
section("4. above_threshold(data, threshold)")

THRESHOLD = 2.0
print(f"  Threshold = {THRESHOLD}")
print(f"  Values > {THRESHOLD}: {[x for x in DATA_LIST if x > THRESHOLD]}")
print(f"  Expected count = {REF_ABOVE}")
print()

check("above_threshold — list input",
      vibration.above_threshold(DATA_LIST, THRESHOLD), REF_ABOVE, tol=0)

check("above_threshold — tuple input",
      vibration.above_threshold(DATA_TUPLE, THRESHOLD), REF_ABOVE, tol=0)

check("above_threshold — threshold above all values → 0",
      vibration.above_threshold(DATA_LIST, 100.0), 0, tol=0)

check("above_threshold — threshold below all values → n",
      vibration.above_threshold(DATA_LIST, -999.0), N, tol=0)

check("above_threshold — strict (value == threshold not counted)",
      vibration.above_threshold([1.0, 2.0, 3.0], 2.0), 1, tol=0)

check("above_threshold — threshold as int (type coercion)",
      vibration.above_threshold([1.0, 2.0, 3.0], 2), 1, tol=0)


# ============================================================================
# 5.  summary
# ============================================================================
section("5. summary(data)")

s = vibration.summary(DATA_LIST)
print(f"  Returned dict: {s}")
print()

check("summary — count",     s["count"],       N,        tol=0)
check("summary — mean",      s["mean"],        REF_MEAN)
check("summary — min",       s["min"],         REF_MIN)
check("summary — max",       s["max"],         REF_MAX)

# Verify all expected keys are present
check("summary — has all 4 keys",
      set(s.keys()) == {"count", "mean", "min", "max"}, True, tol=0)

# Tuple input gives same result
s2 = vibration.summary(DATA_TUPLE)
check("summary — tuple gives same count",   s2["count"], N,        tol=0)
check("summary — tuple gives same mean",    s2["mean"],  REF_MEAN)


# ============================================================================
# 6.  EDGE CASES — empty input
# ============================================================================
section("6. Edge cases — empty input")

check("peak_to_peak([])  → 0.0",  vibration.peak_to_peak([]),    0.0)
check("rms([])           → 0.0",  vibration.rms([]),             0.0)
check("std_dev([])       → 0.0",  vibration.std_dev([]),         0.0)
check("above_threshold([],1.0)→0", vibration.above_threshold([], 1.0), 0, tol=0)

es = vibration.summary([])
check("summary([]) count → 0",    es["count"], 0,   tol=0)
check("summary([]) mean  → 0.0",  es["mean"],  0.0)
check("summary([]) min   → 0.0",  es["min"],   0.0)
check("summary([]) max   → 0.0",  es["max"],   0.0)


# ============================================================================
# 7.  EDGE CASES — invalid input (type errors)
# ============================================================================
section("7. Edge cases — invalid input (TypeError expected)")

check_raises("non-list string input",
             lambda: vibration.rms("bad input"), TypeError)

check_raises("dict input",
             lambda: vibration.rms({"a": 1}), TypeError)

check_raises("list containing a string element",
             lambda: vibration.rms([1.0, "two", 3.0]), TypeError)

check_raises("above_threshold — missing threshold argument",
             lambda: vibration.above_threshold([1.0, 2.0]), TypeError)

# Direct coercion check (integers in list are valid, coerced to float)
check("int elements coerced to float — peak_to_peak([1,2,3])",
      vibration.peak_to_peak([1, 2, 3]), 2.0)

check("int elements coerced to float — rms([3,4])",
      vibration.rms([3, 4]), math.sqrt(12.5))


# ============================================================================
# 8.  REAL-WORLD SIMULATION
# ============================================================================
section("8. Real-world simulation — high-frequency vibration burst")

import random
random.seed(42)

# 10 000 readings: normal vibration (mean≈0, std≈1.5) + occasional spikes
readings = [random.gauss(0, 1.5) for _ in range(9950)]
readings += [random.uniform(5.0, 10.0) for _ in range(50)]   # 50 spike readings
random.shuffle(readings)

alarm_threshold = 4.5

p2p  = vibration.peak_to_peak(readings)
rms_ = vibration.rms(readings)
std  = vibration.std_dev(readings)
over = vibration.above_threshold(readings, alarm_threshold)
smry = vibration.summary(readings)

print(f"  Readings:          {len(readings):,}")
print(f"  Peak-to-peak:      {p2p:.4f} m/s²")
print(f"  RMS:               {rms_:.4f} m/s²")
print(f"  Std dev:           {std:.4f} m/s²")
print(f"  Above {alarm_threshold} m/s²:     {over} readings")
print(f"  Summary:")
print(f"    count = {smry['count']}")
print(f"    mean  = {smry['mean']:.4f}")
print(f"    min   = {smry['min']:.4f}")
print(f"    max   = {smry['max']:.4f}")

# Sanity checks against Python reference
ref_p2p  = max(readings) - min(readings)
ref_rms  = math.sqrt(sum(x*x for x in readings) / len(readings))
ref_over = sum(1 for x in readings if x > alarm_threshold)

check("simulation peak_to_peak matches Python ref", p2p,  ref_p2p)
check("simulation rms          matches Python ref", rms_, ref_rms)
check("simulation above_thresh matches Python ref", over, ref_over, tol=0)


# ============================================================================
# FINAL SUMMARY
# ============================================================================
section("RESULTS")
total = passed + failed
print(f"\n  {BOLD}Tests passed: {GREEN}{passed}{RESET}{BOLD} / {total}{RESET}")
if failed:
    print(f"  {BOLD}Tests FAILED: {RED}{failed}{RESET}")
else:
    print(f"  {BOLD}{GREEN}All tests passed.{RESET}")
print()

