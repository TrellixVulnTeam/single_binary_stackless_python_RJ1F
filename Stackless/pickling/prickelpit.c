#include "Python.h"
#ifdef STACKLESS

#include <stddef.h>  /* for offsetof() */
#include "compile.h"

#include "pycore_stackless.h"
#include "pycore_slp_prickelpit.h"

/******************************************************

  type template and support for pickle helper types

 ******************************************************/

#if PY_VERSION_HEX >= 0x030502C1
/* issue25718 got fixed in 3.4.4rc1 */
#define NO_STATE_FORMAT "()"
#define NO_STATE_ARG    /* nothing */
#else
/* Bug http://bugs.python.org/issue25718 requires, that the state object for
 * mutable types has a boolean value of True. Immutable types use a different
 * copy.copy() mechanism.
 */
#define NO_STATE_FORMAT "(O)"
#define NO_STATE_ARG    ,Py_None
#endif

/* check that we really have the right wrapper type */

static int is_wrong_type(PyTypeObject *type)
{
    /* this works because the tp_base's name was modified to
     * point into the wrapper's name
     */
    if (type->tp_base == NULL ||
        strrchr(type->tp_name, '.')+1 != type->tp_base->tp_name) {
        PyErr_SetString(PyExc_TypeError, "incorrect wrapper type");
        return -1;
    }
    return 0;
}

/* supporting __setstate__ for the wrapper type */

static PyObject *
generic_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *inst;
    /* we don't want to support derived types here. */
    if (is_wrong_type(type))
        return NULL;
    assert(type->tp_base->tp_new != NULL);
    inst = type->tp_base->tp_new(type->tp_base, args, kwds);
    if (inst != NULL)
        Py_TYPE(inst) = type;
    return inst;
}

int
slp_generic_init(PyObject *ob, PyObject *args, PyObject *kwds)
{

    initproc init = Py_TYPE(ob)->tp_base->tp_init;

    if (init)
        return init(ob, args, kwds);
    return 0;
}

static PyObject *
generic_setstate(PyObject *self, PyObject *args)
{
    if (is_wrong_type(Py_TYPE(self))) return NULL;
    Py_TYPE(self) = Py_TYPE(self)->tp_base;
    Py_INCREF(self);
    return self;
}

/* redirecting cls.__new__ */

static PyObject *
_new_wrapper(PyObject *self, PyObject *args, PyObject *kwds)
{
    PyTypeObject *type;
    PyObject *newfunc, *res = NULL;

    if (self == NULL || !PyType_Check(self))
        Py_FatalError("__new__() called with non-type 'self'");
    type = (PyTypeObject *)self;
    if (!PyTuple_Check(args) || PyTuple_GET_SIZE(args) < 1) {
        PyErr_Format(PyExc_TypeError,
                     "%s.__new__(): not enough arguments",
                     type->tp_name);
        return NULL;
    }
    if (is_wrong_type(type)) return NULL;

    newfunc = PyObject_GetAttrString((PyObject *) type->tp_base, "__new__");
    if (newfunc != NULL) {
        res = PyObject_Call(newfunc, args, kwds);
        Py_DECREF(newfunc);
    }
    return res;
}

/* just in case that setstate gets not called, we need to protect* */

static void
_wrap_dealloc(PyObject *ob)
{
    Py_TYPE(ob) = Py_TYPE(ob)->tp_base;
    if (Py_TYPE(ob)->tp_dealloc != NULL)
        Py_TYPE(ob)->tp_dealloc(ob);
}

static int
_wrap_traverse(PyObject *ob, visitproc visit, void *arg)
{
    PyTypeObject *type = Py_TYPE(ob);
    int ret = 0;
    Py_TYPE(ob) = type->tp_base;
    if (Py_TYPE(ob)->tp_traverse != NULL)
        ret = Py_TYPE(ob)->tp_traverse(ob, visit, arg);
    Py_TYPE(ob) = type;
    return ret;
}

static int
_wrap_clear(PyObject *ob)
{
    Py_TYPE(ob) = Py_TYPE(ob)->tp_base;
    if (Py_TYPE(ob)->tp_clear != NULL)
        Py_TYPE(ob)->tp_clear(ob);
    return 0;
}


#define MAKE_WRAPPERTYPE(type, prefix, name, reduce, newfunc, setstate) \
 \
static PyMethodDef prefix##_methods[] = { \
    {"__reduce__",     (PyCFunction)reduce,             METH_NOARGS,    NULL}, \
    {"__setstate__",   (PyCFunction)setstate,           METH_O,         NULL}, \
    {"__new__",        (PyCFunction)(void(*)(void))_new_wrapper, METH_VARARGS | METH_KEYWORDS, \
     PyDoc_STR("wwwwwaaaaaT.__new__(S, ...) -> " \
               "a new object with type S, a subtype of T")}, \
    {NULL, NULL} \
}; \
 \
static struct _typeobject wrap_##type = { \
    PyVarObject_HEAD_INIT(&PyType_Type, 0) \
    .tp_name = "_stackless._wrap." name, \
    .tp_dealloc = (destructor)_wrap_dealloc, \
    .tp_getattro = PyObject_GenericGetAttr, \
    .tp_setattro = PyObject_GenericSetAttr, \
    .tp_traverse = (traverseproc) _wrap_traverse, \
    .tp_clear = (inquiry) _wrap_clear, \
    .tp_methods = prefix##_methods, \
    .tp_base = &type, \
    .tp_init = slp_generic_init, \
    .tp_new = newfunc, \
};


PyDoc_STRVAR(set_reduce_frame__doc__,
"set_reduce_frame(func) -- set the function used to reduce frames during pickling.\n"
"The function takes a frame as its sole argument and must return a pickleable object.\n");

static PyObject *
set_reduce_frame(PyObject *self, PyObject *func)
{
    PyThreadState * ts = _PyThreadState_GET();
    if (func == Py_None) {
        Py_CLEAR(ts->interp->st.reduce_frame_func);
    } else {
        if (!PyCallable_Check(func)) {
            TYPE_ERROR("func must be callable", NULL);
        }
        Py_INCREF(func);
        Py_XSETREF(ts->interp->st.reduce_frame_func, func);
    }
    Py_RETURN_NONE;
}

PyObject *
slp_reduce_frame(PyFrameObject * frame) {
    PyThreadState * ts = _PyThreadState_GET();
    if (!PyFrame_Check(frame) || ts->interp->st.reduce_frame_func == NULL) {
        Py_INCREF(frame);
        return (PyObject *)frame;
    }
    return PyObject_CallFunctionObjArgs(ts->interp->st.reduce_frame_func, (PyObject *)frame, NULL);
}

/* Helper function for gen_setstate and tb_setstate.
 * It unwraps the first argument of the args tuple, if it is a _Frame_Wrapper.
 * Returns a new reference to an argument tuple.
 *
 * This functionality is required, to adhere to the __reduce__/__setstate__ protocol.
 * It requires, that __setstate__ accepts the state returned by __reduce__. (copy.copy()
 * depends on it.)
 */
