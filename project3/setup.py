# setup.py
# ===========================================================================
# Build script for the vibration C extension module.
#
# Uses setuptools (preferred over the deprecated distutils).
# setuptools.Extension tells the build system:
#   - the module name as Python will import it  ("vibration")
#   - the list of C source files                (["vibration.c"])
#
# Build command (run from the directory containing this file):
#   python setup.py build_ext --inplace
#
# The --inplace flag places the compiled shared library (.so on Linux/macOS,
# .pyd on Windows) in the current directory so that test_vibration.py can
# import it without installing it system-wide.
#
# After a successful build you will see a file such as:
#   vibration.cpython-311-aarch64-linux-gnu.so   (name reflects Python version + arch)
#
# Alternatively, with pip (recommended for modern Python):
#   pip install --no-build-isolation -e .
# ===========================================================================

from setuptools import setup, Extension

vibration_module = Extension(
    name="vibration",           # Python import name: `import vibration`
    sources=["vibration.c"],    # C source files to compile

    # Compiler flags:
    #   -O2          : optimise for speed — important for real-time analytics
    #   -Wall        : enable all warnings during development
    #   -std=c99     : use C99 for // comments, declare-anywhere variables,
    #                  stdint types, and C99 math behaviour
    #   -ffast-math  : allow re-association of FP ops for speed; acceptable
    #                  because we do not rely on strict IEEE 754 edge cases
    extra_compile_args=["-O2", "-Wall", "-std=c99", "-ffast-math"],
)

setup(
    name="vibration",
    version="1.0.0",
    description="Real-time vibration signal analysis — Python C extension",
    author="Student",
    ext_modules=[vibration_module],
    # Link against the math library for sqrt()
    # (on most Linux systems libm is already linked via Python itself,
    #  but listing it explicitly is safe and portable)
)

