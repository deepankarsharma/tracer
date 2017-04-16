        title "Injection Thunk Assembly Routine"

;++
;
; Copyright (c) 2017 Trent Nelson <trent@trent.me>
;
; Module Name:
;
;   InjectionThunk4.asm
;
; Abstract:
;
;   This module implements the injection thunk routine as a NESTED_ENTRY.
;
;--

include ksamd64.inc

;
; Define a home parameter + return address structure.  Before alloc_stack in
; the prologue, this struct is used against rsp.  After rsp, we use rbp.
;

Home struct
        ReturnAddress   dq      ?       ; 8     32      40      (28h)
        union
            Thunk       dq      ?       ; 8     24      32      (20h)
            SavedRcx    dq      ?       ; 8     24      32      (20h)
            Param1      dq      ?       ; 8     24      32      (20h)
        ends
        union
            SavedR12    dq      ?       ; 8     16      24      (18h)
            Param2      dq      ?       ; 8     16      24      (18h)
        ends
        union
            SavedRbp    dq      ?       ; 8     8       16      (10h)
            SavedR8     dq      ?       ; 8     8       16      (10h)
            Param3      dq      ?       ; 8     8       16      (10h)
        ends
        union
            SavedR9     dq      ?       ; 8     0       8       (08h)
            Param4      dq      ?       ; 8     0       8       (08h)
        ends
Home ends

;
; Define the RTL_INJECTION_THUNK_CONTEXT structure.
;

AddRuntimeFunction equ 1

Thunk struct
        Flags               dd      ?
        EntryCount          dd      ?
        FunctionTable       dq      ?
        BaseAddress         dq      ?
        RtlAddFunctionTable dq      ?
        ContextAddress      dq      ?
        BasePointer         dq      ?
        StackPointer        dq      ?
        ReturnAddress       dq      ?
        ExitThread          dq      ?
        LoadLibraryW        dq      ?
        GetProcAddress      dq      ?
        ModulePath          dq      ?
        ModuleHandle        dq      ?
        FunctionName        dq      ?
        FunctionAddress     dq      ?
Thunk ends

;
; Define error codes.
;

RtlAddFunctionTableFailed   equ     -1
LoadLibraryWFailed          equ     -2
GetProcAddressFailed        equ     -3

;++
;
; LONG
; InjectionThunk4(
;     _In_ PRTL_INJECTION_THUNK_CONTEXT Thunk
;     );
;
; Routine Description:
;
;   This routine is the initial entry point of our injection logic.  That is,
;   newly created remoted threads in a target process have their start address
;   set to a copy of this routine (that was prepared in a separate process and
;   then injected via WriteProcessMemory()).
;
;   It is responsible for registering a runtime function for itself, such that
;   appropriate unwind data can be found by the kernel if an exception occurs
;   and the stack is being unwound.  It then loads a library designated by the
;   fully qualified path in the thunk's module path field via LoadLibraryW,
;   then calls GetProcAddress on the returned module for the function name also
;   contained within the thunk.  If an address is successfully resolved, the
;   routine is called with the thunk back as the first parameter, and the return
;   value is propagated back to our caller (typically, this will be the routine
;   kernel32!UserBaseInitThunk).
;
;   In practice, the module path we attempt to load is InjectionThunk.dll, and
;   the function name we resolve is "InjectionRemoteThreadEntry".  This routine
;   is responsible for doing more heavy lifting in C prior to calling the actual
;   caller's end routine.
;
; Arguments:
;
;   Thunk (rcx) - Supplies a pointer to the injection context thunk.
;
; Return Value:
;
;   If an error occured in this routine, an error code (see above) will be
;   returned (ranging in value from -1 to -3).  If this routine succeeded,
;   the return value of the function we were requested to execute will be
;   returned instead.  (Unfortunately, there's no guarantee that this won't
;   overlap with our error codes.)
;
;   This return value will end up as the exit code for the thread if being
;   called in the injection context.
;
;--

        NESTED_ENTRY InjectionThunk4, _TEXT$00

;
; This routine uses the non-volatile r12 to store the Thunk pointer (initially
; passed in via rcx).  As we only have one parameter, we use the home parameter
; space reserved for rdx for saving r12 (instead of pushing it to the stack).
;

        save_reg r12, Home.SavedR12             ; Use rdx home to save r12.

