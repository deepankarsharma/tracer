/*++

Copyright (c) 2016 Trent Nelson <trent@trent.me>

Module Name:

    TraceStoreContext.c

Abstract:

    This module implements trace context functionality.  Functions are provided
    for initializing a trace context record, as well as binding a trace store to
    a trace context.

--*/

#include "stdafx.h"

_Use_decl_annotations_
BOOL
InitializeTraceContext(
    PRTL Rtl,
    PALLOCATOR Allocator,
    PTRACER_CONFIG TracerConfig,
    PTRACE_CONTEXT TraceContext,
    PULONG SizeOfTraceContext,
    PTRACE_SESSION TraceSession,
    PTRACE_STORES TraceStores,
    PTP_CALLBACK_ENVIRON ThreadpoolCallbackEnvironment,
    PTP_CALLBACK_ENVIRON CancellationThreadpoolCallbackEnvironment,
    PTRACE_CONTEXT_FLAGS TraceContextFlags,
    PVOID UserData
    )
/*++

Routine Description:

    This routine initializes a TRACE_CONTEXT structure.  This involves setting
    relevant fields in the structure then binding the context to the trace
    stores.

Arguments:

    Rtl - Supplies a pointer to an RTL structure.

    Allocator - Supplies a pointer to an ALLOCATOR structure.

    TracerConfig - Supplies a pointer to an initialized TRACER_CONFIG structure.
        This is used to obtain runtime parameter information.

    TraceContext - Supplies a pointer to a TRACE_CONTEXT structure.

    SizeOfTraceContext - Supplies a pointer to a variable that contains the
        buffer size allocated for the TraceContext parameter.  The actual size
        of the structure will be written to this variable.

    TraceSession - Supplies a pointer to a TRACE_SESSION structure.

    TraceStores - Supplies a pointer to a TRACE_STORES structure to bind the
        trace context to.

    ThreadpoolCallbackEnvironment - Supplies a pointer to a threadpool callback
        environment to use for the trace context.  This threadpool will be used
        to submit various asynchronous thread pool memory map operations.

    CancellationThreadpoolCallbackEnvironment - Supplies a pointer to a
        cancellation threadpool callback environment to use for the trace
        context.  Threadpool worker threads can submit work to this pool that
        trigger thread group cancellations safely.

    TraceContextFlags - Supplies an optional pointer to a TRACE_CONTEXT_FLAGS
        structure to use for the trace context.

    UserData - Supplies an optional pointer to user data that can be used by
        a caller to track additional context information per TRACE_CONTEXT
        structure.

Return Value:

    TRUE on success, FALSE on failure.  The required buffer size for the
    TRACE_CONTEXT structure can be obtained by passing in a valid pointer
    for SizeOfTraceContext and NULL for the remaining parameters.

--*/
{
    BOOL Success;
    BOOL IsReadonly;
    BOOL ManualReset;
    USHORT Index;
    USHORT StoreIndex;
    USHORT NumberOfTraceStores;
    USHORT NumberOfRemainingMetadataStores;
    ULONG NumberOfBytesToZero;
    DWORD Result;
    DWORD SpinCount;
    TRACE_STORE_TRAITS Traits;
    TRACE_CONTEXT_FLAGS ContextFlags;
    HANDLE Event;
    HANDLE ResumeAllocationEvent;
    HANDLE BindCompleteEvent;
    PTRACE_STORE_WORK Work;
    PTRACE_STORE TraceStore;
    PTRACER_RUNTIME_PARAMETERS RuntimeParameters;
    PALLOCATE_RECORDS_WITH_TIMESTAMP SuspendedAllocator;

    //
    // Validate size parameters.
    //

    if (!TraceContext) {
        if (SizeOfTraceContext) {
            *SizeOfTraceContext = sizeof(*TraceContext);
        }
        return FALSE;
    }

    if (!SizeOfTraceContext) {
        return FALSE;
    }

    if (*SizeOfTraceContext < sizeof(*TraceContext)) {
        *SizeOfTraceContext = sizeof(*TraceContext);
        return FALSE;
    }

    //
    // Validate arguments.
    //

    if (!ARGUMENT_PRESENT(Rtl)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(Allocator)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(TracerConfig)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(TraceStores)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(ThreadpoolCallbackEnvironment)) {
        return FALSE;
    }

    if (!ARGUMENT_PRESENT(CancellationThreadpoolCallbackEnvironment)) {
        return FALSE;
    }

    if (ARGUMENT_PRESENT(TraceContextFlags)) {
        ContextFlags = *TraceContextFlags;
        ContextFlags.Valid = FALSE;
    } else {
        SecureZeroMemory(&ContextFlags, sizeof(ContextFlags));
    }

    //
    // We zero the entire trace context structure, unless the caller has set the
    // IgnorePreferredAddresses context flag, in which case, we zero up to but
    // not including the first field related to the ignore bitmap.
    //

    NumberOfBytesToZero = sizeof(*TraceContext);

    //
    // Test the following invariants of the context flags:
    //  - If TraceContextFlags indicates readonly, TraceStores should as well.
    //  - If not readonly:
    //      - Ensure IgnorePreferredAddresses is not set.
    //      - Ensure TraceSession is not NULL.
    //  - Else:
    //      - If IgnorePreferredAddresses is set, ensure the bitmap is valid.
    //

    if (ContextFlags.Readonly) {

        IsReadonly = TRUE;

        //
        // Verify TraceStores is also readonly.
        //

        if (!TraceStores->Flags.Readonly) {
            return FALSE;
        }

        //
        // Verify the ignore bitmap if applicable.
        //

        if (ContextFlags.IgnorePreferredAddresses) {
            ULONG BitmapIndex;
            ULONG HintIndex;
            ULONG PreviousIndex;
            ULONG NumberOfSetBits;
            TRACE_STORE_ID TraceStoreId;
            PRTL_BITMAP Bitmap;

            //
            // Initialize bitmap alias.
            //

            Bitmap = &TraceContext->IgnorePreferredAddressesBitmap;

            //
            // The caller is responsible for initializing SizeOfBitMap.  Verify
            // it matches what we expect.
            //

            if (Bitmap->SizeOfBitMap != MAX_TRACE_STORE_IDS) {
                return FALSE;
            }

            //
            // Ensure the bitmap's buffer field is NULL; we set this ourselves.
            //

            if (Bitmap->Buffer) {
                return FALSE;
            }

            //
            // Initialize buffer pointer.
            //

            Bitmap->Buffer = (PULONG)&TraceContext->BitmapBuffer[0];

            //
            // Zero variables before loop.
            //

            BitmapIndex = 0;
            HintIndex = 0;
            PreviousIndex = 0;
            NumberOfSetBits = 0;

            //
            // Walk the bitmap and extract each bit, validate it is within
            // range, convert into a trace store ID, resolve the corresponding
            // trace store pointer, and set the store's IgnorePreferredAddresses
            // flag.  Fail early by returning FALSE on any erroneous conditions.
            //

            do {

                //
                // Extract the next bit from the bitmap.
                //

                BitmapIndex = Rtl->RtlFindSetBits(Bitmap, 1, HintIndex);

                //
                // Verify we got a sane index back.
                //

                if (BitmapIndex == BITS_NOT_FOUND ||
                    BitmapIndex >= TraceStoreInvalidId) {
                    return FALSE;
                }

                if (BitmapIndex <= PreviousIndex) {

                    //
                    // The search has wrapped, so exit the loop.
                    //

                    break;
                }

                //
                // The index is valid.  Convert to trace store ID, then resolve
                // the store's pointer and set the flag.  Update previous index
                // and hint index.
                //

                TraceStoreId = (TRACE_STORE_ID)BitmapIndex;
                TraceStore = TraceStoreIdToTraceStore(TraceStores,
                                                      TraceStoreId);
                TraceStore->IgnorePreferredAddresses = TRUE;

                PreviousIndex = BitmapIndex;
                HintIndex = BitmapIndex + 1;
                NumberOfSetBits++;

            } while (1);

            //
            // Sanity check that we saw at least one bit by this stage.
            //

            if (!NumberOfSetBits) {
                __debugbreak();
                return FALSE;
            }

            //
            // Adjust the number of bytes to zero such that we exclude the first
            // bitmap related field onward.
            //

            NumberOfBytesToZero = (
                FIELD_OFFSET(
                    TRACE_CONTEXT,
                    BitmapBufferSizeInQuadwords
                )
            );

        }

    } else {

        IsReadonly = FALSE;

        if (TraceStores->Flags.Readonly) {

            //
            // TraceStore is set readonly but context indicates otherwise.
            //

            return FALSE;

        } else if (!ARGUMENT_PRESENT(TraceSession)) {

            //
            // If we're not readonly, TraceSession must be provided.
            //

            return FALSE;
        }

        if (ContextFlags.IgnorePreferredAddresses) {

            //
            // IgnorePreferredAddresses only valid when readonly.
            //

            return FALSE;
        }
    }

    //
    // Zero the structure before we start using it.
    //

    SecureZeroMemory(TraceContext, NumberOfBytesToZero);

    TraceContext->TimerFunction = TraceStoreGetTimerFunction();
    if (!TraceContext->TimerFunction) {
        return FALSE;
    }

    //
    // Create a manual reset event for the loading complete state.
    //

    ManualReset = TRUE;
    TraceContext->LoadingCompleteEvent = CreateEvent(NULL,
                                                     ManualReset,
                                                     FALSE,
                                                     NULL);
    if (!TraceContext->LoadingCompleteEvent) {
        return FALSE;
    }

    TraceContext->ThreadpoolCallbackEnvironment = ThreadpoolCallbackEnvironment;

    TraceContext->CancellationThreadpoolCallbackEnvironment = (
        ThreadpoolCallbackEnvironment
    );

    TraceContext->ThreadpoolCleanupGroup = CreateThreadpoolCleanupGroup();
    if (!TraceContext->ThreadpoolCleanupGroup) {
        return FALSE;
    }

    SetThreadpoolCallbackCleanupGroup(
        ThreadpoolCallbackEnvironment,
        TraceContext->ThreadpoolCleanupGroup,
        NULL
    );

    if (!InitializeTraceStoreTime(Rtl, &TraceContext->Time)) {
        return FALSE;
    }

    TraceContext->SizeOfStruct = (USHORT)(*SizeOfTraceContext);
    TraceContext->TraceSession = TraceSession;
    TraceContext->TraceStores = TraceStores;
    TraceContext->UserData = UserData;
    TraceContext->Rtl = Rtl;
    TraceContext->Allocator = Allocator;
    TraceContext->TracerConfig = TracerConfig;

    TraceContext->Flags = ContextFlags;

    TraceContext->InitializeAllocatorFromTraceStore = (
        InitializeAllocatorFromTraceStore
    );

    NumberOfTraceStores = TraceStores->NumberOfTraceStores;

    //
    // We subtract 2 from ElementsPerTraceStore to account for the normal trace
    // store and :MetadataInfo trace store.
    //

    NumberOfRemainingMetadataStores = (
        (TraceStores->ElementsPerTraceStore - 2) *
        NumberOfTraceStores
    );

#define INIT_WORK(Name, NumberOfItems)                               \
    Work = &TraceContext->##Name##Work;                              \
                                                                     \
    InitializeSListHead(&Work->ListHead);                            \
                                                                     \
    Work->WorkCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL); \
    if (!Work->WorkCompleteEvent) {                                  \
        goto Error;                                                  \
    }                                                                \
                                                                     \
    Work->ThreadpoolWork = CreateThreadpoolWork(                     \
        Name##Callback,                                              \
        TraceContext,                                                \
        ThreadpoolCallbackEnvironment                                \
    );                                                               \
    if (!Work->ThreadpoolWork) {                                     \
        goto Error;                                                  \
    }                                                                \
                                                                     \
    Work->TotalNumberOfItems = NumberOfItems;                        \
    Work->NumberOfActiveItems = NumberOfItems;                       \
    Work->NumberOfFailedItems = 0

    INIT_WORK(BindMetadataInfoStore, NumberOfTraceStores);
    INIT_WORK(BindRemainingMetadataStores, NumberOfRemainingMetadataStores);
    INIT_WORK(BindTraceStore, NumberOfTraceStores);
    INIT_WORK(ReadonlyNonStreamingBindComplete, NumberOfTraceStores);

    TraceContext->BindsInProgress = NumberOfTraceStores;

    //
    // Initialize the failure singly-linked list head, and create a threadpool
    // work item for the cleanup threadpool members routine.  This allows main
    // threads to cleanly initiate threadpool cleanup without introducing any
    // racing/blocking issues.
    //

    InitializeSListHead(&TraceContext->FailedListHead);
    TraceContext->CleanupThreadpoolMembersWork = CreateThreadpoolWork(
        CleanupThreadpoolMembersCallback,
        TraceContext,
        CancellationThreadpoolCallbackEnvironment
    );

    if (!TraceContext->CleanupThreadpoolMembersWork) {
        goto Error;
    }

    if (IsReadonly) {

        //
        // Enumerate the trace stores and create the relocation complete events
        // first before any threadpool work is submitted.  These are used for
        // coordinating relocation synchronization between stores and must be
        // available for all stores as soon as the binding has been kicked off
        // for one store.  This is because a store could finish mapping itself
        // and be ready to process any relocations before the stores it is
        // dependent upon have finished loading themselves.  By using explicit
        // events and WaitForSingleObject/WaitForMultipleObjects (depending on
        // whether or not we're dependent on one or multiple stores), we avoid
        // any race conditions with regards to trace stores not being ready when
        // we want them.
        //

        FOR_EACH_TRACE_STORE(TraceStores, Index, StoreIndex) {

            //
            // N.B. We use ManualReset == TRUE because we want the event to
            //      stay signaled once relocation has been complete.  This
            //      ensures that other stores can call WaitForMultipleObjects
            //      at any time and pick up the signaled event.
            //

            Event = CreateEvent(NULL, ManualReset, FALSE, NULL);
            if (!Event) {
                goto Error;
            }

            TraceStores->RelocationCompleteEvents[Index] = Event;
        }

        goto InitializeAllocators;
    }

    //
    // Register the AtExitEx callback.
    //

    Success = Rtl->AtExitEx(TraceStoreAtExitEx,
                            NULL,
                            TraceContext,
                            &TraceContext->AtExitExEntry);
    if (!Success) {
        goto Error;
    }

    //
    // This is where we check any flags that correspond to threadpool timers
    // we need to initialize.
    //

    if (TraceStores->Flags.EnableWorkingSetTracing) {
        PTRACE_STORE WsWatchInfoExStore;

        //
        // Working set tracing has been enabled.  Initialize the slim lock and
        // create the threadpool timer that will be responsible for flushing
        // the working set changes periodically.
        //

        InitializeSRWLock(&TraceContext->WorkingSetChangesLock);
        TraceContext->GetWorkingSetChangesTimer = (
            CreateThreadpoolTimer(
                (PTP_TIMER_CALLBACK)GetWorkingSetChangesTimerCallback,
                (PVOID)TraceContext,
                (PTP_CALLBACK_ENVIRON)ThreadpoolCallbackEnvironment
            )
        );

        if (!TraceContext->GetWorkingSetChangesTimer) {
            goto Error;
        }

        //
        // Initialize the process for working set monitoring.  We need to do
        // this before GetWsChanges() or GetWsChangesEx() can be called.
        //

        if (!Rtl->K32InitializeProcessForWsWatch(GetCurrentProcess())) {
            TraceContext->LastError = GetLastError();
            goto Error;
        }


        //
        // Override the BindComplete method of the working set watch information
        // trace store such that the threadpool timer work can be kicked off as
        // soon as the backing trace store is available.
        //

        WsWatchInfoExStore = (
            TraceStoreIdToTraceStore(
                TraceStores,
                TraceStoreWsWatchInfoExId
            )
        );

        WsWatchInfoExStore->BindComplete = WsWatchInfoExStoreBindComplete;
    }

    if (TraceStores->Flags.EnablePerformanceTracing) {
        PTRACE_STORE PerformanceStore;

        //
        // Performance tracing has been enabled.  Initialize the slim lock and
        // create the threadpool timer that will be responsible for periodically
        // capturing system performance information.
        //

        InitializeSRWLock(&TraceContext->CapturePerformanceMetricsLock);
        TraceContext->CapturePerformanceMetricsTimer = (
            CreateThreadpoolTimer(
                (PTP_TIMER_CALLBACK)CapturePerformanceMetricsTimerCallback,
                (PVOID)TraceContext,
                (PTP_CALLBACK_ENVIRON)ThreadpoolCallbackEnvironment
            )
        );

        if (!TraceContext->CapturePerformanceMetricsTimer) {
            goto Error;
        }

        //
        // Override the BindComplete method of the performance trace store such
        // that the threadpool timer work can be kicked off as soon as the trace
        // store is available.
        //

        PerformanceStore = (
            TraceStoreIdToTraceStore(
                TraceStores,
                TraceStorePerformanceId
            )
        );

        PerformanceStore->BindComplete = PerformanceStoreBindComplete;
    }

    //
    // Intentional follow-on to InitializeAllocators.
    //

