/* Runtime support for jitted code.
 *
 * Jitted code runs a frame that was pushed by jit_entry() in jyston.c. It does
 * not push an entry frame like _PyEval_EvalFrameDefault does, the frame is
 * linked to tstate->current_frame directly, the same way the interpreter
 * links inlined calls. On every exit (return, error or deopt) the frame is
 * unlinked again.
 *
 * Helpers that take a slots pointer operate on the value stack of the frame.
 * Unless noted otherwise they consume their inputs, also on error, so that
 * the caller knows the stack depth that is left for unwinding.
 */

#include "jit.h"

struct JyStats jy_stats;

/* From Python/ceval_macros.h, which is not installed. */
static inline int
enter_recursive_py(PyThreadState *tstate)
{
    return (tstate->py_recursion_remaining-- <= 0) &&
        _Py_CheckRecursiveCallPy(tstate);
}

static inline void
leave_recursive_py(PyThreadState *tstate)
{
    tstate->py_recursion_remaining++;
}

static inline uintptr_t
load_eval_breaker(PyThreadState *tstate)
{
    return _Py_atomic_load_uintptr_relaxed(&tstate->eval_breaker);
}

static int
code_is_instrumented(PyCodeObject *co)
{
    _PyCoMonitoringData *monitoring = co->_co_monitoring;
    if (monitoring == NULL) {
        return 0;
    }
    for (int i = 0; i < (int)Py_ARRAY_LENGTH(monitoring->active_monitors.tools); i++) {
        if (monitoring->active_monitors.tools[i]) {
            return 1;
        }
    }
    return 0;
}

/* Unlink the frame and give it to the CPython interpreter. */
static PyObject *
leave_and_interpret(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    leave_recursive_py(tstate);
    tstate->current_frame = frame->previous;
    _Py_LeaveRecursiveCallTstate(tstate);
    JY_STAT_INC(deopts);
    return _PyEval_EvalFrame(tstate, frame, 0);
}

static PyObject *
leave_and_pop(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    leave_recursive_py(tstate);
    tstate->current_frame = frame->previous;
    _PyEval_FrameClearAndPop(tstate, frame);
    _Py_LeaveRecursiveCallTstate(tstate);
    return NULL;
}

/* Run a pushed frame with jitted code, mirroring the prologue of
 * _PyEval_EvalFrameDefault. Falls back to the interpreter if the code object
 * is being monitored or the interpreter has pending work. */
PyObject *
jy_run(PyThreadState *tstate, _PyInterpreterFrame *frame, JyJitFunc func)
{
    PyCodeObject *co = _PyFrame_GetCode(frame);
#ifdef Py_GIL_DISABLED
    if (frame->tlbc_index != ((_PyThreadStateImpl *)tstate)->tlbc_index ||
            _PyCode_GetTLBCFast(tstate, co) == NULL) {
        JY_STAT_INC(deopts);
        return _PyEval_EvalFrame(tstate, frame, 0);
    }
    uintptr_t version = _Py_atomic_load_uintptr_acquire(&co->_co_instrumentation_version);
#else
    uintptr_t version = co->_co_instrumentation_version;
#endif
    uintptr_t eval_breaker = load_eval_breaker(tstate);
    if (eval_breaker != version || code_is_instrumented(co)) {
        JY_STAT_INC(deopts);
        return _PyEval_EvalFrame(tstate, frame, 0);
    }
    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }
    frame->previous = tstate->current_frame;
    tstate->current_frame = frame;
    if (enter_recursive_py(tstate)) {
        return leave_and_pop(tstate, frame);
    }
    JY_STAT_INC(jit_entries);
    return func(tstate, frame, _PyFrame_GetBytecode(frame), eval_breaker);
}

void
jy_incref(PyObject *obj)
{
    Py_INCREF(obj);
}

void
jy_decref(PyObject *obj)
{
    Py_DECREF(obj);
}

PyObject *
jy_return(PyThreadState *tstate, _PyInterpreterFrame *frame,
          _PyStackRef retval, int instr)
{
    retval = PyStackRef_MakeHeapSafe(retval);
    frame->instr_ptr = _PyFrame_GetBytecode(frame) + instr;
    frame->stackpointer = _PyFrame_Stackbase(frame);
    leave_and_pop(tstate, frame);
    return PyStackRef_AsPyObjectSteal(retval);
}

/* The equivalent of the error and exception_unwind labels of the interpreter
 * for code objects without exception handlers. */
