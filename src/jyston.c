/* Jyston: a JIT for CPython 3.15.
 *
 * Entry hook: instead of a PEP 523 eval_frame hook (which disables inlined
 * Python-to-Python calls for the whole interpreter), jyston replaces the
 * vectorcall slot of individual function objects. A function watcher installs
 * jy_vectorcall on every function created after enable(). Calls are counted
 * per code object; once a code object gets hot it is compiled, and its calls
 * are executed through jit_entry() from then on. Functions whose code is
 * rejected get the default vectorcall back, so CPython can specialize and
 * inline calls to them as usual.
 *
 * Deoptimization: JIT code writes the value stack and instr_ptr back into the
 * _PyInterpreterFrame and hands the frame to the CPython interpreter, which
 * resumes execution at that instruction.
 *
 * There is no code generation yet: every compiled function deopts at its first
 * instruction.
 */

#define Py_BUILD_CORE_MODULE 1
#include "Python.h"

#if PY_VERSION_HEX < 0x030F0000 || PY_VERSION_HEX >= 0x03100000
#error "jyston only supports CPython 3.15"
#endif

#include "internal/pycore_ceval.h"
#include "internal/pycore_code.h"
#include "internal/pycore_function.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_interpframe.h"
#include "internal/pycore_stackref.h"

#define DEFAULT_JIT_THRESHOLD 100

enum {
    JY_WARMING = 0,
    JY_COMPILED = 1,
    JY_REJECTED = 2,
};

/* Per code object state, stored in the co_extra slot. */
typedef struct {
    int state;
    Py_ssize_t calls;
} JyCodeInfo;

static struct {
    Py_ssize_t functions_hooked;
    Py_ssize_t codes_compiled;
    Py_ssize_t codes_rejected;
    Py_ssize_t jit_entries;
    Py_ssize_t deopts;
} stats;

#define STAT_INC(name) _Py_atomic_add_ssize(&stats.name, 1)

static int jy_enabled = 0;
static int initialized = 0;
static int func_watcher_id = -1;
static Py_ssize_t code_extra_index = -1;
static Py_ssize_t jit_threshold = DEFAULT_JIT_THRESHOLD;
static PyMutex info_lock;
static PyMutex compile_lock;

static PyObject *jy_vectorcall(PyObject *callable, PyObject *const *args,
                               size_t nargsf, PyObject *kwnames);

static void
free_code_info(void *info)
{
    PyMem_RawFree(info);
}

/* Returns a borrowed pointer, or NULL (with or without an exception set) if
 * there is no info and create is false, or if the allocation failed. */
static JyCodeInfo *
get_code_info(PyCodeObject *co, int create)
{
    void *extra = NULL;
    if (PyUnstable_Code_GetExtra((PyObject *)co, code_extra_index, &extra) < 0) {
        return NULL;
    }
    if (extra != NULL || !create) {
        return extra;
    }
    PyMutex_Lock(&info_lock);
    if (PyUnstable_Code_GetExtra((PyObject *)co, code_extra_index, &extra) == 0
            && extra == NULL) {
        JyCodeInfo *info = PyMem_RawCalloc(1, sizeof(JyCodeInfo));
        if (info != NULL) {
            if (PyUnstable_Code_SetExtra((PyObject *)co, code_extra_index, info) < 0) {
                PyMem_RawFree(info);
                info = NULL;
            }
        }
        extra = info;
    }
    PyMutex_Unlock(&info_lock);
    return extra;
}

static void
restore_default_vectorcall(PyFunctionObject *func)
{
    /* Write the slot directly instead of using PyFunction_SetVectorcall, which
     * would also clear func_version and prevent CPython from ever specializing
     * calls to this function. */
    _Py_atomic_store_ptr_relaxed(&func->vectorcall, (void *)_PyFunction_Vectorcall);
}

/* Generate machine code for a code object. Returns 0 on success, -1 if the
 * code object is not supported. */
static int
jit_compile(PyCodeObject *co, JyCodeInfo *info)
{
    (void)co;
    (void)info;
    return 0;
}

static int
compile_code(PyCodeObject *co, JyCodeInfo *info)
{
    PyMutex_Lock(&compile_lock);
    int state = _Py_atomic_load_int_relaxed(&info->state);
    if (state == JY_WARMING) {
        if (jit_compile(co, info) == 0) {
            state = JY_COMPILED;
            STAT_INC(codes_compiled);
        }
        else {
            state = JY_REJECTED;
            STAT_INC(codes_rejected);
        }
        _Py_atomic_store_int_release(&info->state, state);
    }
    PyMutex_Unlock(&compile_lock);
    return state;
}