static PyObject *
unwrap_frame_arg(PyObject * args) {
    PyThreadState * ts = _PyThreadState_GET();
    PyObject *wrapper_type, *arg0, *result;
    int is_instance;
    Py_ssize_t len, i;

    if (!PyTuple_Check(args)) {
        Py_INCREF(args);
        return args;
    }
    if ((len = PyTuple_Size(args)) < 1) {
        if (len < 0)
            return NULL;
        Py_INCREF(args);
        return args;
    }
    if ((arg0 = PyTuple_GetItem(args, 0)) == NULL) /* arg0 is a borrowed reference */
        return NULL;
    if ((wrapper_type = PyObject_GetAttrString(
            ts->interp->st.reduce_frame_func, "__self__")) == NULL)
        return NULL;
    is_instance = PyObject_IsInstance(arg0, wrapper_type);
    Py_DECREF(wrapper_type);
    if (is_instance == 0) {
        Py_INCREF(args);
        return args;
    } else if (is_instance == -1) {
        return NULL;
    }
    if ((arg0 = PyObject_GetAttrString(arg0, "frame")) == NULL)
        return NULL;
    if ((result = PyTuple_New(len)) == NULL) {
        Py_DECREF(arg0);
        return NULL;
    }
    if (PyTuple_SetItem(result, 0, arg0)) { /* steals ref to arg0 */
        Py_DECREF(arg0);
        Py_DECREF(result);
        return NULL;
    }
    for (i=1; i<len; i++) {
        if ((arg0 = PyTuple_GetItem(args, i)) == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        /* arg0 is a borrowed reference */
        Py_INCREF(arg0);
        if (PyTuple_SetItem(result, i, arg0)) { /* steals ref to arg0 */
            Py_DECREF(arg0);
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

static struct PyMethodDef _new_methoddef[] = {
    {"__new__", (PyCFunction)(void(*)(void))_new_wrapper, METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("T.__new__(S, ...) -> "
               "a new object with type S, a subtype of T.__base__")},
    {0}
};

static int init_type(PyTypeObject *t, int (*initchain)(PyObject *), PyObject * mod)
{
    PyMethodDescrObject *reduce;
    PyWrapperDescrObject *init;
    PyObject *func;
    const char *name = strrchr(t->tp_name, '.')+1;

    /* we patch the type to use *our* name, which makes no difference */
    assert (strcmp(name, t->tp_base->tp_name) == 0);
    t->tp_base->tp_name = name;
    t->tp_basicsize = t->tp_base->tp_basicsize;
    t->tp_itemsize  = t->tp_base->tp_itemsize;
    /* PEP-590 Vectorcall-protocol requires to copy tp_descr_get, tp_vectorcall_offset
     * and tp_call from the base class
     */
    t->tp_descr_get = t->tp_base->tp_descr_get;
    t->tp_vectorcall_offset = t->tp_base->tp_vectorcall_offset;
    t->tp_call = t->tp_base->tp_call;

    t->tp_flags     = t->tp_base->tp_flags & ~Py_TPFLAGS_READY;
    if (PyObject_SetAttrString(mod, name, (PyObject *) t))
        return -1;
    /* patch the method descriptors to require the base type */
    if (PyType_Ready(t)) return -1;
    init = (PyWrapperDescrObject *) PyDict_GetItemString(t->tp_dict,
                                "__init__");
    PyDescr_TYPE(init) = t->tp_base;
    reduce = (PyMethodDescrObject *) PyDict_GetItemString(t->tp_dict,
                                "__reduce__");
    PyDescr_TYPE(reduce) = t->tp_base;
    /* insert the __new__ replacement which is special */
    func = PyCFunction_New(_new_methoddef, (PyObject *)t);
    if (func == NULL || PyDict_SetItemString(t->tp_dict, "__new__", func))
        return -1;
    if (initchain != NULL)
        return initchain(mod);
    return 0;
}

/* root of init function chain */

#define initchain NULL

/* helper to execute a bit of code which simplifies things */

static PyObject *
run_script(char *src, char *retname)
{
    PyObject *globals = PyDict_New();
    PyObject *retval;

    if (globals == NULL)
        return NULL;
    if (PyDict_SetItemString(globals, "__builtins__",
                             PyEval_GetBuiltins()) != 0)
        return NULL;
    retval = PyRun_String(src, Py_file_input, globals, globals);
    if (retval != NULL) {
        Py_DECREF(retval);
        retval = PyMapping_GetItemString(globals, retname);
    }
    PyDict_Clear(globals);
    Py_DECREF(globals);
    return retval;
}

/******************************************************

  default execute function for invalid frames

 ******************************************************/

/*
 * note that every new execute function should also create
 * a different call of this function.
 */

PyObject *
slp_cannot_execute(PyCFrameObject *f, const char *exec_name, PyObject *retval)
{
    /*
     * Special rule for frame execution functions: we now own a reference to retval!
     */

    /*
     * show an error message and raise exception.
     */
    PyThreadState *tstate = _PyThreadState_GET();

    /* if we already have an exception, we keep it */
    if (retval != NULL) {
        Py_DECREF(retval);
        PyErr_Format(PyExc_RuntimeError, "cannot execute invalid frame with "
                "'%.100s': frame had a C state that can't be restored or an invalid code object.",
                exec_name);
    }

    SLP_STORE_NEXT_FRAME(tstate, f->f_back);

    return NULL;
}

/* registering and retrieval of frame exec functions */

/* unfortunately, this object is not public,
 * so we need to repeat it here:
 */

typedef struct {
    PyObject_HEAD
    PyObject *dict;
} proxyobject;

int
slp_register_execute(PyTypeObject *t, char *name, PyFrame_ExecFunc *good,
                     PyFrame_ExecFunc *bad)
{
    PyObject *g = NULL, *b = NULL, *nameobj = NULL;
    PyObject *tup = NULL, *dic = NULL;
    PyObject *o;
    proxyobject *dp = NULL;
    int ret = -1;

/*
    WE CANNOT BE DOING THIS HERE, AS THE EXCEPTION CLASSES ARE NOT INITIALISED.
    assert(PyObject_IsSubclass((PyObject *)t, (PyObject *)&PyFrame_Type) ||
           PyObject_IsSubclass((PyObject *)t,
                               (PyObject *)&PyCFrame_Type));
*/
    if (0
        || PyType_Ready(t) || name == NULL
        || (nameobj = PyUnicode_FromString(name)) == NULL
        || (g = PyLong_FromVoidPtr(good)) == NULL
        || (b = PyLong_FromVoidPtr(bad)) == NULL
        || (tup = Py_BuildValue("OO", g, b)) == NULL
        )
        goto err_exit;
    dp = (proxyobject*) PyDict_GetItemString(t->tp_dict, "_exec_map");
    if ((dic = dp ? dp->dict : NULL) == NULL) {
        if (0
            || (dic = PyDict_New()) == NULL
            || (dp = (proxyobject *) PyDictProxy_New(dic)) == NULL
            || PyDict_SetItemString(t->tp_dict, "_exec_map",
                                    (PyObject *) dp)
            )
            goto err_exit;
    }
    else {
        Py_INCREF(dic);
        Py_INCREF(dp);
    }
    if (0
        || (o = PyDict_SetDefault(dp->dict, nameobj, tup)) == NULL
        || !PyObject_RichCompareBool(o, tup, Py_EQ)
        || (o = PyDict_SetDefault(dp->dict, g, nameobj)) == NULL
        || !PyObject_RichCompareBool(o, nameobj, Py_EQ)
        || (o = PyDict_SetDefault(dp->dict, b, nameobj)) == NULL
        || !PyObject_RichCompareBool(o, nameobj, Py_EQ)
        ) {
                if (! PyErr_Occurred())
                PyErr_SetString(PyExc_SystemError, "duplicate/ambiguous exec func");
        goto err_exit;
    }
    PyErr_Clear();
    ret = 0;
err_exit:
    Py_XDECREF(nameobj);
    Py_XDECREF(g);
    Py_XDECREF(b);
    Py_XDECREF(tup);
    Py_XDECREF(dic);
    Py_XDECREF(dp);
    return ret;
}

int
slp_find_execfuncs(PyTypeObject *type, PyObject *exec_name,
                   PyFrame_ExecFunc **good, PyFrame_ExecFunc **bad)
{
    PyObject *g, *b;
    proxyobject *dp = (proxyobject *)
                      PyDict_GetItemString(type->tp_dict, "_exec_map");
    PyObject *dic = dp ? dp->dict : NULL;
    PyObject *exec_tup = dic ? PyDict_GetItem(dic, exec_name) : NULL;

    if (0
        || exec_tup == NULL
        || !PyArg_ParseTuple(exec_tup, "OO", &g, &b)
        || (*good = (PyFrame_ExecFunc*)PyLong_AsVoidPtr(g)) == NULL
        || (*bad =  (PyFrame_ExecFunc*)PyLong_AsVoidPtr(b)) == NULL) {
        char msg[500];

        PyErr_Clear();
        sprintf(msg, "Frame exec function '%.20s' not defined for %s",
            _PyUnicode_AsString(exec_name), type->tp_name);
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }
    return 0;
}

PyObject *
slp_find_execname(PyCFrameObject *cf, int *valid)
{
    PyObject *exec_name = NULL;
    proxyobject *dp = (proxyobject *)
                      PyDict_GetItemString(Py_TYPE(cf)->tp_dict, "_exec_map");
    PyObject *dic = dp ? dp->dict : NULL;
    PyObject *exec_addr = PyLong_FromVoidPtr(cf->f_execute);

    assert(valid != NULL);

    if (exec_addr == NULL) return NULL;
    exec_name = dic ? PyDict_GetItem(dic, exec_addr) : NULL;
    if (exec_name == NULL) {
        char msg[500];
        PyErr_Clear();
        sprintf(msg, "frame exec function at %p is not registered!",
            (void *)cf->f_execute);
        PyErr_SetString(PyExc_ValueError, msg);
        *valid = 0;
    }
    else {
        PyFrame_ExecFunc *good, *bad;
        if (slp_find_execfuncs(Py_TYPE(cf), exec_name, &good, &bad)) {
            exec_name = NULL;
            goto err_exit;
        }
        if (cf->f_execute == bad)
            *valid = 0;
        else if (cf->f_execute != good) {
            PyErr_SetString(PyExc_SystemError,
                "inconsistent c?frame function registration");
            goto err_exit;
        }
    }
err_exit:
    Py_XDECREF(exec_addr);
    Py_XINCREF(exec_name);
    return exec_name;
}

/******************************************************

  pickling of objects that may contain NULLs

 ******************************************************/

/*
 * To restore arrays which can contain NULLs, we add an extra
 * tuple at the beginning, which contains the positions of
 * all objects which are meant to be a real NULL.
 */

PyObject *
slp_into_tuple_with_nulls(PyObject **start, Py_ssize_t length)
{
    PyObject *res = PyTuple_New(length+1);
    PyObject *nulls = PyTuple_New(0);
    Py_ssize_t i, nullcount = 0;
    if (res == NULL)
        return NULL;
    for (i=0; i<length; ++i) {
        PyObject *ob = start[i];
        if (ob == NULL) {
            /* store None, and add the position to nulls */
            PyObject *pos = PyLong_FromSsize_t(i);
            if (pos == NULL)
                return NULL;
            ob = Py_None;
            if (_PyTuple_Resize(&nulls, ++nullcount))
                return NULL;
            PyTuple_SET_ITEM(nulls, nullcount-1, pos);
        }
        Py_INCREF(ob);
        PyTuple_SET_ITEM(res, i+1, ob);
    }
    /* save NULL positions as first element */
    PyTuple_SET_ITEM(res, 0, nulls);
    return res;
}

Py_ssize_t
slp_from_tuple_with_nulls(PyObject **start, PyObject *tup)
{
    Py_ssize_t i, length = PyTuple_GET_SIZE(tup)-1;
    PyObject *nulls;
    if (length < 0) return 0;

    /* put the values into the array */
    for (i=0; i<length; ++i) {
        PyObject *ob = PyTuple_GET_ITEM(tup, i+1);
        Py_INCREF(ob);
        start[i] = ob;
    }
    nulls = PyTuple_GET_ITEM(tup, 0);
    if (!PyTuple_Check(nulls)) {
        /* XXX we should report this error */
        return length;
    }
    /* wipe the NULL positions */
    for (i=0; i<PyTuple_GET_SIZE(nulls); ++i) {
        PyObject *pos = PyTuple_GET_ITEM(nulls, i);
        if (PyLong_CheckExact(pos)) {
            int p = PyLong_AS_LONG(pos);
            if (p >= 0 && p < length) {
                PyObject *hold = start[p];
                start[p] = NULL;
                Py_XDECREF(hold);
            }
        }
    }
    return length;
}

/******************************************************

  pickling of code objects

 ******************************************************/

#define codetuplefmt "liiiiiiSOOOSSiSOO"
/* Index of co_code in the tuple given to code_new */
static const Py_ssize_t code_co_code_index = 6;

/*
 * An unused (invalid) opcode. See opcode.h for a list of used opcodes.
 * If Stackless unpickles a code object with an invalid magic number, it prefixes
 * co_code with this opcode.
 *
 * frame_setstate tests if the first opcode of the code of the frame is CODE_INVALID_OPCODE
 * and eventually marks a frame as invalid.
 */
static const char CODE_INVALID_OPCODE = 0;

static struct _typeobject wrap_PyCode_Type;
static long bytecode_magic = 0;

static PyObject *
code_reduce(PyCodeObject * co, PyObject *unused)
{
    if (0 >= bytecode_magic) {
        bytecode_magic = PyImport_GetMagicNumber();
        if (-1 == bytecode_magic)
            return NULL;
    }

    PyObject *tup = Py_BuildValue(
        "(O(" codetuplefmt ")())",
        &wrap_PyCode_Type,
        bytecode_magic,
        co->co_argcount,
        co->co_posonlyargcount,
        co->co_kwonlyargcount,
        co->co_nlocals,
        co->co_stacksize,
        co->co_flags,
        co->co_code,
        co->co_consts,
        co->co_names,
        co->co_varnames,
        co->co_filename,
        co->co_name,
        co->co_firstlineno,
        co->co_lnotab,
        co->co_freevars,
        co->co_cellvars
    );

    return tup;
}

static PyObject *
code_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    long magic = 0;

    if (0 >= bytecode_magic) {
        bytecode_magic = PyImport_GetMagicNumber();
        if (-1 == bytecode_magic)
            return NULL;
    }

    assert(PyTuple_CheckExact(args));
    if (PyTuple_GET_SIZE(args) == sizeof(codetuplefmt) - 1) {
        /*  */
        magic = PyLong_AsLong(PyTuple_GET_ITEM(args, 0));
        if (-1 == magic && PyErr_Occurred()) {
            return NULL;
        }
        args = PyTuple_GetSlice(args, 1, sizeof(codetuplefmt) - 1);
    } else {
        PyErr_SetString(PyExc_IndexError, "code_new: Argument tuple has wrong size.");
        return NULL;
    }
    if (NULL == args)
        return NULL;

    if (bytecode_magic != magic) {
        if (PyErr_WarnFormat(PyExc_RuntimeWarning, 1, "Unpickling code object with invalid magic number %ld", magic)) {
            Py_DECREF(args);
            return NULL;
        }

        PyObject *code = PyTuple_GET_ITEM(args, code_co_code_index);
        if (NULL == code) {
            Py_DECREF(args);
            return NULL;
        }
        if (!PyBytes_Check(code)) {
            Py_DECREF(args);
            PyErr_SetString(PyExc_TypeError,
                            "Unpickling code object: code is not a bytes object");
            return NULL;
        }
        Py_ssize_t code_len = PyBytes_Size(code);
        assert(code_len <= INT_MAX);
        assert(code_len % sizeof(_Py_CODEUNIT) == 0);

        /* Now prepend an invalid opcode to the code.
         */
        PyObject *code2 = PyBytes_FromStringAndSize(NULL, code_len + sizeof(_Py_CODEUNIT));
        char *p = PyBytes_AS_STRING(code2);
        p[0] = Py_BUILD_ASSERT_EXPR(sizeof(_Py_CODEUNIT) == 2) + CODE_INVALID_OPCODE;
        p[1] = 0; /* Argument */
        memcpy(p + sizeof(_Py_CODEUNIT), PyBytes_AS_STRING(code), code_len);
        if (PyTuple_SetItem(args, code_co_code_index, code2)) {
            Py_DECREF(args);
            return NULL;
        }
    }

    PyObject *retval = generic_new(type, args, kwds);
    Py_DECREF(args);
    return retval;
}

MAKE_WRAPPERTYPE(PyCode_Type, code, "code", code_reduce, code_new,
                 generic_setstate)

static int init_codetype(PyObject * mod)
{
    return init_type(&wrap_PyCode_Type, initchain, mod);
}
#undef initchain
#define initchain init_codetype


/******************************************************

  pickling addition to cell objects

 ******************************************************/

/*
 * cells create cycles via function closures.
 * We therefore need to use the 3-element protocol
 * of __reduce__
 * We must also export this type to funcobject where
 * a typecheck of the function_closure member is done,
 * since a function may get a __setstate__ call with
 * a partially initialized cell object.
 */

static PyTypeObject wrap_PyCell_Type;
PyTypeObject *_Pywrap_PyCell_Type = &wrap_PyCell_Type;

static PyObject *
cell_reduce(PyCellObject *cell, PyObject *unused)
{
    PyObject *tup = NULL;

    if (cell->ob_ref == NULL) {
        tup = Py_BuildValue("(O()())", &wrap_PyCell_Type);
    }
    else {
        tup = Py_BuildValue("(O()(O))", &wrap_PyCell_Type, cell->ob_ref);
    }
    return tup;
}

static PyObject *
cell_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *ob;

    if (is_wrong_type(type)) return NULL;
    if (!PyArg_ParseTuple (args, "", &ob))
        return NULL;
    ob = PyCell_New(NULL);
    if (ob != NULL)
        Py_TYPE(ob) = type;
    return ob;
}

/* note that args is a tuple, although we use METH_O */

static PyObject *
cell_setstate(PyObject *self, PyObject *args)
{
    PyCellObject *cell = (PyCellObject *) self;
    PyObject *ob = NULL;

    if (is_wrong_type(Py_TYPE(self))) return NULL;
    if (!PyArg_ParseTuple (args, "|O", &ob))
        return NULL;
    Py_XINCREF(ob);
    Py_CLEAR(cell->ob_ref);
    cell->ob_ref = ob;
    Py_INCREF(self);
    Py_TYPE(self) = Py_TYPE(self)->tp_base;
    return self;
}

MAKE_WRAPPERTYPE(PyCell_Type, cell, "cell", cell_reduce, cell_new, cell_setstate)

static int init_celltype(PyObject * mod)
{
    return init_type(&wrap_PyCell_Type, initchain, mod);
}
#undef initchain
#define initchain init_celltype


/******************************************************

  pickling addition to function objects

 ******************************************************/

#define functuplefmt_pre38 "OOOOOO"
#define functuplefmt functuplefmt_pre38 "OOOOO"

static PyTypeObject wrap_PyFunction_Type;

static PyObject *
func_reduce(PyFunctionObject * func, PyObject *unused)
{
    /* See funcobject.c: some attribute can't be NULL. */
    assert(func->func_code);
    assert(func->func_globals);
    assert(func->func_name);
    assert(func->func_doc);
    assert(func->func_qualname);

    PyObject *dict = PyObject_GenericGetDict((PyObject *)func, NULL);
    if (NULL == dict)
        return NULL;

    PyObject *tup = Py_BuildValue(
        "(O()(" functuplefmt "))",
        &wrap_PyFunction_Type,
        /* Standard function constructor arguments. */
        func->func_code,
        func->func_globals,
        func->func_name,
        func->func_defaults != NULL ? func->func_defaults : Py_None,
        func->func_closure != NULL ? func->func_closure : Py_None,
        func->func_module != NULL ? func->func_module : Py_None,
        func->func_kwdefaults != NULL ? func->func_kwdefaults : Py_None,
        func->func_doc,
        dict,
        func->func_annotations != NULL ? func->func_annotations : Py_None,
        func->func_qualname
    );
    Py_DECREF(dict);
    return tup;
}

static PyObject *
func_new(PyTypeObject *type, PyObject *args, PyObject *kewd)
{
    PyObject *ob = NULL, *co = NULL, *globals = NULL;

    /* create a fake function for later initialization */
    if (is_wrong_type(type)) return NULL;
    if ((co = Py_CompileString("", "", Py_file_input)) != NULL)
        if ((globals = PyDict_New()) != NULL)
            if ((ob = PyFunction_New(co, globals)) != NULL)
                Py_TYPE(ob) = type;
    Py_XDECREF(co);
    Py_XDECREF(globals);
    return ob;
}

#define COPY(src, dest, attr) Py_XINCREF(src->attr); Py_CLEAR(dest->attr); \
                              dest->attr = src->attr

static PyObject *
func_setstate(PyObject *self, PyObject *args)
{
    PyFunctionObject *fu;
    PyObject *args2;
    char *pcode;

    if (is_wrong_type(Py_TYPE(self))) return NULL;
    Py_TYPE(self) = Py_TYPE(self)->tp_base;

    /* Test for an invalid code object */
    args2 = PyTuple_GetItem(args, 0);
    if (NULL==args2)
        return NULL;
    if (! PyCode_Check(args2)) {
        PyErr_SetString(PyExc_TypeError, "func_setstate: value for func_code is not a code object");
        return NULL;
    }
    pcode = PyBytes_AsString(((PyCodeObject *) args2)->co_code);
    if (NULL == pcode)
        return NULL;
    if (*pcode == CODE_INVALID_OPCODE) {
        /* invalid code object, was pickled with a different version of python */
        if (PyErr_WarnFormat(PyExc_RuntimeWarning, 1, "Unpickling function with invalid code object: %V",
                PyTuple_GetItem(args, 2), "~ name is missing ~"))
            return NULL;
    }

    args2 = PyTuple_GetSlice(args, 0, 5);
    if (args2 == NULL)
        return NULL;

    fu = (PyFunctionObject *) Py_TYPE(self)->tp_new(Py_TYPE(self), args2, NULL);
    Py_DECREF(args2);
    if (fu == NULL)
        return NULL;

    PyFunctionObject *target = (PyFunctionObject *) self;
    COPY(fu, target, func_code);
    COPY(fu, target, func_globals);
    COPY(fu, target, func_name);
    COPY(fu, target, func_defaults);
    COPY(fu, target, func_closure);
    Py_DECREF(fu);

    args2 = PyTuple_GetItem(args, 5);
    if (NULL == args2)
        return NULL;
    Py_INCREF(args2);
    Py_XSETREF(target->func_module, args2);

    if (PyTuple_GET_SIZE(args) != sizeof(functuplefmt_pre38)-1) {
        /* Stackless 3.8 and up */
        if (PyTuple_GET_SIZE(args) != sizeof(functuplefmt)-1) {
            PyErr_Format(PyExc_IndexError, "function.__setstate__ expects a tuple of length %d", (int)sizeof(functuplefmt)-1);
            return NULL;
        }
        args2 = PyTuple_GET_ITEM(args, 6);
        if (PyFunction_SetKwDefaults(self, args2))
            return NULL;

        args2 = PyTuple_GET_ITEM(args, 7);
        Py_INCREF(args2);
        Py_XSETREF(target->func_doc, args2);

        args2 = PyTuple_GET_ITEM(args, 8);
        if (args2 != Py_None && PyObject_GenericSetDict(self, args2, NULL))
            return NULL;

        args2 = PyTuple_GET_ITEM(args, 9);
        if (PyFunction_SetAnnotations(self, args2))
            return NULL;

        args2 = PyTuple_GET_ITEM(args, 10);
        if(!PyUnicode_Check(args2)) {
            PyErr_SetString(PyExc_TypeError, "__qualname__ must be set to a string object");
            return NULL;
        }
        Py_INCREF(args2);
        Py_XSETREF(target->func_qualname, args2);
    }

    Py_INCREF(self);
    return self;
}

#undef COPY

MAKE_WRAPPERTYPE(PyFunction_Type, func, "function", func_reduce, func_new,
                 func_setstate)

static int init_functype(PyObject * mod)
{
    return init_type(&wrap_PyFunction_Type, initchain, mod);
}
#undef initchain
#define initchain init_functype


/******************************************************

  pickling addition to frame objects

 ******************************************************/

#define frametuplefmt "O)(OiBOiOOiiOO"

SLP_DEF_INVALID_EXEC(slp_channel_seq_callback)
SLP_DEF_INVALID_EXEC(slp_tp_init_callback)

static PyTypeObject wrap_PyFrame_Type;

static PyObject *
frameobject_reduce(PyFrameObject *f, PyObject *unused)
{
    int i;
    PyObject **f_stacktop;
    PyObject *blockstack_as_tuple = NULL, *localsplus_as_tuple = NULL,
    *res = NULL;
    int valid = 1;
    int have_locals = f->f_locals != NULL;
    PyObject * dummy_locals = NULL;
    PyObject * f_trace = NULL;
    PyThreadState *ts = _PyThreadState_GET();

    if (!have_locals)
        if ((dummy_locals = PyDict_New()) == NULL)
            return NULL;

    blockstack_as_tuple = PyTuple_New (f->f_iblock);
    if (blockstack_as_tuple == NULL) goto err_exit;

    for (i = 0; i < f->f_iblock; i++) {
        PyObject *tripel = Py_BuildValue("iii",
                        f->f_blockstack[i].b_type,
                        f->f_blockstack[i].b_handler,
                        f->f_blockstack[i].b_level);
        if (!tripel) goto err_exit;
        PyTuple_SET_ITEM(blockstack_as_tuple, i, tripel);
    }

    f_stacktop = f->f_stacktop;
    if (f_stacktop != NULL) {
        if (f_stacktop < f->f_valuestack) {
            PyErr_SetString(PyExc_ValueError, "stack underflow");
        goto err_exit;
        }
        localsplus_as_tuple = slp_into_tuple_with_nulls(
            f->f_localsplus, f_stacktop - f->f_localsplus);
        if (localsplus_as_tuple == NULL) goto err_exit;
    }
    else {
        localsplus_as_tuple = slp_into_tuple_with_nulls(
            f->f_localsplus, f->f_valuestack - f->f_localsplus);
        if (localsplus_as_tuple == NULL) goto err_exit;

        /* frames without a stacktop cannot be run */
        valid = 0;
    }

    f_trace = f->f_trace;
    if (NULL == f_trace)
        f_trace = Py_None;
    Py_INCREF(f_trace);
    if (f_trace != Py_None) {
        if (!(ts->st.pickleflags & SLP_PICKLEFLAGS_PRESERVE_TRACING_STATE)) {
            Py_DECREF(f_trace);
            f_trace = Py_None;
            Py_INCREF(f_trace);
        }
    }

    res = Py_BuildValue ("(O(" frametuplefmt "))",
                         &wrap_PyFrame_Type,
                         f->f_code,
                         f->f_code,
                         valid,
                         f->f_executing,
                         f->f_globals,
                         have_locals,
                         have_locals ? f->f_locals : dummy_locals,
                         f_trace,
                 f->f_lasti,
                 f->f_lineno,
                 blockstack_as_tuple,
                 localsplus_as_tuple
                 );

err_exit:
    Py_XDECREF(blockstack_as_tuple);
    Py_XDECREF(localsplus_as_tuple);
    Py_XDECREF(dummy_locals);
    Py_XDECREF(f_trace);
    return res;
}

#define frametuplenewfmt "O!:frame.__new__"
#define frametuplesetstatefmt "O!iBO!iO!OiiO!O:frame.__setstate__"

static PyObject *
frame_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyThreadState *ts = _PyThreadState_GET();
    PyFrameObject *f;
    PyCodeObject *f_code;
    PyObject *globals;

    if (is_wrong_type(type)) return NULL;
    if (!PyArg_ParseTuple(args, frametuplenewfmt, &PyCode_Type, &f_code))
        return NULL;
    if (SLP_CURRENT_FRAME(ts) != NULL && PyFrame_Check(SLP_CURRENT_FRAME(ts))) {
        globals = SLP_CURRENT_FRAME(ts)->f_globals;
        Py_INCREF(globals);
    }
    else
        globals = Py_BuildValue("{sO}", "__builtins__",
                                PyEval_GetBuiltins());
    if (globals == NULL)
        return NULL;
    f = PyFrame_New(ts, (PyCodeObject *) f_code, globals, globals);
    if (f != NULL)
        Py_TYPE(f) = &wrap_PyFrame_Type;
    Py_DECREF(globals);
    return (PyObject *) f;
}


static PyObject *
frame_setstate(PyFrameObject *f, PyObject *args)
{
    int f_lasti, f_lineno;
    Py_ssize_t i;
    PyObject *f_globals, *f_locals, *blockstack_as_tuple;
    PyObject *localsplus_as_tuple, *trace, *f_code;
    int valid, have_locals;
    char f_executing;
    Py_ssize_t tmp;
    char *pcode;

    if (is_wrong_type(Py_TYPE(f))) return NULL;

    Py_CLEAR(f->f_locals);

    if (!PyArg_ParseTuple (args, frametuplesetstatefmt,
            &PyCode_Type, &f_code,
            &valid,
            &f_executing,
            &PyDict_Type, &f_globals,
            &have_locals,
            &PyDict_Type, &f_locals,
            &trace,
            &f_lasti,
            &f_lineno,
            &PyTuple_Type, &blockstack_as_tuple,
            &localsplus_as_tuple))
        return NULL;

    if (f->f_code != (PyCodeObject *) f_code) {
        PyErr_SetString(PyExc_TypeError,
                        "invalid code object for frame_setstate");
        return NULL;
    }
    pcode = PyBytes_AsString(((PyCodeObject *) f_code)->co_code);
    if (NULL == pcode)
        return NULL;
    if (*pcode == CODE_INVALID_OPCODE)
        valid = 0;  /* invalid code object, was pickled with a different version of python */

    if (have_locals) {
        Py_INCREF(f_locals);
        f->f_locals = f_locals;
    }
    Py_INCREF(f_globals);
    Py_SETREF(f->f_globals, f_globals);

    if (trace != Py_None) {
        if (!PyCallable_Check(trace)) {
            PyErr_SetString(PyExc_TypeError,
                            "trace must be a function for frame");
            goto err_exit;
        }
        Py_INCREF(trace);
        assert(f->f_trace == NULL);
        f->f_trace = trace;
    }

    if (PyTuple_Check(localsplus_as_tuple)) {
        Py_ssize_t space =  f->f_code->co_stacksize + (f->f_valuestack - f->f_localsplus);

        if (PyTuple_GET_SIZE(localsplus_as_tuple)-1 > space) {
            PyErr_SetString(PyExc_ValueError, "invalid localsplus for frame");
            goto err_exit;
        }
        f->f_stacktop = f->f_localsplus;
        f->f_stacktop += slp_from_tuple_with_nulls(f->f_localsplus,
                                                   localsplus_as_tuple);
    }
    else if (localsplus_as_tuple == Py_None) {
        Py_ssize_t ncells, nfreevars;

        f->f_stacktop = NULL;
        valid = 0;  /* cannot run frame without stack */

        /* Now handle pickles without localsplus in case of cell variables.
           Python requires that localsplus[index_of_a_cellvar] is a cell object.
           Otherwise PyFrame_FastToLocals() crashes in frameobject.c: map_to_dict */
        ncells = PyTuple_GET_SIZE(f->f_code->co_cellvars);
        nfreevars = PyTuple_GET_SIZE(f->f_code->co_freevars);
        if (ncells || nfreevars) {
            PyObject **values;

            assert(f->f_code->co_nlocals + ncells + nfreevars == f->f_valuestack - f->f_localsplus);

            values = f->f_localsplus + f->f_code->co_nlocals;
            for(i = ncells + nfreevars; --i >= 0; ) {
                if (values[i] == NULL) {
                    values[i] = PyCell_New(NULL);  /* uninitialised variable */
                } else {
                    assert(PyCell_Check(values[i]));
                }
            }
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError, "stack must be tuple or None for frame");
        goto err_exit;
    }

    /* mark this frame as coming from unpickling */
    Py_INCREF(Py_None);
    Py_CLEAR(f->f_back);
    f->f_back = (PyFrameObject *) Py_None;

    f->f_lasti = f_lasti;
    f->f_lineno = f_lineno;
    tmp = PyTuple_GET_SIZE(blockstack_as_tuple);
    f->f_iblock = Py_SAFE_DOWNCAST(tmp, Py_ssize_t, int);
    if (f->f_iblock < 0 || f->f_iblock > CO_MAXBLOCKS) {
        PyErr_SetString(PyExc_ValueError, "invalid blockstack for frame");
        goto err_exit;
    }
    for (i = 0; i < CO_MAXBLOCKS; i++) {
        if (i < f->f_iblock) {
            if (!PyArg_ParseTuple(
                PyTuple_GET_ITEM(blockstack_as_tuple, i),
                "iii",
                &f->f_blockstack[i].b_type,
                &f->f_blockstack[i].b_handler,
                &f->f_blockstack[i].b_level
                ))
                goto err_exit;
        } else {
            f->f_blockstack[i].b_type =
            f->f_blockstack[i].b_handler =
            f->f_blockstack[i].b_level = 0;
        }
    }

    /* See if this frame is valid to be run. */
    f->f_executing = valid ? f_executing : SLP_FRAME_EXECUTING_INVALID;

    Py_TYPE(f) = &PyFrame_Type;
    Py_INCREF(f);
    return (PyObject *) f;
err_exit:
    /* Clear members that could leak. */
    PyFrame_Type.tp_clear((PyObject*)f);

    return NULL;
}

PyFrameObject *
slp_clone_frame(PyFrameObject *f)
{
    PyObject *tup, *func, *args;
    PyFrameObject *fnew;

    if (PyFrame_Check(f))
        tup = frameobject_reduce(f, NULL);
    else
        tup = PyObject_CallMethod((PyObject *) f, "__reduce__", "");
    if (tup == NULL)
        return NULL;
    if (!PyTuple_Check(tup)) {
        PyErr_SetNone(PyExc_TypeError);
        goto error;
    }
    if (PyTuple_GET_SIZE(tup) < 2) {
        PyErr_SetNone(PyExc_ValueError);
        goto error;
    }
    func = PyTuple_GET_ITEM(tup, 0);
    args = PyTuple_GET_ITEM(tup, 1);
    fnew = (PyFrameObject *) PyObject_CallObject(func, args);
    if (fnew == NULL) {
        goto error;
    }
    if (PyTuple_GET_SIZE(tup) >= 3) {
        PyObject *tmp;
        args = PyTuple_GET_ITEM(tup, 2);
        tmp = PyObject_CallMethod((PyObject *) fnew, "__setstate__", "(O)", args);
        Py_XDECREF(tmp);
        if (tmp == NULL) {
            Py_DECREF(fnew);
            fnew = NULL;
        }
    }
    Py_DECREF(tup);
    return fnew;
error:
    Py_DECREF(tup);
    return NULL;
}

/*
 * return a usable reference to the frame.
 * If the frame doesn't come from unpickling,
 * a clone is created.
 * Otherwise, the frame is incref'd.
 */

PyFrameObject *
slp_ensure_new_frame(PyFrameObject *f)
{
    /* the type check for tasklets is included here for brevity */
    if (! (PyCFrame_Check(f) || PyFrame_Check(f)) ) {
        PyErr_SetString(PyExc_TypeError,
            "tasklet unpickle needs list of frames last parameter.");
        return NULL;
    }
    if ((PyObject *) f->f_back != Py_None) {
        f = slp_clone_frame(f);
        if (f==NULL) {
            return NULL;
        }
    }
    else {
        Py_INCREF(f);
    }
    Py_CLEAR(f->f_back);
    return f;
}

MAKE_WRAPPERTYPE(PyFrame_Type, frame, "frame", frameobject_reduce, frame_new, frame_setstate)

static int init_frametype(PyObject * mod)
{
    return slp_register_execute(&PyCFrame_Type, "channel_seq_callback",
                             slp_channel_seq_callback, SLP_REF_INVALID_EXEC(slp_channel_seq_callback))
        || slp_register_execute(&PyCFrame_Type, "slp_tp_init_callback",
                             slp_tp_init_callback, SLP_REF_INVALID_EXEC(slp_tp_init_callback))
        || init_type(&wrap_PyFrame_Type, initchain, mod);
}
#undef initchain
#define initchain init_frametype


/******************************************************

  pickling of tracebacks

 ******************************************************/

/*
 * Simplified version with full recursion.
 * This is fine, since we are making cPickle stackless.
 */

typedef PyTracebackObject tracebackobject;

static PyTypeObject wrap_PyTraceBack_Type;

static PyObject *
tb_reduce(tracebackobject * tb, PyObject *unused)
{
    PyObject *tup = NULL;
    PyObject *frame_reducer;
    const char *fmt = "(O()(OiiO))";

    if (tb->tb_next == NULL)
        fmt = "(O()(Oii))";
    frame_reducer = slp_reduce_frame(tb->tb_frame);
    if (frame_reducer == NULL)
        return NULL;

    tup = Py_BuildValue(fmt,
                        &wrap_PyTraceBack_Type,
                        frame_reducer, tb->tb_lasti, tb->tb_lineno, tb->tb_next);
    Py_DECREF(frame_reducer);
    return tup;
}

static PyObject *
tb_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    tracebackobject *tb;

    if (is_wrong_type(type)) return NULL;
    if (!PyArg_ParseTuple(args, ":traceback")) return NULL;

    tb = PyObject_GC_New(tracebackobject, &PyTraceBack_Type);
    if (tb != NULL) {
        tb->tb_next = NULL;
        tb->tb_frame = NULL;
        PyObject_GC_Track(tb);
        Py_TYPE(tb) = type;
    }
    return (PyObject *) tb;
}

