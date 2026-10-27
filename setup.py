import os
import platform
import shutil
import subprocess
import sys

from setuptools import Extension, setup

if sys.version_info[:2] != (3, 15):
    raise RuntimeError("jyston currently only supports Python 3.15")

ROOT = os.path.dirname(os.path.abspath(__file__))


def dynasm(src, dst):
    luajit = os.environ.get("LUAJIT") or shutil.which("luajit")
    if luajit is None:
        raise RuntimeError("luajit is required to build jyston, set LUAJIT or add it to PATH")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.check_call([luajit, os.path.join(ROOT, "LuaJIT/dynasm/dynasm.lua"), "-o", dst, src])


sources = ["src/jyston.c", "src/jit_runtime.c"]
if platform.machine().lower() in ("x86_64", "amd64"):
    dynasm("src/jit_x86_64.dasc", "build/gen/jit_x86_64.c")
    sources.append("build/gen/jit_x86_64.c")
else:
    sources.append("src/jit_stub.c")

setup(
    name="jyston",
    version="0.1.0",
    description="A JIT for CPython, based on Pyston lite",
    url="https://github.com/aisk/jyston",
    ext_modules=[
        Extension(
            "jyston",
            sources=sources,
            include_dirs=["src", "LuaJIT"],
            extra_compile_args=["-std=gnu11", "-Werror=implicit-function-declaration"],
        )
    ],
)
