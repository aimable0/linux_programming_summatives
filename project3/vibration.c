/*
 * vibration.c
 * ===========================================================================
 * Python C Extension Module — Real-Time Vibration Signal Analysis
 *
 * Exposes five functions to Python:
 *   peak_to_peak(data)        → float
 *   rms(data)                 → float
 *   std_dev(data)             → float
 *   above_threshold(data, t)  → int
 *   summary(data)             → dict
 *
 * Build:
 *   python setup.py build_ext --inplace
 *
 * Usage:
 *   import vibration
 *   vibration.rms([1.0, 2.0, 3.0])
 *
 * ===========================================================================
 * MEMORY HANDLING STRATEGY
 * ===========================================================================
 * No heap allocations (malloc/free) are performed inside any function.
 * All computation is done using a fixed set of local C variables declared
 * on the stack (doubles, Py_ssize_t counters).
 *
 * Rationale:
 *   - The input data already lives in the Python list/tuple; we read it
 *     element-by-element via PyList_GET_ITEM / PyTuple_GET_ITEM which
 *     return borrowed references — no copy of the data is needed.
 *   - Each statistical pass (min, max, sum, sum-of-squares) is O(1) in
 *     auxiliary space regardless of input length.
 *   - The only allocation in the entire module is the single PyDict built
 *     in summary(), and that is managed entirely by the Python runtime.
 *
 * This makes the extension safe and efficient under high-frequency
 * real-time conditions where memory pressure matters.
 *
 * ===========================================================================
 * NUMERICAL STABILITY
 * ===========================================================================
 * std_dev() uses a TWO-PASS algorithm:
 *   Pass 1 — compute the mean.
 *   Pass 2 — accumulate sum of squared deviations from the mean.
 *
 * The naive ONE-PASS formula  Var = E[x²] - (E[x])²  is algebraically
 * equivalent but numerically catastrophic when the variance is small
 * relative to the magnitude of the values.  For example, readings of
 * [1000000.001, 1000000.002] have a tiny variance but huge squared sums;
 * floating-point cancellation in the one-pass formula produces large
 * relative errors or even negative variance.
 *
 * The two-pass approach keeps deviations (x_i - mean) small, preserving
 * precision.  For an industrial sensor the readings are often tightly
 * clustered around a baseline, so this matters in practice.
 * ===========================================================================
 */

#define PY_SSIZE_T_CLEAN        /* use Py_ssize_t for all size arguments     */
#include <Python.h>
#include <math.h>               /* sqrt()                                    */


/* ---------------------------------------------------------------------------
 * parse_data_sequence
 * ---------------------------------------------------------------------------
 * Shared helper that validates the first argument of every exported function.
 *
 * Accepts:  a Python list or tuple of floats (or ints, which are coerced)
 * Returns:  1 on success, 0 on failure (Python exception already set)
 *
 * Parameters:
 *   obj   — the raw PyObject* passed by the caller
 *   items — out: number of elements in the sequence
 *   seq   — out: borrowed reference to the validated sequence object
 *           (the caller must NOT Py_DECREF this; it is owned by the caller's
 *            argument tuple)
 *
 * Why no copy?
 *   Both PyList_GET_ITEM and PyTuple_GET_ITEM return borrowed references to
 *   the existing Python float objects.  PyFloat_AsDouble() extracts the C
 *   double without allocating anything.  So validation + extraction are both
 *   allocation-free.
 * --------------------------------------------------------------------------- */
static int
parse_data_sequence(PyObject *obj, Py_ssize_t *items, PyObject **seq)
{
    if (!PyList_Check(obj) && !PyTuple_Check(obj)) {
        PyErr_SetString(PyExc_TypeError,
                        "data must be a list or tuple of floats");
        return 0;
    }
    *seq   = obj;
    *items = PySequence_Fast_GET_SIZE(obj);   /* O(1): uses ob_size directly */
    return 1;
}