InitializeAllocators:

    //
    // Load the critical section spin count from runtime parameters.
    //

    RuntimeParameters = &TracerConfig->RuntimeParameters;
    SpinCount = (
        RuntimeParameters->ConcurrentAllocationsCriticalSectionSpinCount
    );

    //
    // Forcibly set all the trace stores' AllocateRecordsWithTimestamp function
    // pointers to the suspended version.  The normal allocator will be restored
    // once the bind complete successfully occurs.  This also requires creating
    // the resume allocations event now, as well as linking the trace store with
    // the trace context so the TraceStoreQueryPerformanceCounter() call made
    // within the suspended allocator will behave properly.
    //

    SuspendedAllocator = SuspendedTraceStoreAllocateRecordsWithTimestamp;

    FOR_EACH_TRACE_STORE(TraceStores, Index, StoreIndex) {

        //
        // N.B. We use ManualReset == TRUE here because we explicitly control
        //      the state of the resume allocation event via SetEvent() and
        //      ResetEvent().
        //

        ResumeAllocationEvent = CreateEvent(NULL, ManualReset, FALSE, NULL);
        if (!ResumeAllocationEvent) {
            goto Error;
        }

        //
        // N.B. We use ManualReset == TRUE for BindComplete because we always
        //      want this to stay signaled once the binding has been complete.
        //

        BindCompleteEvent = CreateEvent(NULL, ManualReset, FALSE, NULL);
        if (!BindCompleteEvent) {
            goto Error;
        }

        TraceStore = &TraceStores->Stores[StoreIndex];
        TraceStore->TraceContext = TraceContext;
        TraceStore->ResumeAllocationsEvent = ResumeAllocationEvent;
        TraceStore->BindCompleteEvent = BindCompleteEvent;

        TraceStore->AllocateRecords = TraceStoreAllocateRecords;
        TraceStore->AllocateRecordsWithTimestamp = SuspendedAllocator;
        TraceStore->SuspendedAllocateRecordsWithTimestamp = (
            SuspendedAllocator
        );

        //
        // Determine which allocator implementation to use based on the
        // page alignment trait.
        //

        Traits = *TraceStore->pTraits;
        if (WantsPageAlignment(Traits)) {
            TraceStore->AllocateRecordsWithTimestampImpl1 = (
                TraceStoreAllocatePageAlignedRecordsWithTimestampImpl
            );
        } else {
            TraceStore->AllocateRecordsWithTimestampImpl1 = (
                TraceStoreAllocateRecordsWithTimestampImpl
            );
        }

        //
        // If the trace store has the concurrent allocations trait set, we need
        // to set the Try* version of the allocators.
        //

        if (!HasConcurrentAllocations(Traits)) {
            continue;
        }

        TraceStore->TryAllocateRecords = TraceStoreTryAllocateRecords;
        TraceStore->TryAllocateRecordsWithTimestamp = (
            TraceStoreTryAllocateRecordsWithTimestamp
        );

        //
        // Adjust the allocators such that the concurrent one sits in front of
        // the standard Impl worker routine.
        //

        TraceStore->AllocateRecordsWithTimestampImpl2 = (
            TraceStore->AllocateRecordsWithTimestampImpl1
        );
        TraceStore->AllocateRecordsWithTimestampImpl1 = (
            ConcurrentTraceStoreAllocateRecordsWithTimestamp
        );

    }

    //
    // Submit the bind metadata info work items for each trace store to the
    // threadpool.
    //

    FOR_EACH_TRACE_STORE(TraceStores, Index, StoreIndex) {
        TraceStore = &TraceStores->Stores[StoreIndex];
        SubmitBindMetadataInfoWork(TraceContext, TraceStore);
    }

    //
    // If async initialization hasn't been disabled, return now.  Otherwise,
    // wait on the loading complete event.
    //

    if (!TracerConfig->Flags.DisableAsynchronousInitialization) {
        return TRUE;
    }

    Result = WaitForSingleObject(TraceContext->LoadingCompleteEvent, INFINITE);

    if (Result != WAIT_OBJECT_0) {

        //
        // We don't `goto Error` here because the error handling attempts to
        // close the threadpool work item.  If a wait fails, it may be because
        // the process is being run down (user canceled operation, something
        // else failed, etc), in which case, we don't need to do any threadpool
        // or event cleanup operations.
        //

        OutputDebugStringA("TraceContext: wait for LoadingComplete failed.\n");
        return FALSE;
    }

    //
    // If there were no failures, the result was successful.
    //

    if (TraceContext->FailedCount == 0) {
        return TRUE;
    }

    //
    // Intentional follow-on to error handling.
    //