/* Hand the frame to the CPython interpreter, which continues at
 * frame->instr_ptr with the value stack ending at frame->stackpointer. */
static PyObject *
deopt(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    STAT_INC(deopts);
    return _PyEval_EvalFrame(tstate, frame, 0);
}

/* Same as _PyEval_Vector (which is not exported), except that the frame is
 * run by jitted code. */
static PyObject *
jit_entry(PyFunctionObject *func, JyCodeInfo *info, PyObject *const *args,
          size_t nargsf, PyObject *kwnames)
{
    (void)info;
    PyThreadState *tstate = PyThreadState_GetUnchecked();
    size_t argcount = PyVectorcall_NARGS(nargsf);
    size_t total_args = argcount;
    if (kwnames) {
        total_args += PyTuple_GET_SIZE(kwnames);
    }
    _PyStackRef stack_array[8] = {0};
    _PyStackRef *arguments = stack_array;
    if (total_args > 8) {
        arguments = PyMem_Malloc(sizeof(_PyStackRef) * total_args);
        if (arguments == NULL) {
            return PyErr_NoMemory();
        }
    }
    /* _PyEvalFramePushAndInit consumes the references to func, locals and
     * all the arguments. */
    for (size_t i = 0; i < total_args; i++) {
        arguments[i] = PyStackRef_FromPyObjectNew(args[i]);
    }
    PyObject *locals = NULL;
    if (!(((PyCodeObject *)func->func_code)->co_flags & CO_OPTIMIZED)) {
        locals = Py_NewRef(func->func_globals);
    }
    _PyInterpreterFrame *frame = _PyEvalFramePushAndInit(
        tstate, PyStackRef_FromPyObjectNew((PyObject *)func), locals,
        arguments, argcount, kwnames, NULL);
    if (total_args > 8) {
        PyMem_Free(arguments);
    }
    if (frame == NULL) {
        return NULL;
    }
    STAT_INC(jit_entries);
    return deopt(tstate, frame);
}

static PyObject *
jy_vectorcall(PyObject *callable, PyObject *const *args, size_t nargsf,
              PyObject *kwnames)
{
    PyFunctionObject *func = (PyFunctionObject *)callable;
    if (!_Py_atomic_load_int_relaxed(&jy_enabled)) {
        restore_default_vectorcall(func);
        return _PyFunction_Vectorcall(callable, args, nargsf, kwnames);
    }
    PyCodeObject *co = (PyCodeObject *)func->func_code;
    JyCodeInfo *info = get_code_info(co, 1);
    if (info == NULL) {
        PyErr_Clear();
        return _PyFunction_Vectorcall(callable, args, nargsf, kwnames);
    }
    int state = _Py_atomic_load_int_acquire(&info->state);
    if (state == JY_WARMING) {
        if (_Py_atomic_add_ssize(&info->calls, 1) + 1 < jit_threshold) {
            return _PyFunction_Vectorcall(callable, args, nargsf, kwnames);
        }
        state = compile_code(co, info);
    }
    if (state == JY_COMPILED) {
        return jit_entry(func, info, args, nargsf, kwnames);
    }
    restore_default_vectorcall(func);
    return _PyFunction_Vectorcall(callable, args, nargsf, kwnames);
}

static int
hook_function(PyFunctionObject *func, int created)
{
    if (func->vectorcall != _PyFunction_Vectorcall) {
        return 0;
    }
    JyCodeInfo *info = get_code_info((PyCodeObject *)func->func_code, 0);
    if (info == NULL) {
        PyErr_Clear();
    }
    else if (_Py_atomic_load_int_relaxed(&info->state) == JY_REJECTED) {
        return 0;
    }
    if (created) {
        /* The function is not visible to other threads yet, and its version is
         * set by MAKE_FUNCTION after this, so write the slot directly. */
        func->vectorcall = jy_vectorcall;
    }
    else {
        /* Call sites may already be specialized for this function, so its
         * version has to be cleared as well. */
        PyFunction_SetVectorcall(func, jy_vectorcall);
    }
    STAT_INC(functions_hooked);
    return 1;
}

/* Note: the watcher API asks callbacks not to modify the function, but the
 * CREATE event fires after func->vectorcall has been initialized and nothing
 * overwrites it afterwards, so replacing it here is safe. */
static int
func_watcher(PyFunction_WatchEvent event, PyFunctionObject *func,
             PyObject *new_value)
{
    (void)new_value;
    if (event == PyFunction_EVENT_CREATE && _Py_atomic_load_int_relaxed(&jy_enabled)) {
        hook_function(func, 1);
    }
    return 0;
}