static
PyObject *
tb_setstate(PyObject *self, PyObject *args)
{
    tracebackobject *tb = (tracebackobject*) self, *next = NULL;
    PyFrameObject *frame;
    int lasti, lineno;

    if (is_wrong_type(Py_TYPE(tb))) return NULL;

    if ((args = unwrap_frame_arg(args)) == NULL)  /* now args is a counted ref! */
        return NULL;

    if (!PyArg_ParseTuple(args,
                          "O!ii|O!:traceback",
                          &PyFrame_Type, &frame,
                          &lasti, &lineno,&PyTraceBack_Type, &next)) {
        Py_DECREF(args);
        return NULL;
    }
    Py_DECREF(args);

    Py_XINCREF(next);
    tb->tb_next = next;
    Py_XINCREF(frame);
    tb->tb_frame = frame;
    tb->tb_lasti = lasti;
    tb->tb_lineno = lineno;
    Py_TYPE(tb) = Py_TYPE(tb)->tp_base;

    if (frame != NULL && next != NULL && next->tb_frame != NULL &&
        (PyObject *)(next->tb_frame->f_back) == Py_None) {
        /* Reconstruct the f_back chain as far as possible. */
        next->tb_frame->f_back = frame;
        Py_INCREF(frame);
        Py_DECREF(Py_None);
    }

    Py_INCREF(self);
    return self;
}

