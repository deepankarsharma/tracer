/*++

Copyright (c) 2016 Trent Nelson <trent@trent.me>

Module Name:

    PyTraceEvent2.c

Abstract:

    This module implements the tracing callback PyTraceEvent2, which uses the
    the PYTHON_TRACE_EVENT2 structure.

--*/

#include "stdafx.h"

#ifdef __cplusplus
extern "C" {
#endif

_Use_decl_annotations_
PPYTHON_TRACE_EVENT2
AllocatePythonTraceEvent2(
    PTRACE_STORE TraceStore,
    PLARGE_INTEGER Timestamp
    )
{
    ULARGE_INTEGER NumberOfRecords = { 1 };
    ULARGE_INTEGER RecordSize = { sizeof(PYTHON_TRACE_EVENT2) };

    return (PPYTHON_TRACE_EVENT2)(
        TraceStore->AllocateRecordsWithTimestamp(
            TraceStore->TraceContext,
            TraceStore,
            &RecordSize,
            &NumberOfRecords,
            Timestamp
        )
    );
}

_Check_return_
_Success_(return != 0)
PPYTHON_EVENT_TRAITS_EX
AllocatePythonEventTraitsEx(
    _In_ PTRACE_STORE TraceStore
    )
{
    ULARGE_INTEGER NumberOfRecords = { 1 };
    ULARGE_INTEGER RecordSize = { sizeof(PYTHON_EVENT_TRAITS_EX) };

    return (PPYTHON_EVENT_TRAITS_EX)(
        TraceStore->AllocateRecordsWithTimestamp(
            TraceStore->TraceContext,
            TraceStore,
            &RecordSize,
            &NumberOfRecords,
            NULL
        )
    );
}

_Use_decl_annotations_
BOOL
PyTraceEvent2(
    PPYTHON_TRACE_CONTEXT Context,
    PPYTHON_FUNCTION Function,
    PPYTHON_EVENT_TRAITS EventTraits,
    PPYFRAMEOBJECT FrameObject,
    PPYOBJECT ArgObject
    )
{
    PYTHON_EVENT_TRAITS_EX EventTraitsEx;
    PPYTHON_EVENT_TRAITS_EX EventTraitsExPointer;

    PPYTHON Python;
    PTRACE_CONTEXT TraceContext;
    PTRACE_STORES TraceStores;
    PTRACE_STORE EventStore;
    PTRACE_STORE EventTraitsExStore;
    PPYTHON_TRACE_EVENT2 Event;
    LARGE_INTEGER Elapsed;
    LARGE_INTEGER Timestamp;

    Python = Context->Python;

    //
    // Load the event and event traits ex trace stores.
    //

    TraceContext = Context->TraceContext;
    TraceStores = TraceContext->TraceStores;
    EventStore = TraceStoreIdToTraceStore(TraceStores, TraceStoreEventId);
    EventTraitsExStore = TraceStoreIdToTraceStore(TraceStores,
                                                  TraceStoreEventTraitsExId);

    //
    // Save the timestamp for this event.
    //

    TraceContextQueryPerformanceCounter(TraceContext, &Elapsed, &Timestamp);

    //
    // Allocate an event.
    //

    Event = AllocatePythonTraceEvent2(EventStore, &Timestamp);

    if (!Event) {
        return FALSE;
    }

    //
    // Allocate a traits event.
    //

    EventTraitsExPointer = AllocatePythonEventTraitsEx(EventTraitsExStore);

    if (!EventTraitsExPointer) {
        return FALSE;
    }

    //
    // Fill out the event and traits.
    //

    Event->Function = Function;

    EventTraitsEx.AsLong = (ULONG)EventTraits->AsByte;
    EventTraitsEx.LineNumberOrCallStackDepth = (ULONG)(
        EventTraits->IsLine ?
            Python->PyFrame_GetLineNumber(FrameObject) :
            Context->Depth
    );

    EventTraitsExPointer->AsLong = EventTraitsEx.AsLong;

    return TRUE;
}

#ifdef __cplusplus
}; // extern "C"
#endif

// vim:set ts=8 sw=4 sts=4 tw=80 expandtab                                     :
