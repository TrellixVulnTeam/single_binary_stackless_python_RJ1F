#ifndef Py_CPYTHON_OBJECT_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef STACKLESS
#include "cpython/slp_exttype.h"
#endif

/********************* String Literals ****************************************/
/* This structure helps managing static strings. The basic usage goes like this:
   Instead of doing

       r = PyObject_CallMethod(o, "foo", "args", ...);

   do

       _Py_IDENTIFIER(foo);
       ...
       r = _PyObject_CallMethodId(o, &PyId_foo, "args", ...);

   PyId_foo is a static variable, either on block level or file level. On first
   usage, the string "foo" is interned, and the structures are linked. On interpreter
   shutdown, all strings are released (through _PyUnicode_ClearStaticStrings).

   Alternatively, _Py_static_string allows choosing the variable name.
   _PyUnicode_FromId returns a borrowed reference to the interned string.
   _PyObject_{Get,Set,Has}AttrId are __getattr__ versions using _Py_Identifier*.
*/
typedef struct _Py_Identifier {
    struct _Py_Identifier *next;
    const char* string;
    PyObject *object;
} _Py_Identifier;

#define _Py_static_string_init(value) { .next = NULL, .string = value, .object = NULL }
#define _Py_static_string(varname, value)  static _Py_Identifier varname = _Py_static_string_init(value)
#define _Py_IDENTIFIER(varname) _Py_static_string(PyId_##varname, #varname)

/* buffer interface */
typedef struct bufferinfo {
    void *buf;
    PyObject *obj;        /* owned reference */
    Py_ssize_t len;
    Py_ssize_t itemsize;  /* This is Py_ssize_t so it can be
                             pointed to by strides in simple case.*/
    int readonly;
    int ndim;
    char *format;
    Py_ssize_t *shape;
    Py_ssize_t *strides;
    Py_ssize_t *suboffsets;
    void *internal;
} Py_buffer;

typedef int (*getbufferproc)(PyObject *, Py_buffer *, int);
typedef void (*releasebufferproc)(PyObject *, Py_buffer *);

typedef PyObject *(*vectorcallfunc)(PyObject *callable, PyObject *const *args,
                                    size_t nargsf, PyObject *kwnames);

/* Maximum number of dimensions */
#define PyBUF_MAX_NDIM 64

/* Flags for getting buffers */
#define PyBUF_SIMPLE 0
#define PyBUF_WRITABLE 0x0001
/*  we used to include an E, backwards compatible alias  */
#define PyBUF_WRITEABLE PyBUF_WRITABLE
#define PyBUF_FORMAT 0x0004
#define PyBUF_ND 0x0008
#define PyBUF_STRIDES (0x0010 | PyBUF_ND)
#define PyBUF_C_CONTIGUOUS (0x0020 | PyBUF_STRIDES)
#define PyBUF_F_CONTIGUOUS (0x0040 | PyBUF_STRIDES)
#define PyBUF_ANY_CONTIGUOUS (0x0080 | PyBUF_STRIDES)
#define PyBUF_INDIRECT (0x0100 | PyBUF_STRIDES)

#define PyBUF_CONTIG (PyBUF_ND | PyBUF_WRITABLE)
#define PyBUF_CONTIG_RO (PyBUF_ND)

#define PyBUF_STRIDED (PyBUF_STRIDES | PyBUF_WRITABLE)
#define PyBUF_STRIDED_RO (PyBUF_STRIDES)

#define PyBUF_RECORDS (PyBUF_STRIDES | PyBUF_WRITABLE | PyBUF_FORMAT)
#define PyBUF_RECORDS_RO (PyBUF_STRIDES | PyBUF_FORMAT)

#define PyBUF_FULL (PyBUF_INDIRECT | PyBUF_WRITABLE | PyBUF_FORMAT)
#define PyBUF_FULL_RO (PyBUF_INDIRECT | PyBUF_FORMAT)