PyObject *
jy_error(PyThreadState *tstate, _PyInterpreterFrame *frame, int instr, int depth)
{
    JY_STAT_INC(jit_errors);
    frame->instr_ptr = _PyFrame_GetBytecode(frame) + instr;
    frame->stackpointer = _PyFrame_Stackbase(frame) + depth;
    if (!_PyErr_Occurred(tstate)) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                         "error return without exception set");
    }
    if (!_PyFrame_IsIncomplete(frame)) {
        PyFrameObject *f = _PyFrame_GetFrameObject(frame);
        if (f != NULL) {
            PyTraceBack_Here(f);
        }
    }
    _PyEval_MonitorRaise(tstate, frame, frame->instr_ptr);
    _PyStackRef *stackbase = _PyFrame_Stackbase(frame);
    while (frame->stackpointer > stackbase) {
        _PyStackRef ref = *--frame->stackpointer;
        PyStackRef_XCLOSE(ref);
    }
    return leave_and_pop(tstate, frame);
}

/* Continue executing the frame in the interpreter, starting at instr. */
PyObject *
jy_deopt(PyThreadState *tstate, _PyInterpreterFrame *frame, int instr, int depth)
{
    frame->instr_ptr = _PyFrame_GetBytecode(frame) + instr;
    frame->stackpointer = _PyFrame_Stackbase(frame) + depth;
    return leave_and_interpret(tstate, frame);
}

/* Called on a backward jump when tstate->eval_breaker differs from the value
 * validated on entry. Returns 0 to continue, -1 on error and 1 if the code
 * has to continue in the interpreter, e.g. because monitoring was enabled. */
int
jy_check_periodic(PyThreadState *tstate, uintptr_t expected)
{
    uintptr_t eval_breaker = load_eval_breaker(tstate);
    if (eval_breaker & _PY_EVAL_EVENTS_MASK) {
        if (_Py_HandlePending(tstate) != 0) {
            return -1;
        }
        eval_breaker = load_eval_breaker(tstate);
    }
    return (eval_breaker & ~_PY_EVAL_EVENTS_MASK) != expected;
}

void
jy_qsbr_quiescent(PyThreadState *tstate)
{
#ifdef Py_GIL_DISABLED
    _Py_qsbr_quiescent_state(((_PyThreadStateImpl *)tstate)->qsbr);
#else
    (void)tstate;
#endif
}

/* slots: lhs, rhs -> res */
int
jy_binary_op(_PyStackRef *slots, int oparg)
{
    _PyStackRef lhs = slots[0];
    _PyStackRef rhs = slots[1];
    PyObject *lhs_o = PyStackRef_AsPyObjectBorrow(lhs);
    PyObject *rhs_o = PyStackRef_AsPyObjectBorrow(rhs);
    _PyStackRef res = PyStackRef_NULL;
    if (PyLong_CheckExact(lhs_o) && PyLong_CheckExact(rhs_o) &&
            _PyLong_BothAreCompact((PyLongObject *)lhs_o, (PyLongObject *)rhs_o)) {
        switch (oparg) {
            case NB_ADD:
            case NB_INPLACE_ADD:
                res = _PyCompactLong_Add((PyLongObject *)lhs_o, (PyLongObject *)rhs_o);
                break;
            case NB_SUBTRACT:
            case NB_INPLACE_SUBTRACT:
                res = _PyCompactLong_Subtract((PyLongObject *)lhs_o, (PyLongObject *)rhs_o);
                break;
            case NB_MULTIPLY:
            case NB_INPLACE_MULTIPLY:
                res = _PyCompactLong_Multiply((PyLongObject *)lhs_o, (PyLongObject *)rhs_o);
                break;
        }
    }
    if (PyStackRef_IsNull(res)) {
        PyObject *res_o = _PyEval_BinaryOps[oparg](lhs_o, rhs_o);
        if (res_o != NULL) {
            res = PyStackRef_FromPyObjectSteal(res_o);
        }
    }
    slots[0] = res;
    slots[1] = PyStackRef_NULL;
    PyStackRef_CLOSE(rhs);
    PyStackRef_CLOSE(lhs);
    return PyStackRef_IsNull(res) ? -1 : 0;
}