/* ---------------------------------------------------------------------------
 * get_item_as_double
 * ---------------------------------------------------------------------------
 * Extracts element i from a list or tuple as a C double.
 * Returns 1 on success, 0 on failure (TypeError set).
 *
 * Using PySequence_Fast_GET_ITEM avoids a second type-dispatch and is safe
 * because parse_data_sequence already confirmed the object is a list/tuple.
 * --------------------------------------------------------------------------- */
static int
get_item_as_double(PyObject *seq, Py_ssize_t i, double *out)
{
    PyObject *item = PySequence_Fast_GET_ITEM(seq, i);  /* borrowed ref */
    double val = PyFloat_AsDouble(item);                /* works for int too */
    if (val == -1.0 && PyErr_Occurred()) {
        PyErr_Format(PyExc_TypeError,
                     "element at index %zd is not a number", i);
        return 0;
    }
    *out = val;
    return 1;
}


/* ===========================================================================
 * vibration_peak_to_peak(data)
 * ===========================================================================
 * Mathematical definition:
 *   peak_to_peak = max(data) - min(data)
 *
 * This represents the total amplitude swing of the vibration signal —
 * a key indicator of mechanical wear or imbalance.
 *
 * Algorithm:
 *   Single pass O(n).  Initialise min and max to the first element, then
 *   scan the remainder updating both accumulators.
 *
 * Time complexity:  O(n)
 * Space complexity: O(1) — two double accumulators on the stack
 *
 * Edge case: empty list → returns 0.0
 * =========================================================================== */