/* Hook the functions that already exist when jyston is enabled. */
static int
hook_existing_functions(void)
{
    PyObject *gc = PyImport_ImportModule("gc");
    if (gc == NULL) {
        return -1;
    }
    PyObject *objects = PyObject_CallMethod(gc, "get_objects", NULL);
    Py_DECREF(gc);
    if (objects == NULL) {
        return -1;
    }
    Py_ssize_t n = PyList_GET_SIZE(objects);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *obj = PyList_GET_ITEM(objects, i);
        if (PyFunction_Check(obj)) {
            hook_function((PyFunctionObject *)obj, 0);
        }
    }
    Py_DECREF(objects);
    return 0;
}

static int
initialize(void)
{
    if (initialized) {
        return 0;
    }
    const char *threshold = Py_GETENV("JYSTON_JIT_THRESHOLD");
    if (threshold != NULL && threshold[0] != '\0') {
        jit_threshold = atol(threshold);
    }
    code_extra_index = PyUnstable_Eval_RequestCodeExtraIndex(free_code_info);
    if (code_extra_index < 0) {
        PyErr_SetString(PyExc_RuntimeError, "no code extra index available");
        return -1;
    }
    func_watcher_id = PyFunction_AddWatcher(func_watcher);
    if (func_watcher_id < 0) {
        return -1;
    }
    initialized = 1;
    return 0;
}

static PyObject *
jyston_enable(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    if (initialize() < 0) {
        return NULL;
    }
    if (_Py_atomic_load_int_relaxed(&jy_enabled)) {
        Py_RETURN_NONE;
    }
    _Py_atomic_store_int_release(&jy_enabled, 1);
    if (hook_existing_functions() < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
jyston_disable(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    /* Hooked functions restore their default vectorcall on the next call. */
    _Py_atomic_store_int_release(&jy_enabled, 0);
    Py_RETURN_NONE;
}

static PyObject *
jyston_is_enabled(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return PyBool_FromLong(_Py_atomic_load_int_relaxed(&jy_enabled));
}

static PyObject *
jyston_stats(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return Py_BuildValue(
        "{sn,sn,sn,sn,sn,sn}",
        "jit_threshold", jit_threshold,
        "functions_hooked", _Py_atomic_load_ssize_relaxed(&stats.functions_hooked),
        "codes_compiled", _Py_atomic_load_ssize_relaxed(&stats.codes_compiled),
        "codes_rejected", _Py_atomic_load_ssize_relaxed(&stats.codes_rejected),
        "jit_entries", _Py_atomic_load_ssize_relaxed(&stats.jit_entries),
        "deopts", _Py_atomic_load_ssize_relaxed(&stats.deopts));
}

static PyObject *
jyston_code_state(PyObject *module, PyObject *code)
{
    if (!PyCode_Check(code)) {
        PyErr_SetString(PyExc_TypeError, "expected a code object");
        return NULL;
    }
    if (!initialized) {
        return PyUnicode_FromString("warming");
    }
    JyCodeInfo *info = get_code_info((PyCodeObject *)code, 0);
    if (info == NULL) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        return PyUnicode_FromString("warming");
    }
    switch (_Py_atomic_load_int_relaxed(&info->state)) {
        case JY_COMPILED:
            return PyUnicode_FromString("compiled");
        case JY_REJECTED:
            return PyUnicode_FromString("rejected");
        default:
            return PyUnicode_FromString("warming");
    }
}

static PyMethodDef jyston_methods[] = {
    {"enable", jyston_enable, METH_NOARGS, "Enable the JIT."},
    {"disable", jyston_disable, METH_NOARGS, "Disable the JIT."},
    {"is_enabled", jyston_is_enabled, METH_NOARGS, "Return whether the JIT is enabled."},
    {"stats", jyston_stats, METH_NOARGS, "Return JIT statistics."},
    {"code_state", jyston_code_state, METH_O,
     "Return 'warming', 'compiled' or 'rejected' for a code object."},
    {NULL, NULL, 0, NULL},
};

static PyModuleDef_Slot jyston_slots[] = {
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static struct PyModuleDef jyston_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "jyston",
    .m_doc = "A JIT for CPython, based on Pyston lite.",
    .m_size = 0,
    .m_methods = jyston_methods,
    .m_slots = jyston_slots,
};

PyMODINIT_FUNC
PyInit_jyston(void)
{
    return PyModuleDef_Init(&jyston_module);
}
