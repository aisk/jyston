"""Tests for the jyston entry hook. Run with JYSTON_JIT_THRESHOLD=1."""

import asyncio
import sys
import threading
import traceback

import jyston

assert jyston.is_enabled(), "jyston should be enabled by the autoload .pth"
assert jyston.stats()["jit_threshold"] == 1, jyston.stats()


def call_twice(f, *args, **kwargs):
    f(*args, **kwargs)
    return f(*args, **kwargs)


# Plain calls, all argument kinds.
def add(a, b=2, *args, c=3, **kwargs):
    return a + b + sum(args) + c + sum(kwargs.values())


before = jyston.stats()["jit_entries"]
assert call_twice(add, 1) == 6
assert add(1, 1) == 5
assert add(1, 1, 1, 1, c=0) == 4
assert add(a=1, b=1, c=1, d=1, e=1) == 5
assert jyston.code_state(add.__code__) == "compiled"
assert jyston.stats()["jit_entries"] > before


def many(a, b, c, d, e, f, g, h, i, j, k=11):
    return [a, b, c, d, e, f, g, h, i, j, k]


assert call_twice(many, *range(10), k=10) == list(range(10)) + [10]


# Recursion and closures.
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


assert fib(20) == 6765


def make_counter():
    count = 0

    def inc():
        nonlocal count
        count += 1
        return count

    return inc


counters = [make_counter() for _ in range(3)]
assert [c() for c in counters for _ in range(2)] == [1, 2, 1, 2, 1, 2]


# Methods and properties.
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm2(self):
        return self.x * self.x + self.y * self.y

    @property
    def sum(self):
        return self.x + self.y


p = Point(3, 4)
assert call_twice(p.norm2) == 25
assert p.sum == 7
assert jyston.code_state(Point.__init__.__code__) == "compiled"


# Generators and coroutines.
def gen(n):
    for i in range(n):
        yield i * 2


assert list(gen(4)) == [0, 2, 4, 6]
assert sum(x for x in range(5)) == 10


async def co(n):
    await asyncio.sleep(0)
    return n + 1


async def agen():
    for i in range(3):
        yield i


async def main():
    return await co(1), [x async for x in agen()]


assert asyncio.run(main()) == (2, [0, 1, 2])


# Exceptions and tracebacks keep the frames of hooked functions.
def inner():
    raise ValueError("boom")


def outer():
    return inner()


try:
    outer()
except ValueError as e:
    names = [f.name for f in traceback.extract_tb(e.__traceback__)]
    assert names[-2:] == ["outer", "inner"], names
else:
    raise AssertionError("no exception")


def caller_name():
    return sys._getframe(1).f_code.co_name


def who_calls():
    return caller_name()


assert who_calls() == "who_calls"


# Deep recursion still raises RecursionError.
def recurse(n):
    return recurse(n + 1)


try:
    recurse(0)
except RecursionError:
    pass
else:
    raise AssertionError("no RecursionError")


# Tracing and monitoring still see jitted functions.
def traced(x):
    y = x + 1
    return y


events = []


def tracer(frame, event, arg):
    if frame.f_code is traced.__code__:
        events.append(event)
    return tracer


sys.settrace(tracer)
try:
    traced(1)
finally:
    sys.settrace(None)
assert events[0] == "call" and events[-1] == "return", events

mon = sys.monitoring
TOOL = mon.PROFILER_ID
mon.use_tool_id(TOOL, "jyston-test")
starts = []
mon.register_callback(TOOL, mon.events.PY_START, lambda code, offset: starts.append(code))
mon.set_events(TOOL, mon.events.PY_START)
try:
    traced(2)
finally:
    mon.set_events(TOOL, 0)
    mon.free_tool_id(TOOL)
assert traced.__code__ in starts


# Replacing __code__ takes effect.
def f1():
    return 1


def f2():
    return 2


assert call_twice(f1) == 1
f1.__code__ = f2.__code__
assert f1() == 2


# Many threads calling the same jitted functions.
def worker(results, idx):
    total = 0
    for i in range(20000):
        total += add(i, 1, c=0)
    results[idx] = total


results = [None] * 8
threads = [threading.Thread(target=worker, args=(results, i)) for i in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()
expected = sum(i + 1 for i in range(20000))
assert results == [expected] * 8, results
if hasattr(sys, "_is_gil_enabled"):
    print("GIL enabled:", sys._is_gil_enabled())


# Disabling restores the default behavior.
def after_disable(x):
    return x * 3


jyston.disable()
entries = jyston.stats()["jit_entries"]
assert call_twice(after_disable, 2) == 6
assert jyston.stats()["jit_entries"] == entries
jyston.enable()

print("ok", jyston.stats())