static PyObject *
vibration_peak_to_peak(PyObject *self, PyObject *args)
{
    PyObject *data_obj;

    /* ── 1. Parse Python arguments ──────────────────────────────────────────
     * PyArg_ParseTuple extracts positional arguments from the args tuple.
     * "O" means "one arbitrary Python object" — we do our own type checking
     * below so we can give a precise error message.
     * ── */
    if (!PyArg_ParseTuple(args, "O", &data_obj))
        return NULL;

    PyObject  *seq;
    Py_ssize_t n;
    if (!parse_data_sequence(data_obj, &n, &seq))
        return NULL;

    /* ── 2. Handle empty input ──────────────────────────────────────────── */
    if (n == 0)
        return PyFloat_FromDouble(0.0);

    /* ── 3. Seed min/max with the first element ─────────────────────────── */
    double first;
    if (!get_item_as_double(seq, 0, &first))
        return NULL;

    double vmin = first, vmax = first;

    /* ── 4. Single-pass scan ────────────────────────────────────────────── */
    for (Py_ssize_t i = 1; i < n; i++) {
        double v;
        if (!get_item_as_double(seq, i, &v))
            return NULL;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    /* ── 5. Return Python float ─────────────────────────────────────────── */
    return PyFloat_FromDouble(vmax - vmin);
}


/* ===========================================================================
 * vibration_rms(data)
 * ===========================================================================
 * Mathematical definition:
 *   RMS = sqrt( (1/n) * Σ x_i² )
 *
 * Root Mean Square is the standard measure of the "energy" of a vibration
 * signal.  It corresponds to the effective amplitude perceived by the
 * machinery structure and is directly related to power dissipation.
 *
 * Algorithm:
 *   Single pass — accumulate the sum of squares, then divide and sqrt.
 *
 * Time complexity:  O(n)
 * Space complexity: O(1) — one double accumulator on the stack
 *
 * Numerical note:
 *   Summing x_i² can overflow for very large values.  In practice vibration
 *   readings in m/s² are small enough that this is not a concern; for
 *   production use Kahan summation could be applied to sum_sq.
 *
 * Edge case: empty list → returns 0.0
 * =========================================================================== */
static PyObject *
vibration_rms(PyObject *self, PyObject *args)
{
    PyObject *data_obj;
    if (!PyArg_ParseTuple(args, "O", &data_obj))
        return NULL;

    PyObject  *seq;
    Py_ssize_t n;
    if (!parse_data_sequence(data_obj, &n, &seq))
        return NULL;

    if (n == 0)
        return PyFloat_FromDouble(0.0);

    /* ── Accumulate sum of squares in a single pass ─────────────────────── */
    double sum_sq = 0.0;
    for (Py_ssize_t i = 0; i < n; i++) {
        double v;
        if (!get_item_as_double(seq, i, &v))
            return NULL;
        sum_sq += v * v;
    }

    return PyFloat_FromDouble(sqrt(sum_sq / (double)n));
}


/* ===========================================================================
 * vibration_std_dev(data)
 * ===========================================================================
 * Mathematical definition (sample standard deviation, Bessel-corrected):
 *   mean  = (1/n)   * Σ x_i
 *   var   = (1/n-1) * Σ (x_i - mean)²
 *   std   = sqrt(var)
 *
 * Sample std dev (dividing by n-1) is used because the data represents a
 * sample drawn from a continuous physical process, not the entire population.
 *
 * Algorithm (TWO-PASS — numerically stable):
 *   Pass 1: compute the mean.
 *   Pass 2: accumulate squared deviations from the mean.
 *
 * WHY TWO PASSES? (see full discussion at top of file)
 *   The one-pass formula Var = E[x²] - (E[x])² suffers catastrophic
 *   cancellation when values are large and variance is small.  The two-pass
 *   approach subtracts the mean first so the deviations are near zero,
 *   maintaining full double precision.
 *
 * Time complexity:  O(n) — two linear passes
 * Space complexity: O(1) — mean + sum_dev on the stack; no copy of data
 *
 * Edge cases:
 *   n == 0 → returns 0.0
 *   n == 1 → returns 0.0  (undefined variance; one sample has zero spread)
 * =========================================================================== */
static PyObject *
vibration_std_dev(PyObject *self, PyObject *args)
{
    PyObject *data_obj;
    if (!PyArg_ParseTuple(args, "O", &data_obj))
        return NULL;

    PyObject  *seq;
    Py_ssize_t n;
    if (!parse_data_sequence(data_obj, &n, &seq))
        return NULL;

    if (n <= 1)
        return PyFloat_FromDouble(0.0);

    /* ── Pass 1: compute mean ───────────────────────────────────────────── */
    double sum = 0.0;
    for (Py_ssize_t i = 0; i < n; i++) {
        double v;
        if (!get_item_as_double(seq, i, &v))
            return NULL;
        sum += v;
    }
    double mean = sum / (double)n;

    /* ── Pass 2: accumulate squared deviations from the mean ────────────── */
    double sum_dev = 0.0;
    for (Py_ssize_t i = 0; i < n; i++) {
        double v;
        if (!get_item_as_double(seq, i, &v))
            return NULL;
        double diff = v - mean;
        sum_dev += diff * diff;
    }

    /* Bessel correction: divide by (n - 1) for sample std dev */
    return PyFloat_FromDouble(sqrt(sum_dev / (double)(n - 1)));
}


/* ===========================================================================
 * vibration_above_threshold(data, threshold)
 * ===========================================================================
 * Mathematical definition:
 *   count = |{ x_i : x_i > threshold }|   (strict inequality)
 *
 * Used in condition monitoring to detect how many readings exceed a safety
 * or alarm threshold.  Strict greater-than matches the specification.
 *
 * Python argument parsing:
 *   "Od" — 'O' = arbitrary object (the list/tuple), 'd' = C double.
 *   The 'd' format code converts a Python int or float directly to a C
 *   double without any explicit PyFloat_AsDouble call needed.
 *
 * Time complexity:  O(n)
 * Space complexity: O(1) — one integer counter on the stack
 *
 * Edge case: empty list → returns 0
 * =========================================================================== */
static PyObject *
vibration_above_threshold(PyObject *self, PyObject *args)
{
    PyObject *data_obj;
    double    threshold;

    /* "Od": one Python object + one C double */
    if (!PyArg_ParseTuple(args, "Od", &data_obj, &threshold))
        return NULL;

    PyObject  *seq;
    Py_ssize_t n;
    if (!parse_data_sequence(data_obj, &n, &seq))
        return NULL;

    /* ── Count readings strictly above threshold ────────────────────────── */
    long count = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        double v;
        if (!get_item_as_double(seq, i, &v))
            return NULL;
        if (v > threshold)
            count++;
    }

    /* PyLong_FromLong builds a Python int from a C long */
    return PyLong_FromLong(count);
}


