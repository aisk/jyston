# Pyston lite

Pyston lite is the JIT part of [Pyston](https://github.com/pyston/pyston), packaged as an extension module for stock CPython. It supports Python 3.7, 3.8, 3.9 and 3.10, and is compatible with existing binary packages.

This repository only contains the pyston-lite sources, stripped out of the Pyston repository.

## Building

Requirements: gcc, `luajit` (to run DynASM), and the `LuaJIT` submodule.

```
git submodule update --init LuaJIT
PYTHON=python3.8 make
```

This creates a virtualenv in `env/` and installs `pyston_lite` together with `pyston_lite_autoload`, which enables the JIT on interpreter startup. Set `DISABLE_PYSTON=1` to turn it off at runtime.

The build runs PGO by default, plus BOLT on Linux x86_64. BOLT needs `llvm-bolt` and `merge-fdata` in `PATH`. Use `NOPGO=1`, `NOBOLT=1` or `NOLTO=1` to skip the corresponding step.

To check whether pyston-lite has been loaded, run `"pyston_lite" in sys.modules`.

## Testing

```
PYTHON=python3.8 make test
```

This runs the scripts in `tests/` and the CPython regression test suite of the target interpreter.