Error:

#define CLEANUP_WORK(Name)                         \
    Work = &TraceContext->##Name##Work;            \
                                                   \
    if (Work->ThreadpoolWork) {                    \
        CloseThreadpoolWork(Work->ThreadpoolWork); \
    }                                              \
                                                   \
    if (Work->WorkCompleteEvent) {                 \
        CloseHandle(Work->WorkCompleteEvent);      \
    }                                              \
                                                   \
    SecureZeroMemory(Work, sizeof(*Work));

    CLEANUP_WORK(BindMetadataInfoStore);
    CLEANUP_WORK(BindRemainingMetadataStores);
    CLEANUP_WORK(BindTraceStore);
    CLEANUP_WORK(ReadonlyNonStreamingBindComplete);

    if (TraceContext->CleanupThreadpoolMembersWork) {
        CloseThreadpoolWork(TraceContext->CleanupThreadpoolMembersWork);
        TraceContext->CleanupThreadpoolMembersWork = NULL;
    }

    if (TraceContext->GetWorkingSetChangesTimer) {
        CloseThreadpoolTimer(TraceContext->GetWorkingSetChangesTimer);
        TraceContext->GetWorkingSetChangesTimer = NULL;
    }

    return FALSE;
}

_Use_decl_annotations_
BOOL
InitializeReadonlyTraceContext(
    PRTL Rtl,
    PALLOCATOR Allocator,
    PTRACER_CONFIG TracerConfig,
    PTRACE_CONTEXT TraceContext,
    PULONG SizeOfTraceContext,
    PTRACE_SESSION TraceSession,
    PTRACE_STORES TraceStores,
    PTP_CALLBACK_ENVIRON ThreadpoolCallbackEnvironment,
    PTP_CALLBACK_ENVIRON CancellationThreadpoolCallbackEnvironment,
    PTRACE_CONTEXT_FLAGS TraceContextFlags,
    PVOID UserData
    )