/* ===========================================================================
 * vibration_summary(data)
 * ===========================================================================
 * Returns a Python dict:
 *   {
 *     "count": n          (Python int)
 *     "mean":  mean       (Python float)
 *     "min":   min value  (Python float)
 *     "max":   max value  (Python float)
 *   }
 *
 * Algorithm:
 *   Single pass — simultaneously accumulate sum (for mean), running min,
 *   and running max.  This keeps the time complexity at O(n) while touching
 *   each element exactly once, which is cache-friendly and efficient for
 *   high-frequency sensor data.
 *
 * Memory handling:
 *   The only allocation is the PyDict and its four key/value PyObjects,
 *   all managed by the Python runtime.  PyDict_SetItemString internally
 *   increments the reference count of each value object.  We Py_DECREF
 *   each value after insertion because we no longer need our reference to
 *   it — the dict holds the only remaining reference.
 *
 *   If any PyFloat_FromDouble or PyDict_SetItemString call fails we
 *   Py_DECREF the partially-built dict and return NULL so Python raises
 *   the pending exception cleanly without leaking memory.
 *
 * Time complexity:  O(n)
 * Space complexity: O(1) auxiliary (the output dict has constant size = 4)
 *
 * Edge case: empty list → count=0, mean=0.0, min=0.0, max=0.0
 * =========================================================================== */
