/*++

Copyright (c) 2016 Trent Nelson <trent@trent.me>

Module Name:

    PythonTracer.c

Abstract:

    This module implements functionality related to tracing an instance of a
    Python interpreter using the trace store functionality.  It relies on the
    Python and TraceStore components.

--*/

#include "stdafx.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL
InitializePythonTraceSession(
    _In_ PUNICODE_STRING BaseDirectory
    )
{
    return FALSE;
}

PVOID
TraceStoreAllocationRoutine(
    _In_ PVOID AllocationContext,
    _In_ const ULONG ByteSize
    )
{
    PTRACE_STORE TraceStore = (PTRACE_STORE)AllocationContext;
    ULARGE_INTEGER NumberOfRecords = { 1 };
    ULARGE_INTEGER RecordSize = { ByteSize };

    return TraceStore->AllocateRecords(
        TraceStore->TraceContext,
        TraceStore,
        &RecordSize,
        &NumberOfRecords
    );
}

PVOID
TraceStoreNullAllocationRoutine(
    _In_ PVOID AllocationContext,
    _In_ const ULONG ByteSize
    )
{
    __debugbreak();
    return NULL;
}

PVOID
TraceStoreCallocRoutine(
    _In_ PVOID AllocationContext,
    _In_ SIZE_T NumberOfElements,
    _In_ SIZE_T ElementSize
    )
{
    PTRACE_STORE TraceStore = (PTRACE_STORE)AllocationContext;
    ULARGE_INTEGER NumberOfRecords;
    ULARGE_INTEGER RecordSize;

    NumberOfRecords.QuadPart = NumberOfElements;
    RecordSize.QuadPart = ElementSize;

    return TraceStore->AllocateRecords(
        TraceStore->TraceContext,
        TraceStore,
        &RecordSize,
        &NumberOfRecords
    );
}

VOID
TraceStoreFreeRoutine(
    _In_opt_ PVOID FreeContext,
    _In_     PVOID Buffer
    )
{
    return;

    /*
    PTRACE_STORE TraceStore;
    TraceStore = (PTRACE_STORE)FreeContext;

    TraceStore->FreeRecords(TraceStore->TraceContext,
                            TraceStore,
                            Buffer);
    */
}

VOID
TraceStoreNullFreeRoutine(
    _In_opt_ PVOID FreeContext,
    _In_     PVOID Buffer
    )
{
    __debugbreak();
    return;
}

BOOL
IsFunctionOfInterestPrefixTree(
    _In_    PRTL                    Rtl,
    _In_    PPYTHON_TRACE_CONTEXT   Context,
    _In_    PPYTHON_FUNCTION        Function
    )
{
    PSTRING ModuleName;
    PPREFIX_TABLE Table;
    PPREFIX_TABLE_ENTRY Entry;

    if (Context->Flags.TraceEverything) {
        return TRUE;
    }

    if (Context->Flags.TraceNothing) {
        return FALSE;
    }

    if (!Context->RuntimeState.HasModuleFilter) {
        return Context->Flags.TraceEverythingWhenNoModuleFilterSet;
    }

    ModuleName = &Function->PathEntry.ModuleName;

    Table = &Context->ModuleFilterPrefixTree;

    Entry = Rtl->PfxFindPrefix(Table, ModuleName);

    return (Entry ? TRUE : FALSE);
}

#define IsFunctionOfInterest IsFunctionOfInterestStringTable

_Use_decl_annotations_
VOID
EnableMemoryTracing(
    PPYTHON_TRACE_CONTEXT   PythonTraceContext
    )
{
    PythonTraceContext->Flags.TraceMemory = TRUE;
}

_Use_decl_annotations_
VOID
DisableMemoryTracing(
    PPYTHON_TRACE_CONTEXT   PythonTraceContext
    )
{
    PythonTraceContext->Flags.TraceMemory = FALSE;
}

_Use_decl_annotations_
VOID
EnableIoCountersTracing(
    PPYTHON_TRACE_CONTEXT   PythonTraceContext
    )
{
    PythonTraceContext->Flags.TraceIoCounters = TRUE;
}