MAKE_WRAPPERTYPE(PyTraceBack_Type, tb, "traceback", tb_reduce, tb_new, tb_setstate)

static int init_tracebacktype(PyObject * mod)
{
    return init_type(&wrap_PyTraceBack_Type, initchain, mod);
}
#undef initchain
#define initchain init_tracebacktype


/******************************************************

  pickling of modules

 ******************************************************/

static PyTypeObject wrap_PyModule_Type;


static PyObject *
module_reduce(PyObject * m, PyObject *unused)
{
    static PyObject *import = NULL;
    PyObject *modules = PyImport_GetModuleDict();
    const char *name = PyModule_GetName(m);

    if (name == NULL) return NULL;

    /* is this module maybe not imported? */
    if (PyDict_GetItemString(modules, name) == NULL)
        return Py_BuildValue("(O(s)O)",
                             &wrap_PyModule_Type,
                             PyModule_GetName(m),
                             PyModule_GetDict(m));

    if (import == NULL) {
        import = run_script("import importlib\n" "ret = importlib.import_module\n", "ret");
        if (import == NULL)
            return NULL;
    }
    return Py_BuildValue("O(s)", import, name);
    /* would be shorter, but the search result is quite arbitrary:
        tup = PyObject_GetAttrString(m, "__name__");
     */
}