/*++

Routine Description:

    This routine initializes a readonly TRACE_CONTEXT structure.  It is a
    convenience method that is equivalent to calling InitializeTraceContext()
    with TraceContextFlags->Readonly set to TRUE.

Arguments:

    Rtl - Supplies a pointer to an RTL structure.

    Allocator - Supplies a pointer to an ALLOCATOR structure.

    TraceContext - Supplies a pointer to a TRACE_CONTEXT structure.

    SizeOfTraceContext - Supplies a pointer to a variable that contains the
        buffer size allocated for the TraceContext parameter.  The actual size
        of the structure will be written to this variable.

    TraceSession - This parameter is ignored and a NULL value is passed to
        InitializeTraceContext() instead.

    TraceStores - Supplies a pointer to a TRACE_STORES structure to bind the
        trace context to.

    ThreadpoolCallbackEnvironment - Supplies a pointer to a threadpool callback
        environment to use for the trace context.  This threadpool will be used
        to submit various asynchronous thread pool memory map operations.

    CancellationThreadpoolCallbackEnvironment - Supplies a pointer to a
        cancellation threadpool callback environment to use for the trace
        context.  Threadpool worker threads can submit work to this pool that
        trigger thread group cancellations safely.

    TraceContextFlags - Supplies an optional pointer to a TRACE_CONTEXT_FLAGS
        structure to use for the trace context.  The Readonly flag will always
        be set on the flags passed over to InitializeTraceContext().

    UserData - Supplies an optional pointer to user data that can be used by
        a caller to track additional context information per TRACE_CONTEXT
        structure.

Return Value:

    TRUE on success, FALSE on failure.  The required buffer size for the
    TRACE_CONTEXT structure can be obtained by passing in a valid pointer
    for SizeOfTraceContext and NULL for the remaining parameters.

--*/
{
    TRACE_CONTEXT_FLAGS Flags = { 0 };

    //
    // Load the caller's flags if the pointer is non-NULL.
    //

    if (ARGUMENT_PRESENT(TraceContextFlags)) {
        Flags = *TraceContextFlags;
    }

    //
    // Set the readonly flag.
    //

    Flags.Readonly = TRUE;

    return InitializeTraceContext(Rtl,
                                  Allocator,
                                  TracerConfig,
                                  TraceContext,
                                  SizeOfTraceContext,
                                  NULL,
                                  TraceStores,
                                  ThreadpoolCallbackEnvironment,
                                  CancellationThreadpoolCallbackEnvironment,
                                  &Flags,
                                  UserData);
}