_Use_decl_annotations_
VOID
DisableIoCountersTracing(
    PPYTHON_TRACE_CONTEXT   PythonTraceContext
    )
{
    PythonTraceContext->Flags.TraceIoCounters = FALSE;
}

_Use_decl_annotations_
VOID
EnableHandleCountTracing(
    PPYTHON_TRACE_CONTEXT   PythonTraceContext
    )
{
    PythonTraceContext->Flags.TraceHandleCount = TRUE;
}

_Use_decl_annotations_
VOID
DisableHandleCountTracing(
    PPYTHON_TRACE_CONTEXT   PythonTraceContext
    )
{
    PythonTraceContext->Flags.TraceHandleCount = FALSE;
}

_Use_decl_annotations_
LONG
PyTraceCallback(
    PVOID           UserContext,
    PPYFRAMEOBJECT  FrameObject,
    LONG            EventType,
    PPYOBJECT       ArgObject
    )
{
    BOOL Success;
    BOOL InPageError;
    PPYTHON_TRACE_CONTEXT Context;

    Context = (PPYTHON_TRACE_CONTEXT)UserContext;
    InPageError = FALSE;

    TRY_MAPPED_MEMORY_OP {

        Success = Context->CallbackWorker(Context,
                                          FrameObject,
                                          EventType,
                                          ArgObject);

    } CATCH_STATUS_IN_PAGE_ERROR {
        InPageError = TRUE;
    }

    if (!Success) {
        __debugbreak();
    }

    if (InPageError) {
        OutputDebugStringA("PythonTracer: STATUS_IN_PAGE_ERROR, disabling.\n");
        Success = FALSE;
    }

    if (!Success) {
        Stop(Context);
    }

    return 0;
}

PVOID
NTAPI
CodeObjectAllocateFromStore(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ CLONG ByteSize
    )
{
    PPYTHON_TRACE_CONTEXT PythonTraceContext;
    if (!Table) {
        return NULL;
    }

    if (ByteSize <= 0) {
        return NULL;
    }

    PythonTraceContext = (PPYTHON_TRACE_CONTEXT)Table->TableContext;

    return NULL;
}

PVOID
NTAPI
CodeObjectAllocateFromHeap(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ CLONG ByteSize
    )
{
    PPYTHON_TRACE_CONTEXT PythonTraceContext;
    HANDLE HeapHandle;
    if (!Table) {
        return NULL;
    }

    if (ByteSize <= 0) {
        return NULL;
    }

    PythonTraceContext = (PPYTHON_TRACE_CONTEXT)Table->TableContext;

    HeapHandle = GetProcessHeap();
    if (!HeapHandle) {
        return NULL;
    }

    return HeapAlloc(HeapHandle, HEAP_ZERO_MEMORY, ByteSize);
}

VOID
NTAPI
CodeObjectFreeFromHeap(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ PVOID Buffer
    )
{
    PPYTHON_TRACE_CONTEXT PythonTraceContext;
    HANDLE HeapHandle;

    if (!Table) {
        return;
    }

    if (Buffer == NULL) {
        return;
    }

    PythonTraceContext = (PPYTHON_TRACE_CONTEXT)Table->TableContext;

    HeapHandle = GetProcessHeap();
    if (!HeapHandle) {
        return;
    }

    HeapFree(HeapHandle, 0, Buffer);
}

FORCEINLINE
RTL_GENERIC_COMPARE_RESULTS
GenericComparePointer(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ PVOID FirstStruct,
    _In_ PVOID SecondStruct
    )
{
    PULONG_PTR First = (PULONG_PTR)FirstStruct;
    PULONG_PTR Second = (PULONG_PTR)SecondStruct;

    if (First < Second) {
        return GenericLessThan;
    }
    else if (First > Second) {
        return GenericGreaterThan;
    }
    else {
        return GenericEqual;
    }
}

