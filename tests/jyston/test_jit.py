"""Tests for the generated machine code. Run with JYSTON_JIT_THRESHOLD=1."""

import platform
import sys
import threading
import traceback

import jyston

JIT = platform.machine().lower() in ("x86_64", "amd64")


def check_compiled(f):
    if JIT:
        assert jyston.code_state(f.__code__) == "compiled", (f, jyston.code_state(f.__code__))


# Integer arithmetic and comparisons, including the boundaries of the inline
# fast paths for compact ints.
def arith(a, b):
    return [a + b, a - b, a * b, a // b, a % b, a < b, a <= b, a == b, a != b, a > b, a >= b]


def ref_arith(a, b):
    return [a + b, a - b, a * b, a // b, a % b, a < b, a <= b, a == b, a != b, a > b, a >= b]


jyston.disable()
values = [0, 1, -1, 7, -7, 255, 256, 257, -5, -6, 2**30 - 1, 2**30, -(2**30) + 1, -(2**30),
          2**62, -(2**62), 2**100, 3.5, -0.0, True, False]
expected = {(a, b): ref_arith(a, b) for a in values for b in values if b}
jyston.enable()
for a in values:
    for b in values:
        if b:
            assert arith(a, b) == expected[a, b], (a, b, arith(a, b), expected[a, b])
check_compiled(arith)


# Operations on other types go through the generic paths.
try:
    arith("ab", 2)
except TypeError:
    pass
else:
    raise AssertionError("no TypeError")


# Loops.
def loops(n):
    total = 0
    for i in range(n):
        if i % 3 == 0:
            continue
        total += i
    j = 0
    while j < n:
        j += 2
    for x in [1, 2, 3]:
        total += x
    for x in (4, 5):
        total -= x
    for c in "abc":
        total += len(c)
    return total, j


assert loops(10) == (sum(i for i in range(10) if i % 3), 10)
assert loops(0) == (0, 0)
check_compiled(loops)


def range_step(start, stop, step):
    out = []
    for i in range(start, stop, step):
        out.append(i)
    return out


assert range_step(10, -10, -3) == list(range(10, -10, -3))
assert range_step(2**62, 2**62 + 10, 4) == list(range(2**62, 2**62 + 10, 4))


# Attributes, globals, subscripts and building containers.
class Obj:
    pass


G = 10


def objects(o, d):
    o.a = G
    o.b = o.a + 1
    d["k"] = (o.a, o.b)
    t = (o.a, [o.b, G])
    return d["k"], t


assert objects(Obj(), {}) == ((10, 11), (10, [11, 10]))
check_compiled(objects)


# Truthiness of arbitrary objects.
def truth(x):
    if x:
        return 1
    return 0


assert [truth(v) for v in (0, 1, "", "a", [], [0], None, Obj())] == [0, 1, 0, 1, 0, 1, 0, 1]


def is_none(x):
    if x is None:
        return "none"
    if x is not None:
        return "some"


assert is_none(None) == "none" and is_none(0) == "some"


# Errors raised in jitted code have the right line numbers, and the value
# stack is cleaned up.
def raises(d, k):
    a = [1, 2, 3]
    return a, d[k]


def call_raises():
    return raises({}, "missing")


for _ in range(3):
    try:
        call_raises()
    except KeyError as e:
        tb = traceback.extract_tb(e.__traceback__)
        assert [f.name for f in tb[-2:]] == ["call_raises", "raises"], tb
        assert tb[-1].lineno == raises.__code__.co_firstlineno + 2, tb[-1].lineno
    else:
        raise AssertionError("no KeyError")


def raise_in_loop(n):
    for i in range(n):
        if i == 5:
            i + "x"
    return n


try:
    raise_in_loop(10)
except TypeError as e:
    assert traceback.extract_tb(e.__traceback__)[-1].lineno == raise_in_loop.__code__.co_firstlineno + 3
else:
    raise AssertionError("no TypeError")


# Unbound locals deopt to the interpreter, which raises.
def unbound(flag):
    if flag:
        x = 1
    for _ in range(2):
        pass
    return x


assert unbound(True) == 1
try:
    unbound(False)
except UnboundLocalError:
    pass
else:
    raise AssertionError("no UnboundLocalError")


# Enabling tracing while a jitted loop runs switches to the interpreter at the
# next backward jump.
def traced_loop(n, start_tracing):
    total = 0
    for i in range(n):
        if i == 3:
            start_tracing()
        total += i
    return total


lines = []


def tracer(frame, event, arg):
    if frame.f_code is traced_loop.__code__ and event == "line":
        lines.append(frame.f_lineno)
    return tracer


def start_tracing():
    sys._getframe(1).f_trace = tracer
    sys.settrace(tracer)


try:
    assert traced_loop(6, start_tracing) == 15
finally:
    sys.settrace(None)
assert lines, "no line events after tracing was enabled"


# Destructors running in the middle of jitted code see a consistent frame.
class Del:
    def __del__(self):
        frame = sys._getframe(1)
        seen.append(frame.f_code.co_name)


seen = []


def drop():
    x = Del()
    x = None
    return x


assert drop() is None and seen == ["drop"], seen


# Deep recursion through jitted code.
def depth(n):
    if n == 0:
        return 0
    return depth(n - 1) + 1


assert depth(500) == 500
try:
    depth(10**6)
except RecursionError:
    pass
else:
    raise AssertionError("no RecursionError")


# Many threads running the same jitted loops.
def worker(results, idx):
    results[idx] = loops(10000)


results = [None] * 8
threads = [threading.Thread(target=worker, args=(results, i)) for i in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()
assert results == [loops(10000)] * 8

print("ok", jyston.stats())