;
; We use rbp as our frame pointer.  As with r12, we repurpose r8's home area
; instead of pushing to the stack, then call set_frame (which sets rsp to rbp).
;

        save_reg    rbp, Home.SavedRbp          ; Use r8 home to save rbp.
        set_frame   rbp, 0                      ; Use rbp as frame pointer.

;
; As we are calling other functions, we need to reserve 32 bytes for their
; parameter home space.
;

        alloc_stack 20h                         ; Reserve home param space.

        END_PROLOGUE

;
; Home our Thunk parameter register, then save in r12.  The homing of rcx isn't
; technically necessary (as we never re-load it from rcx), but it doesn't hurt,
; and it is useful during development and debugging to help detect anomalies
; (if we clobber r12 accidentally, for example).
;

        mov     Home.Thunk[rbp], rcx            ; Home Thunk (rcx) parameter.
        mov     r12, rcx                        ; Move Thunk into r12.

;
; Determine if we need to register a runtime function entry for this thunk.
;

        mov     r8d, Thunk.Flags                ; Load flags into r8d.
        test    r8d, AddRuntimeFunction         ; Is flag set?
        jz      Inj20                           ; No, jump to main logic.

;
; Register a runtime function for this currently executing piece of code.  This
; is done when we've been copied into memory at runtime.
;

        mov     rcx, Thunk.FunctionTable[r12]           ; Load FunctionTable.
        xor     rdx, rdx                                ; Clear rdx.
        mov     edx, dword ptr Thunk.EntryCount[r12]    ; Load EntryCount.
        mov     r8, Thunk.BaseAddress[r12]              ; Load BaseAddress.
        call    Thunk.RtlAddFunctionTable[r12]          ; Invoke function.
        test    rax, rax                                ; Check result.
        jz      short Inj10                             ; Function failed.
        jmp     short Inj20                             ; Function succeeded.

Inj10:  mov     rax, RtlAddFunctionTableFailed          ; Load error code.
        jmp     short Inj90                             ; Jump to epilogue.

;
; Prepare for a LoadLibraryW() call against the module path in the thunk.
;

Inj20:  mov     rcx, Thunk.ModulePath[r12]              ; Load ModulePath.
        call    Thunk.LoadLibraryW[r12]                 ; Call LoadLibraryW().
        test    rax, rax                                ; Check Handle != NULL.
        jz      short Inj30                             ; Handle is NULL.
        jmp     short Inj40                             ; Handle is valid.

Inj30:  mov     rax, LoadLibraryWFailed                 ; Load error code.
        jmp     short Inj90                             ; Jump to epilogue.

;
; Module was loaded successfully.  The Handle value lives in rax.  Save a copy
; in the thunk, then prepare arguments for a call to GetProcAddress().
;

Inj40:  mov     Thunk.ModuleHandle[r12], rax            ; Save Handle.
        mov     rcx, rax                                ; Load as 1st param.
        mov     rdx, Thunk.FunctionName[r12]            ; Load name as 2nd.
        call    Thunk.GetProcAddress[r12]               ; Call GetProcAddress.
        test    rax, rax                                ; Check return value.
        jz      short Inj50                             ; Lookup failed.
        jmp     short Inj60                             ; Lookup succeeded.

Inj50:  mov     rax, GetProcAddressFailed               ; Load error code.
        jmp     short Inj90                             ; Jump to return.

;
; The function name was resolved successfully.  The function address lives in
; rax.  Save a copy in the thunk, and then prepare arguments for a call to it.
;

Inj60:  mov     Thunk.FunctionAddress[r12], rax         ; Save func ptr.
        mov     rcx, r12                                ; Load thunk into rcx.
        call    rax                                     ; Call the function.

;
; Intentional follow-on to Inj90 to exit the function; rax will be returned back
; to the caller.
;

Inj90:

;
; Begin epilogue.  Restore r12 and rbp from home parameter space and return
; rsp to its original value.
;

        mov     r12, Home.SavedR12[rbp]                 ; Restore non-vol r12.
        mov     rbp, Home.SavedRbp[rbp]                 ; Restore non-vol rbp.
        add     rsp, 20h                                ; Restore home space.

        ret

        NESTED_END InjectionThunk4, _TEXT$00

; vim:set tw=80 ts=8 sw=4 sts=4 et syntax=masm fo=croql com=:;                 :

end
