.DEFAULT_GOAL := env/lite.stamp

PYTHON?=python3.8

package: build_wheels

ARCH:=$(shell uname -m)
# BOLT needs llvm-bolt and merge-fdata in PATH, set NOBOLT=1 to skip it
ifeq ($(ARCH)$(NOBOLT),x86_64)
	BOLT_DOCKERFLAGS:=-e BOLTFLAGS=1
	BOLT_CMD:=python3 bolt_wheel.py wheelhouse/*.whl
endif

build_wheels:
	git clean -x -i -d -f .
	bash -c 'if [ -d wheelhouse ]; then sudo rm -rf wheelhouse ; fi'
	$(MAKE) build_wheels_noclean

ifeq ($(shell uname), Darwin)
build_wheels_noclean:
	./build_wheels_macos.sh
else
build_wheels_noclean:
	docker run -e PYSTON_LITE_NAME -e NOBOLT=1 $(BOLT_DOCKERFLAGS) -e PLAT=manylinux2014_$(ARCH) -v `pwd`:/io quay.io/pypa/manylinux2014_$(ARCH) bash -c "bash /io/build_wheels.sh && chown -R $(shell id -u):$(shell id -g) /io/"
	$(BOLT_CMD)
endif

upload_wheels: env
	env/bin/pip install twine
	./env/bin/twine upload wheelhouse/*.whl || true

test_packages:
	rm -rf /tmp/env
	$(PYTHON) -m venv /tmp/env
	/tmp/env/bin/pip install wheel
	/tmp/env/bin/pip install --upgrade pyston_lite_autoload
	time /tmp/env/bin/python -c 'for i in range(100000000): pass'
	DISABLE_PYSTON=1 time /tmp/env/bin/python -c 'for i in range(100000000): pass'


######
# The rest of this file is for testing things:

env:
	$(PYTHON) -m venv env
	touch $@

env/lite.stamp: $(wildcard *.c) $(wildcard *.h) | env
	./env/bin/python setup.py install
	cd autoload; ../env/bin/python setup.py install
	touch $@

# test_code has a bug when run with another client that calls RequestCodeExtraIndex
# note: the testsuite doesn't pass for my Ubuntu system cpython, but it does pass
# if I reconfigure this makefile to use a manually-built cpython
#
# A number of other tests in the cpython testsuite fail with the Ubuntu packages, and I'm not sure why.
# This happens regardless of whether pyston-lite is installed, so just exclude them.
ADDITIONAL_TESTS_TO_SKIP?=
ifeq ($(shell uname),Darwin)
# this test errors on macOS (also with stock cpython)
ADDITIONAL_TESTS_TO_SKIP:=$(ADDITIONAL_TESTS_TO_SKIP) test_ctypes
endif
ifeq ($(shell uname -m),arm64) # only triggers on macOS (linux returns aarch64)
# this tests crash on arm64 macOS (also with stock cpython)
ADDITIONAL_TESTS_TO_SKIP:=$(ADDITIONAL_TESTS_TO_SKIP) test_dbm test_dbm_ndbm
endif
test: env/lite.stamp
	set -ex; for fn in tests/*.py; do ./env/bin/python $$fn; done
	./env/bin/python -c 'import test.support; test.support.check_impl_detail = lambda **kw: False; import test.test_code; test.test_code.test_main()'
	./env/bin/python -m test -j0 -x test_code test_distutils test_ensurepip test_minidom test_site test_xml_etree test_xml_etree_c test_capi $(ADDITIONAL_TESTS_TO_SKIP)



ENV:=
# Path to a benchmark script (plus arguments), e.g. from https://github.com/pyston/python-macrobenchmarks
BENCH?=


bench: env/lite.stamp
	$(ENV) ./env/bin/python $(BENCH)

bench_baseline: env/lite.stamp
	DISABLE_PYSTON=1 ./env/bin/python $(BENCH)

perf_bench: env/lite.stamp
	$(ENV) JIT_PERF_MAP=1 perf record -o perf_lite.data ./env/bin/python $(BENCH)
	$(MAKE) perf_report
perf_report:
	perf report -i perf_lite.data -n --objdump=tools/perf_jit.py

perf_bench_baseline: env/lite.stamp
	DISABLE_PYSTON=1 JIT_PERF_MAP=1 perf record -o perf_baseline.data ./env/bin/python $(BENCH)
	$(MAKE) perf_report_baseline
perf_report_baseline:
	perf report -i perf_baseline.data -n --objdump=tools/perf_jit.py

dbg_bench: env/lite.stamp
	DISABLE_PYSTONFULL=1 $(ENV) gdb --args ./env/bin/python $(BENCH) 1000000
