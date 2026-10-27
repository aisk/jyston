import sys

from setuptools import Extension, setup

if sys.version_info[:2] != (3, 15):
    raise RuntimeError("jyston currently only supports Python 3.15")

setup(
    name="jyston",
    version="0.1.0",
    description="A JIT for CPython, based on Pyston lite",
    url="https://github.com/aisk/jyston",
    ext_modules=[
        Extension(
            "jyston",
            sources=["src/jyston.c"],
            extra_compile_args=["-std=gnu11", "-Werror=implicit-function-declaration"],
        )
    ],
)