FORCEINLINE
RTL_GENERIC_COMPARE_RESULTS
GenericComparePyObjectHash(
    _In_ PPYTHON Python,
    _In_ PVOID FirstStruct,
    _In_ PVOID SecondStruct
    )
{
    PPYOBJECT First = (PPYOBJECT)FirstStruct;
    PPYOBJECT Second = (PPYOBJECT)SecondStruct;

    LONG FirstHash = Python->PyObject_Hash(First);
    LONG SecondHash = Python->PyObject_Hash(Second);

    if (First < Second) {
        return GenericLessThan;
    }
    else if (First > Second) {
        return GenericGreaterThan;
    }
    else {
        return GenericEqual;
    }
}

RTL_GENERIC_COMPARE_RESULTS
NTAPI
CodeObjectCompare(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ PVOID FirstStruct,
    _In_ PVOID SecondStruct
    )
{
    return GenericComparePointer(Table, FirstStruct, SecondStruct);
}

RTL_GENERIC_COMPARE_RESULTS
NTAPI
FunctionCompare(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ PVOID FirstStruct,
    _In_ PVOID SecondStruct
    )
{
    PPYTHON_FUNCTION First = (PPYTHON_FUNCTION)FirstStruct;
    PPYTHON_FUNCTION Second = (PPYTHON_FUNCTION)SecondStruct;

    return GenericComparePointer(Table,
                                 First->CodeObject,
                                 Second->CodeObject);
}


