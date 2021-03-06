        title "IsPrefixOfStringInTable_x64"
        option nokeyword:<Length>
;++
;
; Copyright (c) Trent Nelson, 2018.
;
; Module Name:
;
;   PrefixSearchStringTable_x64_*.asm.
;
; Abstract:
;
;   These modules implement the IsPrefixOfStringInTable routine.
;
;   N.B. Keep this header identical between files to declutter diff output.
;
;--

include StringTable.inc

;++
;
; STRING_TABLE_INDEX
; IsPrefixOfStringInTable_x64_*(
;     _In_ PSTRING_TABLE StringTable,
;     _In_ PSTRING String,
;     _Out_opt_ PSTRING_MATCH Match
;     )
;
; Routine Description:
;
;   Searches a string table to see if any strings "prefix match" the given
;   search string.  That is, whether any string in the table "starts with
;   or is equal to" the search string.
;
;   This routine is identical to version 4, but has the initial negative match
;   instructions re-ordered and tweaked in order to reduce the block throughput
;   reported by IACA (from 3.74 to 3.48).
;
;   N.B. Although this does result in a measurable speedup, the clarity suffers
;        somewhat due to the fact that instructions that were previously paired
;        together are now spread out (e.g. moving the string buffer address into
;        rax and then loading that into xmm0 three instructions later).
;
; Arguments:
;
;   StringTable - Supplies a pointer to a STRING_TABLE struct.
;
;   String - Supplies a pointer to a STRING struct that contains the string to
;       search for.
;
;   Match - Optionally supplies a pointer to a variable that contains the
;       address of a STRING_MATCH structure.  This will be populated with
;       additional details about the match if a non-NULL pointer is supplied.
;
; Return Value:
;
;   Index of the prefix match if one was found, NO_MATCH_FOUND if not.
;
;--

        LEAF_ENTRY IsPrefixOfStringInTable_x64_5, _TEXT$00

;
; Load the address of the string buffer into rax.
;

        ;IACA_VC_START

        mov     rax, String.Buffer[rdx]         ; Load buffer addr.

;
; Broadcast the byte-sized string length into xmm4.
;

        vpbroadcastb xmm4, byte ptr String.Length[rdx]  ; Broadcast length.

;
; Load the lengths of each string table slot into xmm3.
;

        vmovdqa xmm3, xmmword ptr StringTable.Lengths[rcx]  ; Load lengths.

;
; Load the search string buffer into xmm0.
;

        vmovdqu xmm0, xmmword ptr [rax]         ; Load search buffer.

;
; Compare the search string's length, which we've broadcasted to all 8-byte
; elements of the xmm4 register, to the lengths of the slots in the string
; table, to find those that are greater in length.
;

        vpcmpgtb    xmm1, xmm3, xmm4            ; Identify long slots.

;
; Shuffle the buffer in xmm0 according to the unique indexes, and store the
; result into xmm5.
;

        vpshufb     xmm5, xmm0, StringTable.UniqueIndex[rcx] ; Rearrange string.

;
; Compare the search string's unique character array (xmm5) against the string
; table's unique chars (xmm2), saving the result back into xmm5.
;

        vpcmpeqb    xmm5, xmm5, StringTable.UniqueChars[rcx] ; Compare to uniq.

;
; Intersect-and-test the unique character match xmm mask register (xmm5) with
; the length match mask xmm register (xmm1).  This affects flags, allowing us
; to do a fast-path exit for the no-match case (where CY = 1 after xmm1 has
; been inverted).
;

        vptest      xmm1, xmm5                  ; Check for no match.
        jnc         short Pfx10                 ; There was a match.

;
; No match, set rax to -1 and return.
;

        xor         eax, eax                    ; Clear rax.
        not         al                          ; al = -1
        ret                                     ; Return.

        ;IACA_VC_END

;
; (There was at least one match, continue with processing.)
;

;
; Calculate the "search length" for the incoming search string, which is
; equivalent of 'min(String->Length, 16)'.  (The search string's length
; currently lives in xmm4, albeit as a byte-value broadcasted across the
; entire register, so extract that first.)
;
; Once the search length is calculated, deposit it back at the second byte
; location of xmm4.
;
;   r10 and xmm4[15:8] - Search length (min(String->Length, 16))
;
;   r11 - String length (String->Length)
;

