.DEFAULT_GOAL := env/jyston.stamp

PYTHON?=python3.15

env/venv.stamp:
	$(PYTHON) -m venv env
	./env/bin/pip install -q setuptools
	touch $@

# Installs jyston together with a .pth file that enables it on startup.
# Set JYSTON_DISABLE=1 to turn it off at runtime.
env/jyston.stamp: env/venv.stamp setup.py $(wildcard src/*.c) $(wildcard src/*.h)
	./env/bin/pip install -q --no-build-isolation .
	./env/bin/python -c 'import shutil, site; shutil.copy("autoload/jyston_autoload.pth", site.getsitepackages()[0])'
	touch $@

# Test cases that check CPython internals which jyston changes on purpose:
# - test_stateless requires co_extra to be empty, jyston keeps its state there.
# - test_loop_quicken expects CALL_PY_* specializations, which CPython does not
#   do for functions with a custom vectorcall.
# - CPython emits C_RETURN for every call that is not inlined, including calls
#   to hooked Python functions. cProfile and sys.setprofile filter these out,
#   but these tests count raw sys.monitoring events.
TESTS_TO_IGNORE:=*.test_stateless *.test_loop_quicken *.test_c_return_count \
	*.MonitoringEventsTest.* *.SimulateProfileTest.*
TESTS_TO_SKIP?=

# Run everything with a JIT threshold of 1, so that every function goes through
# the JIT entry path.
test: env/jyston.stamp
	set -ex; for fn in tests/*.py tests/jyston/*.py; do JYSTON_JIT_THRESHOLD=1 ./env/bin/python $$fn; done
	JYSTON_JIT_THRESHOLD=1 ./env/bin/python -m test -j0 $(foreach t,$(TESTS_TO_IGNORE),--ignore '$(t)') $(if $(TESTS_TO_SKIP),-x $(TESTS_TO_SKIP))

clean:
	rm -rf build env *.egg-info

.PHONY: test clean
