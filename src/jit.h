#ifndef JYSTON_JIT_H
#define JYSTON_JIT_H

#define Py_BUILD_CORE_MODULE 1
#include "Python.h"

#if PY_VERSION_HEX < 0x030F0000 || PY_VERSION_HEX >= 0x03100000
#error "jyston only supports CPython 3.15"
#endif

#include "opcode.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_code.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_function.h"
#include "internal/pycore_instruments.h"
#include "internal/pycore_interpframe.h"
#include "internal/pycore_list.h"
#include "internal/pycore_long.h"
#include "internal/pycore_opcode_metadata.h"
#include "internal/pycore_range.h"
#include "internal/pycore_stackref.h"
#include "internal/pycore_tuple.h"

/* Machine code for a code object. Arguments: the thread state, the frame,
 * the bytecode the frame executes and the value of tstate->eval_breaker that
 * was validated on entry. Returns a new reference, or NULL with an exception
 * set. The frame has been popped when it returns. */
typedef PyObject *(*JyJitFunc)(PyThreadState *, _PyInterpreterFrame *,
                               _Py_CODEUNIT *, uintptr_t);

/* Per code object state, stored in the co_extra slot. */
typedef struct {
    int state;
    Py_ssize_t calls;
    JyJitFunc func;
    void *mem;
    size_t mem_size;
} JyCodeInfo;

/* Compile a code object. Returns 0 and sets info->func on success, returns -1
 * if the code object is not supported. Never sets an exception. */
int jy_jit_compile(PyCodeObject *co, JyCodeInfo *info);
void jy_jit_free(JyCodeInfo *info);

/* Run a frame pushed by _PyEvalFramePushAndInit with jitted code. */
PyObject *jy_run(PyThreadState *tstate, _PyInterpreterFrame *frame, JyJitFunc func);

/* Runtime helpers called from jitted code, see jit_runtime.c. */
void jy_incref(PyObject *obj);
void jy_decref(PyObject *obj);
PyObject *jy_return(PyThreadState *tstate, _PyInterpreterFrame *frame,
                    _PyStackRef retval, int instr);
PyObject *jy_error(PyThreadState *tstate, _PyInterpreterFrame *frame,
                   int instr, int depth);
PyObject *jy_deopt(PyThreadState *tstate, _PyInterpreterFrame *frame,
                   int instr, int depth);
int jy_check_periodic(PyThreadState *tstate, uintptr_t expected);
void jy_qsbr_quiescent(PyThreadState *tstate);
int jy_binary_op(_PyStackRef *slots, int oparg);
int jy_compare_op(_PyStackRef *slots, int oparg);
int jy_to_bool(_PyStackRef *slots);
int jy_load_global(_PyInterpreterFrame *frame, PyObject *name, _PyStackRef *dst);
int jy_load_attr(PyThreadState *tstate, _PyStackRef *slots, PyObject *name, int method);
int jy_store_attr(_PyStackRef *slots, PyObject *name);
int jy_store_subscr(_PyStackRef *slots);
int jy_build_tuple(_PyStackRef *slots, int n);
int jy_build_list(_PyStackRef *slots, int n);
int jy_get_iter(_PyStackRef *slots, int oparg);
int jy_for_iter(PyThreadState *tstate, _PyInterpreterFrame *frame, _PyStackRef *slots);
int jy_call(PyThreadState *tstate, _PyInterpreterFrame *frame, _PyStackRef *slots, int oparg);

/* Statistics shared by jyston.c and the JIT. */
extern struct JyStats {
    Py_ssize_t functions_hooked;
    Py_ssize_t codes_compiled;
    Py_ssize_t codes_rejected;
    Py_ssize_t jit_entries;
    Py_ssize_t deopts;
    Py_ssize_t jit_errors;
} jy_stats;

#define JY_STAT_INC(name) _Py_atomic_add_ssize(&jy_stats.name, 1)

#endif