Pfx10:  vpextrb     r11, xmm4, 0                ; Load length.
        mov         rax, 16                     ; Load 16 into rax.
        mov         r10, r11                    ; Copy into r10.
        cmp         r10w, ax                    ; Compare against 16.
        cmova       r10w, ax                    ; Use 16 if length is greater.
        vpinsrb     xmm4, xmm4, r10d, 1         ; Save back to xmm4b[1].

;
; Home our parameter registers into xmm registers instead of their stack-backed
; location, to avoid memory writes.
;

        vpxor       xmm2, xmm2, xmm2            ; Clear xmm2.
        vpinsrq     xmm2, xmm2, rcx, 0          ; Save rcx into xmm2q[0].
        vpinsrq     xmm2, xmm2, rdx, 1          ; Save rdx into xmm2q[1].

;
; Intersect xmm5 and xmm1 (as we did earlier with the 'vptest xmm1, xmm5'),
; yielding a mask identifying indices we need to perform subsequent matches
; upon.  Convert this into a bitmap and save in xmm2d[2].
;

        vpandn      xmm5, xmm1, xmm5            ; Intersect unique + lengths.
        vpmovmskb   edx, xmm5                   ; Generate a bitmap from mask.

;
; We're finished with xmm5; repurpose it in the same vein as xmm2 above.
;

        vpxor       xmm5, xmm5, xmm5            ; Clear xmm5.
        vpinsrq     xmm5, xmm5, r8, 0           ; Save r8 into xmm5q[0].

;
; Summary of xmm register stashing for the rest of the routine:
;
; xmm2:
;        0:63   (vpinsrq 0)     rcx (1st function parameter, StringTable)
;       64:127  (vpinsrq 1)     rdx (2nd function paramter, String)
;
; xmm4:
;       0:7     (vpinsrb 0)     length of search string
;       8:15    (vpinsrb 1)     min(String->Length, 16)
;      16:23    (vpinsrb 2)     loop counter (when doing long string compares)
;      24:31    (vpinsrb 3)     shift count
;
; xmm5:
;       0:63    (vpinsrq 0)     r8 (3rd function parameter, StringMatch)
;      64:95    (vpinsrd 2)     bitmap of slots to compare
;      96:127   (vpinsrd 3)     index of slot currently being processed
;

;
; Initialize rcx as our counter register by doing a popcnt against the bitmap
; we just generated in edx, and clear our shift count register (r9).
;

        popcnt      ecx, edx                    ; Count bits in bitmap.
        xor         r9, r9                      ; Clear r9.

        align 16

;
; Top of the main comparison loop.  The bitmap will be present in rdx.  Count
; trailing zeros of the bitmap, and then add in the shift count, producing an
; index (rax) we can use to load the corresponding slot.
;
; Register usage at top of loop:
;
;   rax - Index.
;
;   rcx - Loop counter.
;
;   rdx - Bitmap initially, then slot length.
;
;   r9 - Shift count.
;
;   r10 - Search length.
;
;   r11 - String length.
;

Pfx20:  tzcnt       r8d, edx                    ; Count trailing zeros.
        mov         eax, r8d                    ; Copy tzcnt to rax,
        add         rax, r9                     ; Add shift to create index.
        inc         r8                          ; tzcnt + 1
        shrx        rdx, rdx, r8                ; Reposition bitmap.
        vpinsrd     xmm5, xmm5, edx, 2          ; Store bitmap, free up rdx.
        xor         edx, edx                    ; Clear edx.
        mov         r9, rax                     ; Copy index back to shift.
        inc         r9                          ; Shift = Index + 1
        vpinsrd     xmm5, xmm5, eax, 3          ; Store the raw index xmm5d[3].

;
; "Scale" the index (such that we can use it in a subsequent vmovdqa) by
; shifting left by 4 (i.e. multiply by '(sizeof STRING_SLOT)', which is 16).
;
; Then, load the string table slot at this index into xmm1, then shift rax back.
;

        shl         eax, 4
        vpextrq     r8, xmm2, 0
        vmovdqa     xmm1, xmmword ptr [rax + StringTable.Slots[r8]]
        shr         eax, 4

