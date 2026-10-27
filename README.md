# Jyston

Jyston is a fork of [Pyston](https://github.com/pyston/pyston), or more precisely of Pyston lite, the JIT part of Pyston packaged as an extension module for stock CPython.

The goal is a JIT for CPython 3.15, including free-threaded builds, that works on CPython's bytecode and specialized instructions directly and does not depend on CPython's own tier 2 / copy-and-patch JIT. It is at an early stage: the entry hook and deoptimization path are in place, but no machine code is generated yet.

## How it works

Instead of a PEP 523 frame evaluation hook, which disables inlined Python-to-Python calls for the whole interpreter, jyston replaces the vectorcall slot of individual function objects. Calls are counted per code object, and hot code objects are compiled. Compiled code runs on CPython's own `_PyInterpreterFrame`, and deoptimizes by writing its state back into the frame and letting the CPython interpreter continue from that instruction.

## Building

```
PYTHON=python3.15 make
```

This creates a virtualenv in `env/` and installs `jyston` together with a `.pth` file that enables it on interpreter startup. Set `JYSTON_DISABLE=1` to turn it off at runtime, and `JYSTON_JIT_THRESHOLD` to change the number of calls before a function is compiled (default 100). `jyston.stats()` returns some counters.

## Testing

```
PYTHON=python3.15 make test
```

This runs the scripts in `tests/` and the CPython regression test suite with a JIT threshold of 1, so that every function goes through the JIT entry path.

## Legacy Pyston lite

The original Pyston lite for Python 3.8, 3.9 and 3.10 lives in `legacy/`. It still builds with `make` in that directory, and needs `luajit` and the `LuaJIT` submodule.