typedef struct {
    PyObject_HEAD
    PyObject *md_dict;
    struct PyModuleDef *md_def;
    void *md_state;
} PyModuleObject;

static PyObject *
module_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *mod, *mod_dict = NULL, *dict = NULL;
    char *name;

    if (is_wrong_type(type)) return NULL;

    /*
     * This is a funny case, like "int(derivednumber)". Instead of creating
     * a module, we have to produce its name string, instead, since this is
     * the way to create a fresh basic module:
     */
    if (PyTuple_GET_SIZE(args) == 1 &&
        PyModule_Check(PyTuple_GET_ITEM(args, 0)) ) {
        mod = PyTuple_GET_ITEM(args, 0);
        return PyObject_GetAttrString(mod, "__name__");
    }
    else if (!PyArg_ParseTuple(args, "s|O!:module", &name,
                               &PyDict_Type, &dict))
        return NULL;
    mod = PyModule_New(name);
    if (mod != NULL)
        mod_dict = PyModule_GetDict(mod);
    if (mod_dict && dict && PyDict_Update(mod_dict, dict)) {
        Py_DECREF(mod);
        mod = NULL;
    }
    return mod;
}

MAKE_WRAPPERTYPE(PyModule_Type, module, "module", module_reduce, module_new, generic_setstate)

static int init_moduletype(PyObject * mod)
{
    return init_type(&wrap_PyModule_Type, initchain, mod);
}
#undef initchain
#define initchain init_moduletype



/******************************************************

  pickling of dictviews
  TODO: Is this really necessary?  Native 3.3 seems to get by without it.

 ******************************************************/


static PyTypeObject wrap_PyDictKeys_Type;

static PyObject *
dictview_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *inst;
    PyObject *dict;
    Py_ssize_t size = PyTuple_Size(args);

    if (size == -1) {
        return NULL;
    }
    if (size != 1) {
        PyErr_SetString(PyExc_ValueError, "args must contain exactly one argument");
        return NULL;
    }
    dict = PyTuple_GetItem(args, 0);
    if (dict == NULL)
        return NULL;
    inst = _PyDictView_New(dict, type->tp_base);
    if (inst != NULL)
        Py_TYPE(inst) = type;
    return (PyObject *)inst;
}

static PyObject *
dictkeysview_reduce(_PyDictViewObject *di, PyObject *unused)
{
    PyObject *tup;

    assert(di != NULL);
    tup = Py_BuildValue("(O(O)" NO_STATE_FORMAT ")",
                &wrap_PyDictKeys_Type,
                di->dv_dict
                NO_STATE_ARG);
    return tup;
}

static PyTypeObject wrap_PyDictValues_Type;

static PyObject *
dictvaluesview_reduce(_PyDictViewObject *di, PyObject *unused)
{
    PyObject *tup;

    assert(di != NULL);
    tup = Py_BuildValue("(O(O)" NO_STATE_FORMAT ")",
                &wrap_PyDictValues_Type,
                di->dv_dict
                NO_STATE_ARG);
    return tup;
}

static PyTypeObject wrap_PyDictItems_Type;