;
; The search string's first 16 characters are already in xmm0.  Compare this
; against the slot that has just been loaded into xmm1, storing the result back
; into xmm1.
;

        vpcmpeqb    xmm1, xmm1, xmm0            ; Compare search string to slot.

;
; Convert the XMM mask into a 32-bit representation, then zero high bits after
; our "search length", which allows us to ignore the results of the comparison
; above for bytes that were after the search string's length, if applicable.
; Then, count the number of bits remaining, which tells us how many characters
; we matched.
;

        vpmovmskb   r8d, xmm1                   ; Convert into mask.
        bzhi        r8d, r8d, r10d              ; Zero high bits.
        popcnt      r8d, r8d                    ; Count bits.

;
; Load the slot length into rdx.  As xmm3 already has all the slot lengths in
; it, we can load rax (the current index) into xmm1 and use it to extract the
; slot length via shuffle.  (The length will be in the lowest byte of xmm1
; after the shuffle, which we can then vpextrb.)
;

        movd        xmm1, rax                   ; Load index into xmm1.
        vpshufb     xmm1, xmm3, xmm1            ; Shuffle lengths.
        vpextrb     rdx, xmm1, 0                ; Extract target length to rdx.

;
; If 16 characters matched, and the search string's length is longer than 16,
; we're going to need to do a comparison of the remaining strings.
;

        cmp         r8w, 16                     ; Compare chars matched to 16.
        je          short @F                    ; 16 chars matched.
        jmp         Pfx30                       ; Less than 16 matched.

;
; All 16 characters matched.  If the slot length is greater than 16, we need
; to do an inline memory comparison of the remaining bytes.  If it's 16 exactly,
; then great, that's a slot match, we're done.
;

@@:     cmp         dl, 16                      ; Compare length to 16.
        ja          Pfx50                       ; Length is > 16.
        je          short Pfx35                 ; Lengths match!
                                                ; Length <= 16, fall through...

;
; Less than or equal to 16 characters were matched.  Compare this against the
; length of the slot; if equal, this is a match, if not, no match, continue.
;

Pfx30:  cmp         r8b, dl                     ; Compare against slot length.
        jne         @F                          ; No match found.
        jmp         short Pfx35                 ; Match found!

;
; No match against this slot, decrement counter and either continue the loop
; or terminate the search and return no match.
;

@@:     vpextrd     edx, xmm5, 2                ; Restore rdx bitmap.
        dec         cx                          ; Decrement counter.
        jnz         Pfx20                       ; cx != 0, continue.

        xor         eax, eax                    ; Clear rax.
        not         al                          ; al = -1
        ret                                     ; Return.

;
; Pfx35 and Pfx40 are the jump targets for when the prefix match succeeds.  The
; former is used when we need to copy the number of characters matched from r8
; back to rax.  The latter jump target doesn't require this.
;

Pfx35:  mov         rax, r8                     ; Copy numbers of chars matched.

;
; Load the match parameter back into r8 and test to see if it's not-NULL, in
; which case we need to fill out a STRING_MATCH structure for the match.
;

Pfx40:  vpextrq     r8, xmm5, 0                 ; Extract StringMatch.
        test        r8, r8                      ; Is NULL?
        jnz         short @F                    ; Not zero, need to fill out.

;
; StringMatch is NULL, we're done. Extract index of match back into rax and ret.
;

        vpextrd     eax, xmm5, 3                ; Extract raw index for match.
        ret                                     ; StringMatch == NULL, finish.

;
; StringMatch is not NULL.  Fill out characters matched (currently rax), then
; reload the index from xmm5 into rax and save.
;

@@:     mov         byte ptr StringMatch.NumberOfMatchedCharacters[r8], al
        vpextrd     eax, xmm5, 3                ; Extract raw index for match.
        mov         byte ptr StringMatch.Index[r8], al