#define PyBUF_READ  0x100
#define PyBUF_WRITE 0x200
/* End buffer interface */


typedef struct {
    /* Number implementations must check *both*
       arguments for proper type and implement the necessary conversions
       in the slot functions themselves. */

    binaryfunc nb_add;
    binaryfunc nb_subtract;
    binaryfunc nb_multiply;
    binaryfunc nb_remainder;
    binaryfunc nb_divmod;
    ternaryfunc nb_power;
    unaryfunc nb_negative;
    unaryfunc nb_positive;
    unaryfunc nb_absolute;
    inquiry nb_bool;
    unaryfunc nb_invert;
    binaryfunc nb_lshift;
    binaryfunc nb_rshift;
    binaryfunc nb_and;
    binaryfunc nb_xor;
    binaryfunc nb_or;
    unaryfunc nb_int;
    void *nb_reserved;  /* the slot formerly known as nb_long */
    unaryfunc nb_float;

    binaryfunc nb_inplace_add;
    binaryfunc nb_inplace_subtract;
    binaryfunc nb_inplace_multiply;
    binaryfunc nb_inplace_remainder;
    ternaryfunc nb_inplace_power;
    binaryfunc nb_inplace_lshift;
    binaryfunc nb_inplace_rshift;
    binaryfunc nb_inplace_and;
    binaryfunc nb_inplace_xor;
    binaryfunc nb_inplace_or;

    binaryfunc nb_floor_divide;
    binaryfunc nb_true_divide;
    binaryfunc nb_inplace_floor_divide;
    binaryfunc nb_inplace_true_divide;

    unaryfunc nb_index;

    binaryfunc nb_matrix_multiply;
    binaryfunc nb_inplace_matrix_multiply;
} PyNumberMethods;

typedef struct {
    lenfunc sq_length;
    binaryfunc sq_concat;
    ssizeargfunc sq_repeat;
    ssizeargfunc sq_item;
    void *was_sq_slice;
    ssizeobjargproc sq_ass_item;
    void *was_sq_ass_slice;
    objobjproc sq_contains;

    binaryfunc sq_inplace_concat;
    ssizeargfunc sq_inplace_repeat;
} PySequenceMethods;

typedef struct {
    lenfunc mp_length;
    binaryfunc mp_subscript;
    objobjargproc mp_ass_subscript;
#ifdef STACKLESS
    /* we put the stackless method flags in here.  This keeps the basic PyTypeObject size
     * constants and thus helps with compatibility with external types
     */
    slp_methodflags slpflags;
#endif
} PyMappingMethods;

#ifdef STACKLESS
/* we need the original object for inclusion in the PyHeapTypeObject */
typedef struct {
    lenfunc mp_length;
    binaryfunc mp_subscript;
    objobjargproc mp_ass_subscript;
} PyMappingMethods_Orig;
#endif

typedef struct {
    unaryfunc am_await;
    unaryfunc am_aiter;
    unaryfunc am_anext;
} PyAsyncMethods;

typedef struct {
     getbufferproc bf_getbuffer;
     releasebufferproc bf_releasebuffer;
} PyBufferProcs;

/* Allow printfunc in the tp_vectorcall_offset slot for
 * backwards-compatibility */
typedef Py_ssize_t printfunc;