static PyObject *
vibration_summary(PyObject *self, PyObject *args)
{
    PyObject *data_obj;
    if (!PyArg_ParseTuple(args, "O", &data_obj))
        return NULL;

    PyObject  *seq;
    Py_ssize_t n;
    if (!parse_data_sequence(data_obj, &n, &seq))
        return NULL;

    /* ── Handle empty input ─────────────────────────────────────────────── */
    if (n == 0) {
        PyObject *d = PyDict_New();
        if (!d) return NULL;
        PyDict_SetItemString(d, "count", PyLong_FromSsize_t(0));
        PyDict_SetItemString(d, "mean",  PyFloat_FromDouble(0.0));
        PyDict_SetItemString(d, "min",   PyFloat_FromDouble(0.0));
        PyDict_SetItemString(d, "max",   PyFloat_FromDouble(0.0));
        return d;
    }

    /* ── Seed accumulators with the first element ───────────────────────── */
    double first;
    if (!get_item_as_double(seq, 0, &first))
        return NULL;

    double sum  = first;
    double vmin = first;
    double vmax = first;

    /* ── Single pass over the remaining elements ────────────────────────── */
    for (Py_ssize_t i = 1; i < n; i++) {
        double v;
        if (!get_item_as_double(seq, i, &v))
            return NULL;
        sum  += v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    double mean = sum / (double)n;

    /* ── Build the result dictionary ────────────────────────────────────── */
    PyObject *result = PyDict_New();
    if (!result) return NULL;

    /* Helper macro: build a value, insert it, release our reference.
     * If building or inserting fails, decref the dict and return NULL.   */
#define SET_FLOAT(key, val)                                         \
    do {                                                            \
        PyObject *_v = PyFloat_FromDouble(val);                     \
        if (!_v || PyDict_SetItemString(result, key, _v) < 0) {    \
            Py_XDECREF(_v);                                         \
            Py_DECREF(result);                                      \
            return NULL;                                            \
        }                                                           \
        Py_DECREF(_v);   /* dict now owns the only reference */     \
    } while (0)

#define SET_INT(key, val)                                           \
    do {                                                            \
        PyObject *_v = PyLong_FromSsize_t(val);                     \
        if (!_v || PyDict_SetItemString(result, key, _v) < 0) {    \
            Py_XDECREF(_v);                                         \
            Py_DECREF(result);                                      \
            return NULL;                                            \
        }                                                           \
        Py_DECREF(_v);                                              \
    } while (0)

    SET_INT  ("count", n);
    SET_FLOAT("mean",  mean);
    SET_FLOAT("min",   vmin);
    SET_FLOAT("max",   vmax);

#undef SET_FLOAT
#undef SET_INT

    return result;   /* caller (Python runtime) owns the reference */
}


/* ===========================================================================
 * Method table
 * ===========================================================================
 * Maps Python-visible function names to C function pointers.
 *
 * METH_VARARGS:
 *   The function receives positional arguments as a tuple (args).
 *   Used for all five functions because they all take positional args only.
 *
 * The table is terminated by a {NULL, NULL, 0, NULL} sentinel so the
 * Python runtime knows where the table ends.
 * =========================================================================== */
static PyMethodDef VibrationMethods[] = {
    {
        "peak_to_peak",
        vibration_peak_to_peak,
        METH_VARARGS,
        "peak_to_peak(data) -> float\n\n"
        "Return the peak-to-peak amplitude (max - min) of the readings.\n"
        "Returns 0.0 for an empty sequence."
    },
    {
        "rms",
        vibration_rms,
        METH_VARARGS,
        "rms(data) -> float\n\n"
        "Return the Root Mean Square of the readings.\n"
        "RMS = sqrt(mean(x_i^2)).\n"
        "Returns 0.0 for an empty sequence."
    },
    {
        "std_dev",
        vibration_std_dev,
        METH_VARARGS,
        "std_dev(data) -> float\n\n"
        "Return the sample standard deviation (Bessel-corrected, n-1).\n"
        "Uses a numerically stable two-pass algorithm.\n"
        "Returns 0.0 for sequences of length 0 or 1."
    },
    {
        "above_threshold",
        vibration_above_threshold,
        METH_VARARGS,
        "above_threshold(data, threshold) -> int\n\n"
        "Return the count of readings strictly greater than threshold."
    },
    {
        "summary",
        vibration_summary,
        METH_VARARGS,
        "summary(data) -> dict\n\n"
        "Return a dict with keys: count, mean, min, max."
    },
    { NULL, NULL, 0, NULL }    /* sentinel — marks end of table */
};


/* ===========================================================================
 * Module definition struct
 * ===========================================================================
 * Describes the module to the Python runtime.
 * PyModuleDef_HEAD_INIT must always be the first field.
 * =========================================================================== */
static struct PyModuleDef vibrationmodule = {
    PyModuleDef_HEAD_INIT,
    "vibration",                /* module name as seen by Python: import vibration  */
    /* module docstring */
    "Real-time vibration signal analysis — Python C extension.\n\n"
    "All computations are performed in C using double precision.\n"
    "Accepts Python lists or tuples of floats as input.",
    -1,                         /* per-interpreter state size; -1 = module is stateless */
    VibrationMethods            /* pointer to the method table above */
};


/* ===========================================================================
 * Module initialisation function
 * ===========================================================================
 * MUST be named  PyInit_<modulename>  (here: PyInit_vibration).
 * Python calls this when `import vibration` is executed.
 * PyModule_Create registers the method table and returns a module object.
 * =========================================================================== */
PyMODINIT_FUNC
PyInit_vibration(void)
{
    return PyModule_Create(&vibrationmodule);
}