;
; Final step, loading the address of the string in the string array.  This
; involves going through the StringTable, so we need to load that parameter
; back into rcx, then resolving the string array address via pStringArray,
; then the relevant STRING offset within the StringArray.Strings structure.
;

        vpextrq     rcx, xmm2, 0            ; Extract StringTable into rcx.
        mov         rcx, StringTable.pStringArray[rcx] ; Load string array.

        shl         eax, 4                  ; Scale the index; sizeof STRING=16.
        lea         rdx, [rax + StringArray.Strings[rcx]] ; Resolve address.
        mov         qword ptr StringMatch.String[r8], rdx ; Save STRING ptr.
        shr         eax, 4                  ; Revert the scaling.

        ret

;
; 16 characters matched and the length of the underlying slot is greater than
; 16, so we need to do a little memory comparison to determine if the search
; string is a prefix match.
;
; The slot length is stored in rax at this point, and the search string's
; length is stored in r11.  We know that the search string's length will
; always be longer than or equal to the slot length at this point, so, we
; can subtract 16 (currently stored in r10) from rax, and use the resulting
; value as a loop counter, comparing the search string with the underlying
; string slot byte-by-byte to determine if there's a match.
;

Pfx50:  sub         rdx, r10                ; Subtract 16 from search length.

;
; Free up some registers by stashing their values into various xmm offsets.
;

        vpinsrb     xmm4, xmm4, ecx, 2      ; Free up rcx register.
        mov         rcx, rdx                ; Free up rdx, rcx is now counter.

;
; Load the search string buffer and advance it 16 bytes.
;

        vpextrq     r11, xmm2, 1            ; Extract String into r11.
        mov         r11, String.Buffer[r11] ; Load buffer address.
        add         r11, r10                ; Advance buffer 16 bytes.

;
; Loading the slot is more involved as we have to go to the string table, then
; the pStringArray pointer, then the relevant STRING offset within the string
; array (which requires re-loading the index from xmm5d[3]), then the string
; buffer from that structure.
;

        vpextrq     r8, xmm2, 0             ; Extract StringTable into r8.
        mov         r8, StringTable.pStringArray[r8] ; Load string array.

        shl         eax, 4                  ; Scale the index; sizeof STRING=16.

        lea         r8, [rax + StringArray.Strings[r8]] ; Resolve address.
        mov         r8, String.Buffer[r8]   ; Load string table buffer address.
        add         r8, r10                 ; Advance buffer 16 bytes.

        xor         eax, eax                ; Clear eax.

;
; We've got both buffer addresses + 16 bytes loaded in r11 and r8 respectively.
; Do a byte-by-byte comparison.
;

        align 16
@@:     mov         dl, byte ptr [rax + r11]    ; Load byte from search string.
        cmp         dl, byte ptr [rax + r8]     ; Compare against target.
        jne         short Pfx60                 ; If not equal, jump.

;
; The two bytes were equal, update rax, decrement rcx and potentially continue
; the loop.
;

        inc         ax                          ; Increment index.
        loopnz      @B                          ; Decrement cx and loop back.

;
; All bytes matched!  Add 16 (still in r10) back to rax such that it captures
; how many characters we matched, and then jump to Pfx40 for finalization.
;

        add         rax, r10
        jmp         Pfx40

;
; Byte comparisons were not equal.  Restore the rcx loop counter and decrement
; it.  If it's zero, we have no more strings to compare, so we can do a quick
; exit.  If there are still comparisons to be made, restore the other registers
; we trampled then jump back to the start of the loop Pfx20.
;

Pfx60:  vpextrb     rcx, xmm4, 2                ; Restore rcx counter.
        dec         cx                          ; Decrement counter.
        jnz         short @F                    ; Jump forward if not zero.

;
; No more comparisons remaining, return.
;

        xor         eax, eax                    ; Clear rax.
        not         al                          ; al = -1
        ret                                     ; Return.

;
; More comparisons remain; restore the registers we clobbered and continue loop.
;

@@:     vpextrb     r10, xmm4, 1                ; Restore r10.
        vpextrb     r11, xmm4, 0                ; Restore r11.
        vpextrd     edx, xmm5, 2                ; Restore rdx bitmap.
        jmp         Pfx20                       ; Continue comparisons.

        ;IACA_VC_END

        LEAF_END   IsPrefixOfStringInTable_x64_5, _TEXT$00

; vim:set tw=80 ts=8 sw=4 sts=4 et syntax=masm fo=croql comments=\:;           :

end