_Use_decl_annotations_
BOOL
InitializePythonTraceContext(
    PRTL Rtl,
    PALLOCATOR Allocator,
    PPYTHON_TRACE_CONTEXT Context,
    PULONG SizeOfContext,
    PPYTHON Python,
    PTRACE_CONTEXT TraceContext,
    PVOID UserData
    )
{
    BOOL Success;
    PTRACE_STORES TraceStores;
    PTRACE_STORE StringBufferStore;
    PTRACE_STORE FunctionTableStore;
    PTRACE_STORE FunctionTableEntryStore;
    PTRACE_STORE PathTableStore;
    PTRACE_STORE PathTableEntryStore;
    PTRACE_STORE StringArrayStore;
    PTRACE_STORE StringTableStore;
    PPY_TRACE_EVENT TraceEvent;
    PPY_TRACE_CALLBACK CallbackWorker;
    PYTHON_ALLOCATORS Allocators;
    ULONG NumberOfAllocators = 0;
    TRACE_STORE_ID TraceStoreId;
    ULONG Result;
    HKEY RegistryKey;

    //
    // Validate arguments.
    //

    if (!Context) {
        if (SizeOfContext) {
            *SizeOfContext = sizeof(*Context);
        }
        return FALSE;
    }

    if (!SizeOfContext) {
        return FALSE;
    }

    if (*SizeOfContext < sizeof(*Context)) {
        *SizeOfContext = sizeof(*Context);
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(Rtl)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(Allocator)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(Python)) {
        return FALSE;
    };

    if (!ARGUMENT_PRESENT(TraceContext)) {
        return FALSE;
    }

    //
    // Arguments are valid, continue with initialization.
    //

    SecureZeroMemory(Context, sizeof(*Context));

    //
    // Open the root registry key.
    //

    if (!OpenRootRegistryKey(&RegistryKey)) {
        return FALSE;
    }

    //
    // Read flags.
    //

    READ_REG_DWORD_FLAG(TraceMemory, FALSE);
    READ_REG_DWORD_FLAG(TraceIoCounters, FALSE);
    READ_REG_DWORD_FLAG(TraceHandleCount, FALSE);
    READ_REG_DWORD_FLAG(ProfileOnly, FALSE);
    READ_REG_DWORD_FLAG(TraceOnly, FALSE);
    READ_REG_DWORD_FLAG(TrackMaxRefCounts, FALSE);
    READ_REG_DWORD_FLAG(TraceEverything, FALSE);
    READ_REG_DWORD_FLAG(TraceNothing, FALSE);
    READ_REG_DWORD_FLAG(TraceEverythingWhenNoModuleFilterSet, FALSE);

    //
    // Read runtime parameters.
    //

    READ_REG_DWORD_RUNTIME_PARAM(TraceEventType, 2);
    READ_REG_DWORD_RUNTIME_PARAM(CallbackWorkerType, 1);

    RegCloseKey(RegistryKey);

    TraceEvent = GetFunctionPointerForTraceEventType(Context);
    if (!TraceEvent) {
        return FALSE;
    }

    CallbackWorker = GetFunctionPointerForCallbackWorkerType(Context);
    if (!CallbackWorker) {
        return FALSE;
    }

    //
    // Continue initialization.
    //

    Context->Size = *SizeOfContext;
    Context->Rtl = Rtl;
    Context->Allocator = Allocator;
    Context->Python = Python;
    Context->TraceContext = TraceContext;
    Context->CallbackWorker = CallbackWorker;
    Context->TraceEventFunction = TraceEvent;
    Context->UserData = UserData;

    Context->Depth = 0;
    Context->SkipFrames = 1;

    TraceStores = TraceContext->TraceStores;

#define INIT_STORE_ALLOCATOR(Name)                                       \
    TraceStoreId = TraceStore##Name##Id;                                 \
    Name##Store = TraceStoreIdToTraceStore(TraceStores, TraceStoreId);   \
    Allocators.##Name##.AllocationRoutine = TraceStoreAllocationRoutine; \
    Allocators.##Name##.AllocationContext = ##Name##Store;               \
    Allocators.##Name##.FreeRoutine = TraceStoreFreeRoutine;             \
    Allocators.##Name##.FreeContext = ##Name##Store;                     \
    NumberOfAllocators++;

#define INIT_NULL_ALLOCATOR(Name)                                            \
    Allocators.##Name##.AllocationRoutine = TraceStoreNullAllocationRoutine; \
    Allocators.##Name##.AllocationContext = NULL;                            \
    Allocators.##Name##.FreeRoutine = TraceStoreNullFreeRoutine;             \
    Allocators.##Name##.FreeContext = NULL;                                  \
    NumberOfAllocators++;

    //
    // Temporary hack: disable the stores we don't use.
    //

    INIT_NULL_ALLOCATOR(String);
    INIT_NULL_ALLOCATOR(HashedString);
    INIT_NULL_ALLOCATOR(Buffer);
    INIT_NULL_ALLOCATOR(HashedStringBuffer);
    INIT_NULL_ALLOCATOR(FilenameString);
    INIT_NULL_ALLOCATOR(FilenameStringBuffer);
    INIT_NULL_ALLOCATOR(DirectoryString);
    INIT_NULL_ALLOCATOR(DirectoryStringBuffer);

    INIT_STORE_ALLOCATOR(StringBuffer);
    INIT_STORE_ALLOCATOR(FunctionTable);
    INIT_STORE_ALLOCATOR(FunctionTableEntry);
    INIT_STORE_ALLOCATOR(PathTable);
    INIT_STORE_ALLOCATOR(PathTableEntry);
    INIT_STORE_ALLOCATOR(StringArray);
    INIT_STORE_ALLOCATOR(StringTable);

    Allocators.NumberOfAllocators = NumberOfAllocators;
    Allocators.SizeInBytes = sizeof(Allocators);

    if (!Python->SetPythonAllocators(Python, &Allocators)) {
        return FALSE;
    }

    Python->InitializePythonRuntimeTables(Python);

    QueryPerformanceFrequency(&Context->Frequency);

    Rtl->PfxInitialize(&Context->ModuleFilterPrefixTree);

    Context->Start = Start;
    Context->Stop = Stop;

    Context->EnableMemoryTracing = EnableMemoryTracing;
    Context->DisableMemoryTracing = DisableMemoryTracing;

    Context->EnableIoCountersTracing = EnableIoCountersTracing;
    Context->DisableIoCountersTracing = DisableIoCountersTracing;

    Context->EnableHandleCountTracing = EnableHandleCountTracing;
    Context->DisableHandleCountTracing = DisableHandleCountTracing;

    Context->AddModuleName = AddModuleName;
    Context->SetModuleNamesStringTable = SetModuleNamesStringTable;

    //
    // If we've been configured to track maximum reference counts, register an
    // extended atexit function that will reflect the maximum values we saw
    // in the registry on exit (if they exceed previously set maximums).
    //
    // (This is also a good test that our SetAtExitEx() machinery is working
    //  properly, which is why we call the global version instead of going
    //  through Rtl->RegisterAtExitEx().)
    //

    if (Context->Flags.TrackMaxRefCounts) {
        Success = AtExitEx(SaveMaxRefCountsAtExit,
                           NULL,
                           Context,
                           &Context->SaveMaxCountsAtExitEntry);
        if (!Success) {
            return FALSE;
        }
    }

    //
    // Do the same for general counters if applicable.
    //

    if (Context->Flags.CountEvents) {
        Success = AtExitEx(SaveCountsToLastRunAtExit,
                           NULL,
                           Context,
                           &Context->SaveCountsToLastRunAtExitEntry);
    } else {
        Success = TRUE;
    }

    return Success;
}