/* slots: left, right -> res */
int
jy_compare_op(_PyStackRef *slots, int oparg)
{
    _PyStackRef left = slots[0];
    _PyStackRef right = slots[1];
    PyObject *left_o = PyStackRef_AsPyObjectBorrow(left);
    PyObject *right_o = PyStackRef_AsPyObjectBorrow(right);
    if (_PyLong_CheckExactAndCompact(left_o) && _PyLong_CheckExactAndCompact(right_o)) {
        /* Same as COMPARE_OP_INT, the low bits of oparg are a mask of the
         * outcomes that are true. */
        Py_ssize_t ileft = _PyLong_CompactValue((PyLongObject *)left_o);
        Py_ssize_t iright = _PyLong_CompactValue((PyLongObject *)right_o);
        slots[0] = (COMPARISON_BIT(ileft, iright) & oparg) ? PyStackRef_True : PyStackRef_False;
        slots[1] = PyStackRef_NULL;
        PyStackRef_CLOSE(right);
        PyStackRef_CLOSE(left);
        return 0;
    }
    PyObject *res_o = PyObject_RichCompare(left_o, right_o, oparg >> 5);
    slots[0] = PyStackRef_NULL;
    slots[1] = PyStackRef_NULL;
    PyStackRef_CLOSE(right);
    PyStackRef_CLOSE(left);
    if (res_o == NULL) {
        return -1;
    }
    if (oparg & 16) {
        int res_bool = PyObject_IsTrue(res_o);
        Py_DECREF(res_o);
        if (res_bool < 0) {
            return -1;
        }
        slots[0] = res_bool ? PyStackRef_True : PyStackRef_False;
    }
    else {
        slots[0] = PyStackRef_FromPyObjectSteal(res_o);
    }
    return 0;
}

/* slots: value -> bool */
int
jy_to_bool(_PyStackRef *slots)
{
    _PyStackRef value = slots[0];
    int err = PyObject_IsTrue(PyStackRef_AsPyObjectBorrow(value));
    slots[0] = PyStackRef_NULL;
    PyStackRef_CLOSE(value);
    if (err < 0) {
        return -1;
    }
    slots[0] = err ? PyStackRef_True : PyStackRef_False;
    return 0;
}

/* Does not consume anything, dst is left NULL on error. */
int
jy_load_global(_PyInterpreterFrame *frame, PyObject *name, _PyStackRef *dst)
{
    _PyEval_LoadGlobalStackRef(frame->f_globals, frame->f_builtins, name, dst);
    return PyStackRef_IsNull(*dst) ? -1 : 0;
}

/* slots: owner -> attr, or owner -> attr, self_or_null if method is set */
int
jy_load_attr(PyThreadState *tstate, _PyStackRef *slots, PyObject *name, int method)
{
    _PyStackRef owner = slots[0];
    _PyStackRef attr;
    if (method) {
        attr = _Py_LoadAttr_StackRefSteal(tstate, owner, name, &slots[1]);
    }
    else {
        attr = _PyObject_GetAttrStackRef(PyStackRef_AsPyObjectBorrow(owner), name);
        slots[0] = PyStackRef_NULL;
        PyStackRef_CLOSE(owner);
    }
    slots[0] = attr;
    return PyStackRef_IsNull(attr) ? -1 : 0;
}

/* slots: v, owner -> */
int
jy_store_attr(_PyStackRef *slots, PyObject *name)
{
    _PyStackRef v = slots[0];
    _PyStackRef owner = slots[1];
    int err = PyObject_SetAttr(PyStackRef_AsPyObjectBorrow(owner), name,
                               PyStackRef_AsPyObjectBorrow(v));
    slots[1] = PyStackRef_NULL;
    PyStackRef_CLOSE(owner);
    slots[0] = PyStackRef_NULL;
    PyStackRef_CLOSE(v);
    return err ? -1 : 0;
}

/* slots: v, container, sub -> */
int
jy_store_subscr(_PyStackRef *slots)
{
    _PyStackRef v = slots[0];
    _PyStackRef container = slots[1];
    _PyStackRef sub = slots[2];
    int err = PyObject_SetItem(PyStackRef_AsPyObjectBorrow(container),
                               PyStackRef_AsPyObjectBorrow(sub),
                               PyStackRef_AsPyObjectBorrow(v));
    slots[2] = PyStackRef_NULL;
    PyStackRef_CLOSE(sub);
    slots[1] = PyStackRef_NULL;
    PyStackRef_CLOSE(container);
    slots[0] = PyStackRef_NULL;
    PyStackRef_CLOSE(v);
    return err ? -1 : 0;
}

