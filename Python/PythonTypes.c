/*++

Copyright (c) 2017 Trent Nelson <trent@trent.me>

Module Name:

    PythonTypes.c

Abstract:

    Define trace store types.

--*/

#include "stdafx.h"

//
// Define the trace store types owned by this module.
//

//
// N.B. Keep these sorted alphabetically.  In vim, mark the start and end brace
//      with ma and mb, then issue the command:
//
//          :'a,'b !sort -b -k 2
//

typedef struct _TRACE_STORE_TYPES {
    PPYTHON_FUNCTION PythonFunction;
    PPYTHON_FUNCTION_TABLE PythonFunctionTable;
    PPYTHON_FUNCTION_TABLE_ENTRY PythonFunctionTableEntry;
    PPYTHON_MODULE_TABLE PythonModuleTable;
    PPYTHON_MODULE_TABLE_ENTRY PythonModuleTableEntry;
    PPYTHON_PATH_TABLE PythonPathTable;
    PPYTHON_PATH_TABLE_ENTRY PythonPathTableEntry;
} TRACE_STORE_TYPES;
typedef TRACE_STORE_TYPES *PTRACE_STORE_TYPES;

//
// Type exposure functions.
//

DECLSPEC_DLLEXPORT
VOID
TraceStoreTypes(
    PTRACE_STORE_TYPES Types
    )
{
    UNREFERENCED_PARAMETER(Types);
}

// vim:set ts=8 sw=4 sts=4 tw=80 expandtab                                     :