static PyObject *
dictitemsview_reduce(_PyDictViewObject *di, PyObject *unused)
{
    PyObject *tup;

    assert(di != NULL);
    tup = Py_BuildValue("(O(O)" NO_STATE_FORMAT ")",
                &wrap_PyDictItems_Type,
                di->dv_dict
                NO_STATE_ARG);
    return tup;
}

MAKE_WRAPPERTYPE(PyDictKeys_Type, dictkeysview, "dict_keys",
         dictkeysview_reduce, dictview_new, generic_setstate)

static int init_dictkeysviewtype(PyObject * mod)
{
    return init_type(&wrap_PyDictKeys_Type, initchain, mod);
}
#undef initchain
#define initchain init_dictkeysviewtype

MAKE_WRAPPERTYPE(PyDictValues_Type, dictvaluesview, "dict_values",
         dictvaluesview_reduce, dictview_new, generic_setstate)

static int init_dictvaluesviewtype(PyObject * mod)
{
    return init_type(&wrap_PyDictValues_Type, initchain, mod);
}
#undef initchain
#define initchain init_dictvaluesviewtype

MAKE_WRAPPERTYPE(PyDictItems_Type, dictitemsview, "dict_items",
         dictitemsview_reduce, dictview_new, generic_setstate)

static int init_dictitemsviewtype(PyObject * mod)
{
    return init_type(&wrap_PyDictItems_Type, initchain, mod);
}
#undef initchain
#define initchain init_dictitemsviewtype



/******************************************************

  pickling of generators, coroutines and asynchronous generators

 ******************************************************/

static PyTypeObject wrap_PyGen_Type;
static PyTypeObject wrap_PyCoro_Type;
static PyTypeObject wrap__PyCoroWrapper_Type;
static PyTypeObject wrap_PyAsyncGen_Type;
static PyTypeObject wrap__PyAsyncGenASend_Type;
static PyTypeObject wrap__PyAsyncGenAThrow_Type;

/* Used to initialize a generator created by gen_new. */
static PyFrameObject *gen_exhausted_frame = NULL;
static PyCoroObject *gen_exhausted_coro = NULL;
static PyAsyncGenObject *gen_exhausted_asyncgen = NULL;

/* A helper for pickling the _PyErr_StackItem* members of generator-like and tasklet
 * objects. This method returns a pointer to the object, that contains the
 * that contains *exc_info.
 */
PyObject *
slp_get_obj_for_exc_state(_PyErr_StackItem *exc_info) {
    PyObject *result;
    assert(exc_info);
    Py_BUILD_ASSERT(offsetof(PyTaskletObject, exc_state) == offsetof(PyGenObject, gi_exc_state));
    Py_BUILD_ASSERT(offsetof(PyTaskletObject, exc_state) == offsetof(PyCoroObject, cr_exc_state));
    Py_BUILD_ASSERT(offsetof(PyTaskletObject, exc_state) == offsetof(PyAsyncGenObject, ag_exc_state));

    result = (PyObject *)(((char *)exc_info) - offsetof(PyTaskletObject, exc_state));
    assert(PyTasklet_Check(result) ||
            PyGen_Check(result) ||
            PyObject_TypeCheck(result, &PyCoro_Type) ||
            PyObject_TypeCheck(result, &PyAsyncGen_Type));
    Py_INCREF(result);
    return result;
}

/*
 * Common code for reducing PyGenObject, PyCoroObject and PyAsyncGenObject
 */

typedef struct {
    PyObject *frame;
    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_traceback;
    PyObject *exc_info_obj;
    PyObject *code;
    PyObject *name;
    PyObject *qualname;
    char running;
} gen_obj_head_ty;

static void
gen_obj_head_clear(gen_obj_head_ty *goh)
{
    Py_DECREF(goh->frame);
    Py_DECREF(goh->exc_type);
    Py_DECREF(goh->exc_value);
    Py_DECREF(goh->exc_traceback);
    Py_DECREF(goh->exc_info_obj);
    Py_XDECREF(goh->code);
    Py_XDECREF(goh->name);
    Py_XDECREF(goh->qualname);
}

/* Reduce the frame of an (asynchronous) generator or a coroutine
 */
static int
reduce_to_gen_obj_head(gen_obj_head_ty *goh, PyFrameObject * frame, const _PyErr_StackItem *exc_state)
{
    goh->code = goh->name = goh->qualname = NULL;
    /* Pickle NULL as None. See gen_setstate() for the corresponding
     * unpickling code. */
    if (frame != NULL) {
        goh->frame = slp_reduce_frame(frame);
        if (goh->frame == NULL)
            return -1;
    }
    else {
        Py_INCREF(Py_None);
        goh->frame = Py_None;
    }

    assert(exc_state != NULL);
    if (exc_state->previous_item != NULL) {
        assert(exc_state->previous_item != &(_PyThreadState_GET()->exc_state));
        goh->exc_info_obj = slp_get_obj_for_exc_state(exc_state->previous_item);
        if (goh->exc_info_obj == NULL) {
            Py_DECREF(goh->frame);
            return -1;
        }
    }
    else {
        Py_INCREF(Py_None);
        goh->exc_info_obj = Py_None;
    }

    goh->exc_type = exc_state->exc_type;
    goh->exc_value = exc_state->exc_value;
    goh->exc_traceback = exc_state->exc_traceback;
    if (goh->exc_type == NULL) goh->exc_type = Py_None;
    if (goh->exc_value == NULL) goh->exc_value = Py_None;
    if (goh->exc_traceback == NULL) goh->exc_traceback = Py_None;
    Py_INCREF(goh->exc_type);
    Py_INCREF(goh->exc_value);
    Py_INCREF(goh->exc_traceback);
    return 0;
}

static int
setstate_from_gen_obj_head(const gen_obj_head_ty *goh,
    PyFrameObject **p_frame, _PyErr_StackItem *exc_state,
    PyObject **p_code, PyObject **p_name, PyObject **p_qualname, char *p_running)
{
    PyFrameObject *f, *old_frame;
    PyObject *code, *old_code;
    PyObject *old_type, *old_value, *old_traceback;
    PyObject *old_name, *old_qualname;

    if (goh->frame == Py_None) {
        /* No frame, generator is exhausted */
        f = NULL;

        /* Even if frame is NULL, code is still valid. Therefore
        * I set it to the code of the exhausted frame singleton.
        */
        assert(gen_exhausted_frame != NULL);
        assert(PyFrame_Check(gen_exhausted_frame));
        code = (PyObject *)gen_exhausted_frame->f_code;
    }
    else if (!PyFrame_Check(goh->frame)) {
        PyErr_SetString(PyExc_TypeError, "invalid frame object");
        return -1;
    }
    else {
        f = (PyFrameObject *)(goh->frame);
        if (!goh->running) {
            f = slp_ensure_new_frame(f);
            if (f == NULL) {
                return -1;
            }
        }
        else {
            Py_INCREF(f);
        }
        code = (PyObject *)f->f_code;
    }
    Py_INCREF(code);

    *p_running = goh->running;
    old_name = *p_name;
    Py_INCREF(goh->name);
    *p_name = goh->name;
    old_qualname = *p_qualname;
    Py_INCREF(goh->qualname);
    *p_qualname = goh->qualname;

    old_type = exc_state->exc_type;
    old_value = exc_state->exc_value;
    old_traceback = exc_state->exc_traceback;
    if (goh->exc_type != Py_None) {
        Py_INCREF(goh->exc_type);
        exc_state->exc_type = goh->exc_type;
    }
    else
        exc_state->exc_type = NULL;

    if (goh->exc_value != Py_None) {
        Py_INCREF(goh->exc_value);
        exc_state->exc_value = goh->exc_value;
    }
    else
        exc_state->exc_value = NULL;

    if (goh->exc_traceback != Py_None) {
        Py_INCREF(goh->exc_traceback);
        exc_state->exc_traceback = goh->exc_traceback;
    }
    else
        exc_state->exc_value = NULL;

    assert(exc_state->previous_item == NULL);
    if (goh->exc_info_obj == Py_None) {
        exc_state->previous_item = NULL;
    }
    else {
        /* Check the preconditions for the next assinment.
        *
        * The cast in the assignment is OK, because all possible concrete types of exc_info_obj
        * have the exec_state member at the same offset.
        */
        assert(PyTasklet_Check(goh->exc_info_obj) ||
            PyGen_Check(goh->exc_info_obj) ||
            PyObject_TypeCheck(goh->exc_info_obj, &PyCoro_Type) ||
            PyObject_TypeCheck(goh->exc_info_obj, &PyAsyncGen_Type));
        Py_BUILD_ASSERT(offsetof(PyTaskletObject, exc_state) == offsetof(PyGenObject, gi_exc_state));
        Py_BUILD_ASSERT(offsetof(PyTaskletObject, exc_state) == offsetof(PyCoroObject, cr_exc_state));
        Py_BUILD_ASSERT(offsetof(PyTaskletObject, exc_state) == offsetof(PyAsyncGenObject, ag_exc_state));
        /* Make sure, that *exc_info_obj stays alive after Py_DECREF(args) at the end of this function.
        */
        assert(Py_REFCNT(goh->exc_info_obj) > 1);
        exc_state->previous_item = &(((PyGenObject *)(goh->exc_info_obj))->gi_exc_state);
    }

    /*
    * The frame might now be initially unpickled (with PyNone as f_back),
    * or it is already chained into a tasklet.
    * Fortunately, we can simply leave it this way:
    * since gi_running is set, there is no way to continue the
    * generator without the corresponding tasklet.
    */
    old_frame = *p_frame;
    *p_frame = f;
    old_code = *p_code;
    *p_code = code;

    Py_XDECREF(old_type);
    Py_XDECREF(old_value);
    Py_XDECREF(old_traceback);
    Py_XDECREF(old_name);
    Py_XDECREF(old_qualname);
    Py_XDECREF(old_code);
    Py_XDECREF(old_frame);

    return 0;
}

/* The usual reduce method.
 */