_Use_decl_annotations_
VOID
SaveMaxRefCountsAtExit(
    BOOL IsProcessTerminating,
    PPYTHON_TRACE_CONTEXT Context
    )
/*++

Routine Description:

    This routine is responsible for writing the maximum reference count values
    observed during a run to the registry on process exit.  It is called by the
    Rtl AtExitEx rundown functionality.  This routine is only called if the
    TrackMaxRefCounts flag has been set.

Arguments:

    IsProcessTerminating - Supplies a boolean value that indicates whether or
        not the process is terminating.  If FALSE, indicates that the library
        has been unloaded via FreeLibrary().

    Context - Supplies a pointer to the PYTHON_TRACE_CONTEXT structure that was
        registered when AtExitEx() was called.

Return Value:

    None.

--*/
{
    ULONG Result;
    HKEY RegistryKey;

    //
    // Validate arguments.
    //

    if (!Context) {
        return;
    }

    if (!OpenRootRegistryKey(&RegistryKey)) {
        return;
    }

    //
    // Write the max ref counts to the registry if they're greater than the
    // current version.
    //

#define UPDATE_COUNT(Name)                                      \
    UPDATE_MAX_REG_QWORD(RegistryKey,                           \
                         Name,                                  \
                         (ULONGLONG)Context->##Name##.QuadPart)

    UPDATE_COUNT(MaxNoneRefCount);
    UPDATE_COUNT(MaxTrueRefCount);
    UPDATE_COUNT(MaxZeroRefCount);
    UPDATE_COUNT(MaxFalseRefCount);

    //
    // Capture MaxDepth as well (despite it not technically being a reference
    // count; it's still useful to track).
    //

    UPDATE_COUNT(MaxDepth);

    //
    // Close the registry key.
    //

    RegCloseKey(RegistryKey);

}

_Use_decl_annotations_
VOID
SaveCountsToLastRunAtExit(
    BOOL IsProcessTerminating,
    PPYTHON_TRACE_CONTEXT Context
    )
/*++

Routine Description:

    This routine is responsible for writing various counters pertaining to an
    active trace session to the registry on process exit.  It is called by the
    Rtl AtExitEx rundown functionality.

Arguments:

    IsProcessTerminating - Supplies a boolean value that indicates whether or
        not the process is terminating.  If FALSE, indicates that the library
        has been unloaded via FreeLibrary().

    Context - Supplies a pointer to the PYTHON_TRACE_CONTEXT structure that was
        registered when AtExitEx() was called.

Return Value:

    None.

--*/
{
    HKEY RegistryKey;

    //
    // Validate arguments.
    //

    if (!Context) {
        return;
    }

    if (!OpenLastRunRegistryKey(&RegistryKey)) {
        return;
    }

    //
    // Define some convenience macros.
    //

#define WRITE_LARGE_INTEGER(Name)                          \
    WRITE_REG_QWORD(RegistryKey,                           \
                    Name,                                  \
                    (ULONGLONG)Context->##Name##.QuadPart)

#define WRITE_ULONGLONG(Name)          \
    WRITE_REG_QWORD(RegistryKey,       \
                    Name,              \
                    Context->##Name##)

    //
    // If we've been tracking max ref counts, write those values now.
    //
    // N.B. This differs slightly from SaveMaxRefCountsAtExit() in that we don't
    //      do the "max" check; as we're writing to LastRun, we just write the
    //      value directly without checking the existing value.
    //

    if (Context->Flags.TrackMaxRefCounts) {
        WRITE_LARGE_INTEGER(MaxNoneRefCount);
        WRITE_LARGE_INTEGER(MaxTrueRefCount);
        WRITE_LARGE_INTEGER(MaxZeroRefCount);
        WRITE_LARGE_INTEGER(MaxFalseRefCount);
    }

    WRITE_LARGE_INTEGER(MaxDepth);

    //
    // Write the counters.
    //

    WRITE_ULONGLONG(FramesTraced);
    WRITE_ULONGLONG(FramesSkipped);
    WRITE_ULONGLONG(NumberOfPythonCalls);
    WRITE_ULONGLONG(NumberOfPythonReturns);
    WRITE_ULONGLONG(NumberOfPythonExceptions);
    WRITE_ULONGLONG(NumberOfPythonLines);
    WRITE_ULONGLONG(NumberOfCCalls);
    WRITE_ULONGLONG(NumberOfCReturns);
    WRITE_ULONGLONG(NumberOfCExceptions);

    //
    // Close the registry key and return.
    //

    RegCloseKey(RegistryKey);
}

_Use_decl_annotations_
BOOL
Start(
    PPYTHON_TRACE_CONTEXT Context
    )
{
    PPYTHON Python;

    if (!Context) {
        return FALSE;
    }

    Python = Context->Python;

    if (!Python) {
        return FALSE;
    }

    if (!Context->Flags.ProfileOnly) {
        Context->RuntimeState.IsTracing = TRUE;
        Python->PyEval_SetTrace(PyTraceCallback, (PPYOBJECT)Context);
    }

    if (!Context->Flags.TraceOnly) {
        Context->RuntimeState.IsProfiling = TRUE;
        Python->PyEval_SetProfile(PyTraceCallback, (PPYOBJECT)Context);
    }

    return TRUE;
}

_Use_decl_annotations_
BOOL
Stop(
    PPYTHON_TRACE_CONTEXT Context
    )
{
    PPYTHON Python;

    if (!Context) {
        return FALSE;
    }

    Python = Context->Python;

    if (!Python) {
        return FALSE;
    }

    Context->RuntimeState.IsTracing = FALSE;
    Context->RuntimeState.IsProfiling = FALSE;

    Python->PyEval_SetTrace(NULL, NULL);
    Python->PyEval_SetProfile(NULL, NULL);

    return TRUE;
}

_Success_(return != 0)
BOOL
AddPrefixTableEntry(
    _In_  PPYTHON_TRACE_CONTEXT Context,
    _In_  PPYOBJECT             StringObject,
    _In_  PPREFIX_TABLE         PrefixTable,
    _Outptr_opt_result_nullonfailure_ PPPREFIX_TABLE_ENTRY  EntryPointer
    )
{
    PRTL Rtl;
    PPYTHON Python;
    STRING String;
    PSTRING Name;
    PPREFIX_TABLE_ENTRY Entry;
    ULONG AllocSize;
    PVOID Buffer;
    BOOL Success;

    Rtl = Context->Rtl;
    Python = Context->Python;

    //
    // Get a STRING representation of the incoming PyObject string name.
    //

    Success = WrapPythonStringAsString(Python,
                                       StringObject,
                                       &String);

    if (!Success) {
        return FALSE;
    }

    //
    // Make sure it's within our limits.
    //

    if (String.Length >= MAX_STRING) {
        return FALSE;
    }

    //
    // Make sure the entry isn't already in the tree.
    //

    Entry = Rtl->PfxFindPrefix(PrefixTable, &String);

    if (Entry) {

        //
        // Entry already exists, nothing more to do.  We don't check the
        // lengths here as our use case for this function is currently limited
        // to runtime manipulation of the module filter table, where a prefix
        // entry is sufficient to enable tracing for a module.
        //

        return TRUE;
    }

    //
    // Entry doesn't exist in the prefix table, allocate space for a
    // PREFIX_TABLE_ENTRY, the corresponding STRING, and the underlying buffer
    // (which we copy so that we can control ownership lifetime), plus 1 for
    // the trailing NUL.
    //

    AllocSize = (
        sizeof(PREFIX_TABLE_ENTRY)  +
        sizeof(STRING)              +
        String.Length               +
        1
    );

    AllocSize = ALIGN_UP_POINTER(AllocSize);

    if (!Python->AllocateBuffer(Python, AllocSize, &Buffer)) {
        return FALSE;
    }

    if (!Buffer) {
        return FALSE;
    }

    Entry = (PPREFIX_TABLE_ENTRY)Buffer;

    //
    // Point our STRING struct to after the PREFIX_TABLE_ENTRY.
    //

    Name = (PSTRING)(
        RtlOffsetToPointer(
            Buffer,
            sizeof(PREFIX_TABLE_ENTRY)
        )
    );

    //
    // And point the STRING's buffer to after the STRING struct.
    //

    Name->Buffer = (PCHAR)(
        RtlOffsetToPointer(
            Buffer,
            sizeof(PREFIX_TABLE_ENTRY) +
            sizeof(STRING)
        )
    );

    //
    // Fill in the name length details and copy the string over.
    //

    Name->Length = String.Length;
    Name->MaximumLength = String.Length+1;
    __movsb(Name->Buffer, String.Buffer, Name->Length);

    //
    // Add trailing NUL.
    //

    Name->Buffer[Name->Length] = '\0';

    //
    // Finally, add to the table.
    //

    Success = Rtl->PfxInsertPrefix(PrefixTable, Name, Entry);

    //
    // Update caller's pointer if applicable.
    //

    if (Success) {
        *EntryPointer = Entry;
    } else {

        //
        // Shouldn't be able to get here.
        //

        __debugbreak();
        return FALSE;
    }

    return TRUE;
}

_Use_decl_annotations_
BOOL
AddModuleName(
    PPYTHON_TRACE_CONTEXT   Context,
    PPYOBJECT               ModuleNameObject
    )
{
    BOOL Success;
    PPREFIX_TABLE_ENTRY PrefixTableEntry;

    if (!ARGUMENT_PRESENT(Context)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(ModuleNameObject)) {
        return FALSE;
    }

    Success = AddPrefixTableEntry(Context,
                                  ModuleNameObject,
                                  &Context->ModuleFilterPrefixTree,
                                  &PrefixTableEntry);

    if (Success) {
        Context->RuntimeState.HasModuleFilter = TRUE;
    }

    return Success;
}

_Use_decl_annotations_
BOOL
SetModuleNamesStringTable(
    PPYTHON_TRACE_CONTEXT Context,
    PSTRING_TABLE StringTable
    )
{
    PSTRING_TABLE ExistingTable;

    if (!ARGUMENT_PRESENT(Context)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(StringTable)) {
        return FALSE;
    }

    ExistingTable = Context->ModuleFilterStringTable;

    if (ExistingTable) {

        //
        // XXX todo: destroy existing table.
        //

    }

    Context->ModuleFilterStringTable = StringTable;

    Context->RuntimeState.HasModuleFilter = TRUE;

    return TRUE;
}

#ifdef __cplusplus
}; // extern "C"
#endif

// vim:set ts=8 sw=4 sts=4 tw=80 expandtab                                     :
