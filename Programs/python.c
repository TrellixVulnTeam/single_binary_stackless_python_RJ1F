/* Minimal main program -- everything is loaded from the library */

#include "Python.h"
#include "pycore_pylifecycle.h"

#ifdef MS_WINDOWS
#if Py_BUILD_RESOURCE_EMBEDDED_MODULE
#include <windows.h>

#define USER_COMMAND_ID 201


static wchar_t *overridden_argv[128];

static void
override_args(int argc, wchar_t **argv, int *new_argc, wchar_t ***new_argv)
{
    HRSRC resource_handle;
    HGLOBAL resource_data_handle;
    unsigned char *resource_data;
    uint32_t argc_resource, i;
    wchar_t *argv_resource;

    *new_argc = argc;
    *new_argv = argv;

    resource_handle = FindResource(NULL, MAKEINTRESOURCE(USER_COMMAND_ID), RT_RCDATA);
    if (resource_handle == NULL) {
        return;
    }

    resource_data_handle = LoadResource(NULL, resource_handle);
    if (resource_data_handle == NULL) {
        return;
    }

    resource_data = LockResource(resource_data_handle);
    if (resource_data == NULL) {
        return;
    }

    argc_resource = *(uint32_t *)resource_data;
    resource_data += sizeof(argc_resource);
    argv_resource = (wchar_t *)resource_data;

    if (argc_resource + argc + 1 > sizeof(overridden_argv)/sizeof(overridden_argv[0])) {
        return;
    }

    overridden_argv[0] = argv[0];
    for (i=0; i<argc_resource; i++) {
        overridden_argv[i + 1] = argv_resource;
        argv_resource += wcslen(argv_resource) + 1;
    }
    for (i=1; i<argc; i++) {
        overridden_argv[argc_resource + i] = argv[i];
    }

    *new_argc = argc_resource + argc;
    *new_argv = overridden_argv;
}
#endif

int
wmain(int argc, wchar_t **argv)
{
#if Py_BUILD_RESOURCE_EMBEDDED_MODULE
    int new_argc;
    wchar_t **new_argv;

    override_args(argc, argv, &new_argc, &new_argv);

    return Py_Main(new_argc, new_argv);
#else
    return Py_Main(argc, argv);
#endif
}
#else
int
main(int argc, char **argv)
{
    return Py_BytesMain(argc, argv);
}
#endif