static PyObject *
gen_reduce(PyGenObject *gen, PyObject *unused)
{
    PyObject *tup;
    gen_obj_head_ty goh;

    if (reduce_to_gen_obj_head(&goh, gen->gi_frame, &gen->gi_exc_state))
        return NULL;

    tup = Py_BuildValue("(O()(ObOOOOOO))",
        &wrap_PyGen_Type,
        goh.frame,
        gen->gi_running,
        gen->gi_name,
        gen->gi_qualname,
        goh.exc_type,
        goh.exc_value,
        goh.exc_traceback,
        goh.exc_info_obj
    );
    gen_obj_head_clear(&goh);
    return tup;
}

static PyObject *
gen_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyGenObject *gen;
    if (is_wrong_type(type)) return NULL;

    /* A reference to frame is stolen by PyGen_New. */
    assert(gen_exhausted_frame != NULL);
    assert(PyFrame_Check(gen_exhausted_frame));
    assert(type == &wrap_PyGen_Type);
    gen = (PyGenObject *)PyGen_NewWithQualName(slp_ensure_new_frame(gen_exhausted_frame), NULL, NULL);
    if (gen == NULL)
        return NULL;
    Py_TYPE(gen) = type;
    return (PyObject *) gen;
}

static PyObject *
gen_setstate(PyObject *self, PyObject *args)
{
    PyGenObject *gen = (PyGenObject *) self;
    gen_obj_head_ty goh;

    if (is_wrong_type(Py_TYPE(self))) return NULL;

    if ((args = unwrap_frame_arg(args)) == NULL)  /* now args is a counted ref! */
        return NULL;

    if (!PyArg_ParseTuple(args, "ObOOOOOO:generator",
                          &goh.frame,
                          &goh.running,
                          &goh.name,
                          &goh.qualname,
                          &goh.exc_type,
                          &goh.exc_value,
                          &goh.exc_traceback,
                          &goh.exc_info_obj)) {
        Py_DECREF(args);
        return NULL;
    }

    if (setstate_from_gen_obj_head(&goh,
        &gen->gi_frame, &gen->gi_exc_state, &gen->gi_code,
        &gen->gi_name, &gen->gi_qualname, &gen->gi_running)) {
        Py_DECREF(args);
        return NULL;
    }

    Py_TYPE(gen) = Py_TYPE(gen)->tp_base;
    Py_INCREF(gen);
    Py_DECREF(args); /* holds the frame and name refs */
    return (PyObject *)gen;
}

MAKE_WRAPPERTYPE(PyGen_Type, gen, "generator", gen_reduce,
                 gen_new, gen_setstate)

SLP_DEF_INVALID_EXEC(gen_iternext_callback)

static int init_generatortype(PyObject * mod)
{
    int res;
    PyGenObject *gen = (PyGenObject *) run_script(
        "def exhausted_generator(): yield 42\n"   /* define a generator */
        "g = exhausted_generator()\n"             /* instanciate it */
        "g.__next__()\n", "g");                       /* force callback frame creation */
    PyFrameObject *cbframe;

    if (gen == NULL || gen->gi_frame->f_back == NULL)
        return -1;
    cbframe = gen->gi_frame->f_back;
    assert(PyCFrame_Check(cbframe));
    res = slp_register_execute(Py_TYPE(cbframe), "gen_iternext_callback",
              ((PyCFrameObject *)cbframe)->f_execute,
              SLP_REF_INVALID_EXEC(gen_iternext_callback))
          || init_type(&wrap_PyGen_Type, initchain, mod);

    assert(gen_exhausted_frame == NULL);
    gen_exhausted_frame = slp_ensure_new_frame(gen->gi_frame);
    if (gen_exhausted_frame == NULL) {
        res = -1;
    } else {
        /* A reference to frame is stolen by PyGen_New. */
        Py_INCREF(gen_exhausted_frame);
        gen_exhausted_asyncgen = (PyAsyncGenObject *)PyAsyncGen_New(gen_exhausted_frame, NULL, NULL);
        /* A reference to frame is stolen by PyCoro_New. */
        Py_INCREF(gen_exhausted_frame);
        gen_exhausted_coro = (PyCoroObject *)PyCoro_New(gen_exhausted_frame, NULL, NULL);
    }
    if (gen_exhausted_asyncgen == NULL || gen_exhausted_coro == NULL) {
        res = -1;
    }

    Py_DECREF(gen);
    return res;
}
#undef initchain
#define initchain init_generatortype

static PyObject *
coro_reduce(PyCoroObject *coro, PyObject *unused)
{
    PyObject *tup;
    gen_obj_head_ty goh;
    PyObject *origin;

    if (reduce_to_gen_obj_head(&goh, coro->cr_frame, &coro->cr_exc_state))
        return NULL;

    origin = coro->cr_origin;
    assert(origin != Py_None);
    if (origin == NULL) {
        /* encode NULL as Py_None */
        origin = Py_None;  /* borrowed ref */
    }
    Py_INCREF(origin);

    tup = Py_BuildValue("(O()(ObOOOOOOO))",
        &wrap_PyCoro_Type,
        goh.frame,
        coro->cr_running,
        coro->cr_name,
        coro->cr_qualname,
        goh.exc_type,
        goh.exc_value,
        goh.exc_traceback,
        goh.exc_info_obj,
        origin
    );

    Py_DECREF(origin);
    gen_obj_head_clear(&goh);
    return tup;
}

static PyObject *
coro_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyCoroObject *coro;
    if (is_wrong_type(type)) return NULL;

    /* A reference to frame is stolen by PyGen_New. */
    assert(gen_exhausted_frame != NULL);
    assert(PyFrame_Check(gen_exhausted_frame));
    assert(type == &wrap_PyCoro_Type);
    coro = (PyCoroObject *)PyCoro_New(slp_ensure_new_frame(gen_exhausted_frame), NULL, NULL);
    if (coro == NULL)
        return NULL;
    Py_TYPE(coro) = type;
    return (PyObject *)coro;
}

static PyObject *
coro_setstate(PyObject *self, PyObject *args)
{
    PyCoroObject *coro = (PyCoroObject *)self;
    gen_obj_head_ty goh;
    PyObject *origin;

    if (is_wrong_type(Py_TYPE(self))) return NULL;

    if ((args = unwrap_frame_arg(args)) == NULL)  /* now args is a counted ref! */
        return NULL;

    if (!PyArg_ParseTuple(args, "ObOOOOOOO:coro_setstate",
        &goh.frame,
        &goh.running,
        &goh.name,
        &goh.qualname,
        &goh.exc_type,
        &goh.exc_value,
        &goh.exc_traceback,
        &goh.exc_info_obj,
        &origin)) {
        Py_DECREF(args);
        return NULL;
    }

    if (setstate_from_gen_obj_head(&goh,
        &coro->cr_frame, &coro->cr_exc_state, &coro->cr_code,
        &coro->cr_name, &coro->cr_qualname, &coro->cr_running)) {
        Py_DECREF(args);
        return NULL;
    }
    if (origin == Py_None)
        origin = NULL;  /* NULL is pickled as Py_None */
    else
        Py_INCREF(origin);
    Py_XSETREF(coro->cr_origin, origin);

    Py_TYPE(coro) = Py_TYPE(coro)->tp_base;
    Py_INCREF(coro);
    Py_DECREF(args); /* holds the frame and name refs */
    return (PyObject *)coro;
}

MAKE_WRAPPERTYPE(PyCoro_Type, coro, "coroutine", coro_reduce,
    coro_new, coro_setstate)

static int init_coroutinetype(PyObject * mod)
{
    return init_type(&wrap_PyCoro_Type, initchain, mod);
}
#undef initchain
#define initchain init_coroutinetype

static PyObject *
async_gen_reduce(PyAsyncGenObject *async_gen, PyObject *unused)
{
    PyThreadState *ts = _PyThreadState_GET();
    PyObject *tup;
    gen_obj_head_ty goh;
    PyObject *finalizer;
    int hooks_inited;

    if (reduce_to_gen_obj_head(&goh, async_gen->ag_frame, &async_gen->ag_exc_state))
        return NULL;

    if (ts->st.pickleflags & SLP_PICKLEFLAGS_PRESERVE_AG_FINALIZER) {
        hooks_inited = async_gen->ag_hooks_inited;
        finalizer = async_gen->ag_finalizer;
        assert(finalizer != Py_None);
        if (finalizer == NULL) {
            /* encode NULL as Py_None */
            finalizer = Py_None;  /* borrowed ref */
        }
    }
    else if (ts->st.pickleflags & SLP_PICKLEFLAGS_RESET_AG_FINALIZER) {
        hooks_inited = 0;
        finalizer = Py_None;
    }
    else {
        hooks_inited = async_gen->ag_hooks_inited;
        finalizer = async_gen->ag_finalizer != NULL ? Py_True : Py_None;
    }
    Py_INCREF(finalizer);

    tup = Py_BuildValue("(O()(ObOOOOOOOii))",
        &wrap_PyAsyncGen_Type,
        goh.frame,
        async_gen->ag_running,
        async_gen->ag_name,
        async_gen->ag_qualname,
        goh.exc_type,
        goh.exc_value,
        goh.exc_traceback,
        goh.exc_info_obj,
        finalizer,
        hooks_inited,
        async_gen->ag_closed
    );

    Py_DECREF(finalizer);
    gen_obj_head_clear(&goh);
    return tup;
}

static PyObject *
async_gen_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyAsyncGenObject *async_gen;
    if (is_wrong_type(type)) return NULL;

    /* A reference to frame is stolen by PyGen_New. */
    assert(gen_exhausted_frame != NULL);
    assert(PyFrame_Check(gen_exhausted_frame));
    assert(type == &wrap_PyAsyncGen_Type);
    async_gen = (PyAsyncGenObject *)PyAsyncGen_New(slp_ensure_new_frame(gen_exhausted_frame), NULL, NULL);
    if (async_gen == NULL)
        return NULL;
    Py_TYPE(async_gen) = type;
    return (PyObject *)async_gen;
}