/* slots: values[n] -> tuple. Does not consume the values on error. */
int
jy_build_tuple(_PyStackRef *slots, int n)
{
    PyObject *tup = _PyTuple_FromStackRefStealOnSuccess(slots, n);
    if (tup == NULL) {
        return -1;
    }
    slots[0] = PyStackRef_FromPyObjectSteal(tup);
    return 0;
}

/* slots: values[n] -> list. Does not consume the values on error. */
int
jy_build_list(_PyStackRef *slots, int n)
{
    PyObject *list = _PyList_FromStackRefStealOnSuccess(slots, n);
    if (list == NULL) {
        return -1;
    }
    slots[0] = PyStackRef_FromPyObjectSteal(list);
    return 0;
}

/* slots: iterable -> iter, index_or_null */
int
jy_get_iter(_PyStackRef *slots, int oparg)
{
    _PyStackRef index_or_null;
    _PyStackRef iter = _PyEval_GetIter(slots[0], &index_or_null, oparg);
    if (PyStackRef_IsError(iter)) {
        slots[0] = PyStackRef_NULL;
        return -1;
    }
    slots[0] = iter;
    slots[1] = index_or_null;
    return 0;
}

/* slots: iter, null_or_index -> iter, null_or_index, next
 * Returns 1 if an item was pushed, 0 if the iterator is exhausted and -1 on
 * error, in which case iter and null_or_index are left on the stack. */
int
jy_for_iter(PyThreadState *tstate, _PyInterpreterFrame *frame, _PyStackRef *slots)
{
    PyObject *iter_o = PyStackRef_AsPyObjectBorrow(slots[0]);
    if (Py_TYPE(iter_o) == &PyRangeIter_Type
#ifdef Py_GIL_DISABLED
            && _PyObject_IsUniquelyReferenced(iter_o)
#endif
            ) {
        /* Same as FOR_ITER_RANGE. */
        _PyRangeIterObject *r = (_PyRangeIterObject *)iter_o;
        if (r->len <= 0) {
            return 0;
        }
        long value = r->start;
        r->start = value + r->step;
        r->len--;
        PyObject *res = PyLong_FromLong(value);
        if (res == NULL) {
            return -1;
        }
        slots[2] = PyStackRef_FromPyObjectSteal(res);
        return 1;
    }
    _PyStackRef null_or_index = slots[1];
    _PyStackRef item = _PyForIter_VirtualIteratorNext(tstate, frame, slots[0],
                                                      &null_or_index);
    slots[1] = null_or_index;
    if (!PyStackRef_IsValid(item)) {
        return PyStackRef_IsError(item) ? -1 : 0;
    }
    slots[2] = item;
    return 1;
}

/* slots: callable, self_or_null, args[oparg] -> res */
int
jy_call(PyThreadState *tstate, _PyInterpreterFrame *frame, _PyStackRef *slots, int oparg)
{
    _PyStackRef callable = slots[0];
    _PyStackRef self_or_null = slots[1];
    if (PyStackRef_TYPE(callable) == &PyMethod_Type && PyStackRef_IsNull(self_or_null)) {
        PyMethodObject *method = (PyMethodObject *)PyStackRef_AsPyObjectBorrow(callable);
        self_or_null = PyStackRef_FromPyObjectNew(method->im_self);
        slots[1] = self_or_null;
        slots[0] = PyStackRef_FromPyObjectNew(method->im_func);
        PyStackRef_CLOSE(callable);
        callable = slots[0];
    }
    _PyStackRef *arguments = slots + 2;
    int total_args = oparg;
    if (!PyStackRef_IsNull(self_or_null)) {
        arguments--;
        total_args++;
    }
    PyObject *res = _Py_VectorCallInstrumentation_StackRefSteal(
        callable, arguments, total_args, PyStackRef_NULL, false,
        frame, frame->instr_ptr, tstate);
    if (res == NULL) {
        slots[0] = PyStackRef_NULL;
        return -1;
    }
    slots[0] = PyStackRef_FromPyObjectSteal(res);
    if (load_eval_breaker(tstate) & _PY_EVAL_EVENTS_MASK) {
        if (_Py_HandlePending(tstate) != 0) {
            _PyStackRef tmp = slots[0];
            slots[0] = PyStackRef_NULL;
            PyStackRef_CLOSE(tmp);
            return -1;
        }
    }
    return 0;
}