typedef struct _typeobject {
    PyObject_VAR_HEAD
    const char *tp_name; /* For printing, in format "<module>.<name>" */
    Py_ssize_t tp_basicsize, tp_itemsize; /* For allocation */

    /* Methods to implement standard operations */

    destructor tp_dealloc;
    Py_ssize_t tp_vectorcall_offset;
    getattrfunc tp_getattr;
    setattrfunc tp_setattr;
    PyAsyncMethods *tp_as_async; /* formerly known as tp_compare (Python 2)
                                    or tp_reserved (Python 3) */
    reprfunc tp_repr;

    /* Method suites for standard classes */

    PyNumberMethods *tp_as_number;
    PySequenceMethods *tp_as_sequence;
    PyMappingMethods *tp_as_mapping;

    /* More standard operations (here for binary compatibility) */

    hashfunc tp_hash;
    ternaryfunc tp_call;
    reprfunc tp_str;
    getattrofunc tp_getattro;
    setattrofunc tp_setattro;

    /* Functions to access object as input/output buffer */
    PyBufferProcs *tp_as_buffer;

    /* Flags to define presence of optional/expanded features */
    unsigned long tp_flags;

    const char *tp_doc; /* Documentation string */

    /* Assigned meaning in release 2.0 */
    /* call function for all accessible objects */
    traverseproc tp_traverse;

    /* delete references to contained objects */
    inquiry tp_clear;

    /* Assigned meaning in release 2.1 */
    /* rich comparisons */
    richcmpfunc tp_richcompare;

    /* weak reference enabler */
    Py_ssize_t tp_weaklistoffset;

    /* Iterators */
    getiterfunc tp_iter;
    iternextfunc tp_iternext;

    /* Attribute descriptor and subclassing stuff */
    struct PyMethodDef *tp_methods;
    struct PyMemberDef *tp_members;
    struct PyGetSetDef *tp_getset;
    struct _typeobject *tp_base;
    PyObject *tp_dict;
    descrgetfunc tp_descr_get;
    descrsetfunc tp_descr_set;
    Py_ssize_t tp_dictoffset;
    initproc tp_init;
    allocfunc tp_alloc;
    newfunc tp_new;
    freefunc tp_free; /* Low-level free-memory routine */
    inquiry tp_is_gc; /* For PyObject_IS_GC */
    PyObject *tp_bases;
    PyObject *tp_mro; /* method resolution order */
    PyObject *tp_cache;
    PyObject *tp_subclasses;
    PyObject *tp_weaklist;
    destructor tp_del;

    /* Type attribute cache version tag. Added in version 2.6 */
    unsigned int tp_version_tag;

    destructor tp_finalize;
    vectorcallfunc tp_vectorcall;

    /* bpo-37250: kept for backwards compatibility in CPython 3.8 only */
    Py_DEPRECATED(3.8) int (*tp_print)(PyObject *, FILE *, int);

#ifdef COUNT_ALLOCS
    /* these must be last and never explicitly initialized */
    Py_ssize_t tp_allocs;
    Py_ssize_t tp_frees;
    Py_ssize_t tp_maxalloc;
    struct _typeobject *tp_prev;
    struct _typeobject *tp_next;
#endif
} PyTypeObject;

/* The *real* layout of a type object when allocated on the heap */
typedef struct _heaptypeobject {
    /* Note: there's a dependency on the order of these members
       in slotptr() in typeobject.c . */
    PyTypeObject ht_type;
    PyAsyncMethods as_async;
    PyNumberMethods as_number;
#ifdef STACKLESS
    PyMappingMethods_Orig as_mapping;
#else
    PyMappingMethods as_mapping;
#endif
    PySequenceMethods as_sequence; /* as_sequence comes after as_mapping,
                                      so that the mapping wins when both
                                      the mapping and the sequence define
                                      a given operator (e.g. __getitem__).
                                      see add_operators() in typeobject.c . */
    PyBufferProcs as_buffer;
    PyObject *ht_name, *ht_slots, *ht_qualname;
    struct _dictkeysobject *ht_cached_keys;
    /* here are optional user slots, followed by the members. */
} PyHeapTypeObject;

/* access macro to the members which are floating "behind" the object */
#define PyHeapType_GET_MEMBERS(etype) \
    ((PyMemberDef *)(((char *)etype) + Py_TYPE(etype)->tp_basicsize))