_Use_decl_annotations_
VOID
CloseTraceContext(
    PTRACE_CONTEXT TraceContext
    )
/*++

Routine Description:

    This routine closes a previously initialized TRACE_CONTEXT structure.

Arguments:

    TraceContext - Supplies a pointer to a TRACE_CONTEXT structure to close.

Return Value:

    None.

--*/
{
    BOOL CancelPendingCallbacks = TRUE;

    //
    // Validate arguments.
    //

    if (!ARGUMENT_PRESENT(TraceContext)) {
        return;
    }

    //
    // Define a helper macro.
    //

#define CLOSE_THREADPOOL_TIMER(Name)                                    \
    if (TraceContext->##Name) {                                         \
        PTP_TIMER Timer = TraceContext->##Name;                         \
        BOOL CancelPendingCallbacks = TRUE;                             \
        SetThreadpoolTimer(Timer, NULL, 0, 0);                          \
        WaitForThreadpoolTimerCallbacks(Timer, CancelPendingCallbacks); \
        CloseThreadpoolTimer(Timer);                                    \
        TraceContext->##Name = NULL;                                    \
    }

    CLOSE_THREADPOOL_TIMER(GetWorkingSetChangesTimer);
    CLOSE_THREADPOOL_TIMER(CapturePerformanceMetricsTimer);

    return;
}

// vim:set ts=8 sw=4 sts=4 tw=80 expandtab                                     :