static PyObject *
async_gen_setstate(PyObject *self, PyObject *args)
{
    PyThreadState *ts = _PyThreadState_GET();
    PyAsyncGenObject *async_gen = (PyAsyncGenObject *)self;
    gen_obj_head_ty goh;
    PyObject *finalizer;
    int closed;
    int hooks_inited;

    if (is_wrong_type(Py_TYPE(self))) return NULL;

    if ((args = unwrap_frame_arg(args)) == NULL)  /* now args is a counted ref! */
        return NULL;

    if (!PyArg_ParseTuple(args, "ObOOOOOOOii:async_gen_setstate",
        &goh.frame,
        &goh.running,
        &goh.name,
        &goh.qualname,
        &goh.exc_type,
        &goh.exc_value,
        &goh.exc_traceback,
        &goh.exc_info_obj,
        &finalizer,
        &hooks_inited,
        &closed)) {
        Py_DECREF(args);
        return NULL;
    }

    if (setstate_from_gen_obj_head(&goh,
        &async_gen->ag_frame, &async_gen->ag_exc_state, &async_gen->ag_code,
        &async_gen->ag_name, &async_gen->ag_qualname, &async_gen->ag_running)) {
        Py_DECREF(args);
        return NULL;
    }

    async_gen->ag_closed = closed;
    Py_TYPE(async_gen) = Py_TYPE(async_gen)->tp_base;

    if (ts->st.pickleflags & SLP_PICKLEFLAGS_PRESERVE_AG_FINALIZER &&
        finalizer != Py_True) {
        if (finalizer == Py_None) {
            /* NULL is pickled as Py_None */
            async_gen->ag_hooks_inited = hooks_inited;
            Py_CLEAR(async_gen->ag_finalizer);
        }
        else {
            async_gen->ag_hooks_inited = hooks_inited;
            Py_INCREF(finalizer);
            Py_XSETREF(async_gen->ag_finalizer, finalizer);
        }
    }
    else if (ts->st.pickleflags & SLP_PICKLEFLAGS_RESET_AG_FINALIZER ||
        !hooks_inited) {
        async_gen->ag_hooks_inited = 0;
        Py_CLEAR(async_gen->ag_finalizer);
    } else {
        assert(hooks_inited);
        async_gen->ag_hooks_inited = 0;
        if (slp_async_gen_init_hooks(async_gen)) {
            async_gen = NULL;
            goto error;
        }
    }

    Py_INCREF(async_gen);
error:
    Py_DECREF(args); /* holds the frame and name refs */
    return (PyObject *)async_gen;
}

MAKE_WRAPPERTYPE(PyAsyncGen_Type, async_gen, "async_generator", async_gen_reduce,
    async_gen_new, async_gen_setstate)

static int
init_async_gentype(PyObject * mod)
{
    return init_type(&wrap_PyAsyncGen_Type, initchain, mod);
}
#undef initchain
#define initchain init_async_gentype

static PyObject *
async_generator_asend_reduce(PyObject *o, PyObject *unused)
{
    return slp_async_gen_asend_reduce(o, &wrap__PyAsyncGenASend_Type);
}

static PyObject *
async_generator_asend_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject * o;
    if (is_wrong_type(type)) return NULL;
    assert(type == &wrap__PyAsyncGenASend_Type);
    assert(gen_exhausted_asyncgen != NULL);
    o = slp_async_gen_asend_new(gen_exhausted_asyncgen);
    if (o != NULL)
        Py_TYPE(o) = type;
    return o;
}

static PyObject *
async_generator_asend_setstate(PyObject *self, PyObject *args)
{
    if (is_wrong_type(Py_TYPE(self))) return NULL;
    return slp_async_gen_asend_setstate(self, args);
}

MAKE_WRAPPERTYPE(_PyAsyncGenASend_Type, asyncgen_asend, "async_generator_asend", async_generator_asend_reduce,
        async_generator_asend_new, async_generator_asend_setstate)

static int
init_async_generator_asend_type(PyObject * mod)
{
    return init_type(&wrap__PyAsyncGenASend_Type, initchain, mod);
}
#undef initchain
#define initchain init_async_generator_asend_type

static PyObject *
async_generator_athrow_reduce(PyObject *o, PyObject *unused)
{
    return slp_async_gen_athrow_reduce(o, &wrap__PyAsyncGenAThrow_Type);
}

static PyObject *
async_generator_athrow_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject * o;
    if (is_wrong_type(type)) return NULL;
    assert(type == &wrap__PyAsyncGenAThrow_Type);
    assert(gen_exhausted_asyncgen != NULL);
    o = slp_async_gen_athrow_new(gen_exhausted_asyncgen);
    if (o != NULL)
        Py_TYPE(o) = type;
    return o;
}

static PyObject *
async_generator_athrow_setstate(PyObject *self, PyObject *args)
{
    if (is_wrong_type(Py_TYPE(self))) return NULL;
    return slp_async_gen_athrow_setstate(self, args);
}

MAKE_WRAPPERTYPE(_PyAsyncGenAThrow_Type, asyncgen_athrow, "async_generator_athrow", async_generator_athrow_reduce,
        async_generator_athrow_new, async_generator_athrow_setstate)

static int
init_async_generator_athrow_type(PyObject * mod)
{
    return init_type(&wrap__PyAsyncGenAThrow_Type, initchain, mod);
}
#undef initchain
#define initchain init_async_generator_athrow_type

static PyObject *
coro_wrapper_reduce(PyObject *o, PyObject *unused)
{
    return slp_coro_wrapper_reduce(o, &wrap__PyCoroWrapper_Type);
}

static PyObject *
coro_wrapper_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject * o;
    if (is_wrong_type(type)) return NULL;
    assert(type == &wrap__PyCoroWrapper_Type);
    assert(gen_exhausted_coro != NULL);
    o = slp_coro_wrapper_new(gen_exhausted_coro);
    if (o != NULL)
        Py_TYPE(o) = type;
    return o;
}

static PyObject *
coro_wrapper_setstate(PyObject *self, PyObject *args)
{
    if (is_wrong_type(Py_TYPE(self))) return NULL;
    return slp_coro_wrapper_setstate(self, args);
}

MAKE_WRAPPERTYPE(_PyCoroWrapper_Type, coro_wrapper, "coroutine_wrapper", coro_wrapper_reduce,
    coro_wrapper_new, coro_wrapper_setstate)

static int init_coroutine_wrapper_type(PyObject * mod)
{
    return init_type(&wrap__PyCoroWrapper_Type, initchain, mod);
}
#undef initchain
#define initchain init_coroutine_wrapper_type

/******************************************************

  support code for module dict pickling

 ******************************************************/

PyObject *
PyStackless_Pickle_ModuleDict(PyObject *pickler, PyObject *self)
{
    PyObject *modict, *retval = NULL, *valuesview = NULL, *values = NULL;
    PyObject *mod = NULL, *id = NULL;

    modict = PyObject_GetAttrString(pickler, "module_dict_ids");
    if (modict == NULL) {
        PyObject *modules, *dict;

        PyErr_Clear();
        if ((modict = PyDict_New()) == NULL) goto finally;
        modules = PyImport_GetModuleDict();
        valuesview = PyDict_Values(modules);
        values = PyObject_CallMethod(valuesview, "__iter__", "()");
        for (;;) {
            mod = PyIter_Next(values);
            if (mod == NULL) {
                if (PyErr_Occurred()) goto finally;
                break;
            }
            if (!PyModule_Check(mod)) {
                Py_DECREF(mod);
                continue;
            }
            dict = PyModule_GetDict(mod);
            id = PyLong_FromVoidPtr(dict);
            if (id == NULL) goto finally;
            if (PyDict_SetItem(modict, id, mod)) goto finally;
            Py_DECREF(id);
            Py_DECREF(mod);
            id = mod = NULL;
        }
        if (PyObject_SetAttrString(pickler, "module_dict_ids", modict))
            goto finally;
    }
    if (!PyDict_Check(modict)) {
        PyErr_SetString(PyExc_TypeError,
                        "pickler: module_dict_ids is not a dictionary");
        goto finally;
    }
    {
        PyObject *thisid = PyLong_FromVoidPtr(self);
        PyObject *themodule;

        if (thisid == NULL) goto finally;
        if ((themodule = PyDict_GetItem(modict, thisid)) == NULL) {
            Py_INCREF(Py_None);
            retval = Py_None;
        }
        else {
            PyObject *builtins = PyEval_GetBuiltins();
            PyObject *getattr = PyDict_GetItemString(builtins, "getattr");
            if (getattr == NULL) goto finally;
            retval = Py_BuildValue("(O(Os))",
                getattr, themodule, "__dict__");
        }
        Py_DECREF(thisid);
    }
finally:
    Py_XDECREF(modict);
    Py_XDECREF(values);
    Py_XDECREF(valuesview);
    return retval;
}

char slp_pickle_moduledict__doc__[] = PyDoc_STR(
    "pickle_moduledict(pickler, dict) -- see if a dict is a global module\n"
    "dictionary. If so, the dict is not pickled, but a\n"
    "getattr(module, \"__dict__\") call isgenerated. As a side effect,\n"
    "the pickler gets a property 'module_dict_ids'\n"
    "which holds all id's of the current global module dictionaries.");

PyObject *
slp_pickle_moduledict(PyObject *self, PyObject *args)
{
    PyObject *pickler, *dict;
    if (!PyArg_ParseTuple(args, "OO!:moduledict_pickler",
                          &pickler,
                          &PyDict_Type, &dict))
        return NULL;
    return PyStackless_Pickle_ModuleDict(pickler, dict);
}

/******************************************************

  source module initialization

 ******************************************************/
static int
_wrapmodule_traverse(PyObject *self, visitproc visit, void *arg)
{
    Py_VISIT(gen_exhausted_frame);
    return 0;
}

static int
_wrapmodule_clear(PyObject *self)
{
    Py_CLEAR(gen_exhausted_frame);
    return 0;
}

static PyMethodDef _wrapmodule_methods[] = {
    {"set_reduce_frame", set_reduce_frame, METH_O, set_reduce_frame__doc__},
    {NULL,                          NULL}       /* sentinel */
};

static struct PyModuleDef _wrapmodule = {
    PyModuleDef_HEAD_INIT,
    "_stackless._wrap",
    NULL,
    -1,
    _wrapmodule_methods,
    NULL,
    _wrapmodule_traverse,
    _wrapmodule_clear,
    NULL
};

PyObject*
slp_init_prickelpit(void)
{
    PyObject *tmp;

    tmp = PyModule_Create(&_wrapmodule);
    if (tmp == NULL)
        return NULL;
    if (initchain(tmp)) {
        Py_CLEAR(tmp);
        return NULL;
    }
    return tmp;
}

#endif