PyAPI_FUNC(const char *) _PyType_Name(PyTypeObject *);
PyAPI_FUNC(PyObject *) _PyType_Lookup(PyTypeObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyType_LookupId(PyTypeObject *, _Py_Identifier *);
PyAPI_FUNC(PyObject *) _PyObject_LookupSpecial(PyObject *, _Py_Identifier *);
PyAPI_FUNC(PyTypeObject *) _PyType_CalculateMetaclass(PyTypeObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyType_GetDocFromInternalDoc(const char *, const char *);
PyAPI_FUNC(PyObject *) _PyType_GetTextSignatureFromInternalDoc(const char *, const char *);

struct _Py_Identifier;
PyAPI_FUNC(int) PyObject_Print(PyObject *, FILE *, int);
PyAPI_FUNC(void) _Py_BreakPoint(void);
PyAPI_FUNC(void) _PyObject_Dump(PyObject *);
PyAPI_FUNC(int) _PyObject_IsFreed(PyObject *);

PyAPI_FUNC(int) _PyObject_IsAbstract(PyObject *);
PyAPI_FUNC(PyObject *) _PyObject_GetAttrId(PyObject *, struct _Py_Identifier *);
PyAPI_FUNC(int) _PyObject_SetAttrId(PyObject *, struct _Py_Identifier *, PyObject *);
PyAPI_FUNC(int) _PyObject_HasAttrId(PyObject *, struct _Py_Identifier *);
/* Replacements of PyObject_GetAttr() and _PyObject_GetAttrId() which
   don't raise AttributeError.

   Return 1 and set *result != NULL if an attribute is found.
   Return 0 and set *result == NULL if an attribute is not found;
   an AttributeError is silenced.
   Return -1 and set *result == NULL if an error other than AttributeError
   is raised.
*/
PyAPI_FUNC(int) _PyObject_LookupAttr(PyObject *, PyObject *, PyObject **);
PyAPI_FUNC(int) _PyObject_LookupAttrId(PyObject *, struct _Py_Identifier *, PyObject **);
PyAPI_FUNC(PyObject **) _PyObject_GetDictPtr(PyObject *);
PyAPI_FUNC(PyObject *) _PyObject_NextNotImplemented(PyObject *);
PyAPI_FUNC(void) PyObject_CallFinalizer(PyObject *);
PyAPI_FUNC(int) PyObject_CallFinalizerFromDealloc(PyObject *);

/* Same as PyObject_Generic{Get,Set}Attr, but passing the attributes
   dict as the last parameter. */
PyAPI_FUNC(PyObject *)
_PyObject_GenericGetAttrWithDict(PyObject *, PyObject *, PyObject *, int);
PyAPI_FUNC(int)
_PyObject_GenericSetAttrWithDict(PyObject *, PyObject *,
                                 PyObject *, PyObject *);

#define PyType_HasFeature(t,f)  (((t)->tp_flags & (f)) != 0)

/* Do not inline, if Stackless is compiled with SLP_WITH_FRAME_REF_DEBUG */
#if !defined(STACKLESS) || !defined(SLP_WITH_FRAME_REF_DEBUG)
static inline void _Py_Dealloc_inline(PyObject *op)
{
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
#ifdef Py_TRACE_REFS
    _Py_ForgetReference(op);
#else
    _Py_INC_TPFREES(op);
#endif
    (*dealloc)(op);
}
#define _Py_Dealloc(op) _Py_Dealloc_inline(op)
#endif   /* !defined(STACKLESS) || !defined(SLP_WITH_FRAME_REF_DEBUG) */


/* Safely decref `op` and set `op` to `op2`.
 *
 * As in case of Py_CLEAR "the obvious" code can be deadly:
 *
 *     Py_DECREF(op);
 *     op = op2;
 *
 * The safe way is:
 *
 *      Py_SETREF(op, op2);
 *
 * That arranges to set `op` to `op2` _before_ decref'ing, so that any code
 * triggered as a side-effect of `op` getting torn down no longer believes
 * `op` points to a valid object.
 *
 * Py_XSETREF is a variant of Py_SETREF that uses Py_XDECREF instead of
 * Py_DECREF.
 */

#define Py_SETREF(op, op2)                      \
    do {                                        \
        PyObject *_py_tmp = _PyObject_CAST(op); \
        (op) = (op2);                           \
        Py_DECREF(_py_tmp);                     \
    } while (0)

#define Py_XSETREF(op, op2)                     \
    do {                                        \
        PyObject *_py_tmp = _PyObject_CAST(op); \
        (op) = (op2);                           \
        Py_XDECREF(_py_tmp);                    \
    } while (0)


PyAPI_DATA(PyTypeObject) _PyNone_Type;
PyAPI_DATA(PyTypeObject) _PyNotImplemented_Type;

/* Maps Py_LT to Py_GT, ..., Py_GE to Py_LE.
 * Defined in object.c.
 */
PyAPI_DATA(int) _Py_SwappedOp[];

/* This is the old private API, invoked by the macros before 3.2.4.
   Kept for binary compatibility of extensions using the stable ABI. */
PyAPI_FUNC(void) _PyTrash_deposit_object(PyObject*);
PyAPI_FUNC(void) _PyTrash_destroy_chain(void);

PyAPI_FUNC(void)
_PyDebugAllocatorStats(FILE *out, const char *block_name, int num_blocks,
                       size_t sizeof_block);
PyAPI_FUNC(void)
_PyObject_DebugTypeStats(FILE *out);

/* Define a pair of assertion macros:
   _PyObject_ASSERT_FROM(), _PyObject_ASSERT_WITH_MSG() and _PyObject_ASSERT().

   These work like the regular C assert(), in that they will abort the
   process with a message on stderr if the given condition fails to hold,
   but compile away to nothing if NDEBUG is defined.

   However, before aborting, Python will also try to call _PyObject_Dump() on
   the given object.  This may be of use when investigating bugs in which a
   particular object is corrupt (e.g. buggy a tp_visit method in an extension
   module breaking the garbage collector), to help locate the broken objects.

   The WITH_MSG variant allows you to supply an additional message that Python
   will attempt to print to stderr, after the object dump. */
#ifdef NDEBUG
   /* No debugging: compile away the assertions: */
#  define _PyObject_ASSERT_FROM(obj, expr, msg, filename, lineno, func) \
    ((void)0)
#else
   /* With debugging: generate checks: */
#  define _PyObject_ASSERT_FROM(obj, expr, msg, filename, lineno, func) \
    ((expr) \
      ? (void)(0) \
      : _PyObject_AssertFailed((obj), Py_STRINGIFY(expr), \
                               (msg), (filename), (lineno), (func)))
#endif

#define _PyObject_ASSERT_WITH_MSG(obj, expr, msg) \
    _PyObject_ASSERT_FROM(obj, expr, msg, __FILE__, __LINE__, __func__)
#define _PyObject_ASSERT(obj, expr) \
    _PyObject_ASSERT_WITH_MSG(obj, expr, NULL)

#define _PyObject_ASSERT_FAILED_MSG(obj, msg) \
    _PyObject_AssertFailed((obj), NULL, (msg), __FILE__, __LINE__, __func__)

/* Declare and define _PyObject_AssertFailed() even when NDEBUG is defined,
   to avoid causing compiler/linker errors when building extensions without
   NDEBUG against a Python built with NDEBUG defined.

   msg, expr and function can be NULL. */
PyAPI_FUNC(void) _PyObject_AssertFailed(
    PyObject *obj,
    const char *expr,
    const char *msg,
    const char *file,
    int line,
    const char *function);

/* Check if an object is consistent. For example, ensure that the reference
   counter is greater than or equal to 1, and ensure that ob_type is not NULL.

   Call _PyObject_AssertFailed() if the object is inconsistent.

   If check_content is zero, only check header fields: reduce the overhead.

   The function always return 1. The return value is just here to be able to
   write:

   assert(_PyObject_CheckConsistency(obj, 1)); */
PyAPI_FUNC(int) _PyObject_CheckConsistency(
    PyObject *op,
    int check_content);

#ifdef __cplusplus
}
#endif
