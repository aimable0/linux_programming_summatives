; ============================================================================
; temperature_processor.asm
; x86-64 Linux Assembly — NASM syntax
;
; Purpose:
;   Opens temperature_data.txt, reads it into memory, traverses it line
;   by line, and reports the total number of lines (including empty ones)
;   and the number of non-empty (valid) temperature readings.
;
; Build:
;   nasm -f elf64 temperature_processor.asm -o temperature_processor.o
;   ld   temperature_processor.o -o temperature_processor
;
; Run:
;   ./temperature_processor
;
; Expected output format:
;   Total readings: X
;   Valid readings: Y
; ============================================================================

default rel             ; all memory references are RIP-relative (position-independent)

; ── Syscall numbers (Linux x86-64 ABI) ──────────────────────────────────────
SYS_READ  equ 0
SYS_WRITE equ 1
SYS_OPEN  equ 2
SYS_CLOSE equ 3
SYS_EXIT  equ 60

; ── File flags ───────────────────────────────────────────────────────────────
O_RDONLY  equ 0

; ── File descriptor constants ────────────────────────────────────────────────
STDOUT    equ 1
STDERR    equ 2

; ── Buffer size: 64 KB — sufficient for typical temperature log files ────────
BUF_SIZE  equ 65536

; ── ASCII constants ──────────────────────────────────────────────────────────
LF        equ 10        ; \n — Unix line ending
CR        equ 13        ; \r — part of Windows CRLF line ending

; ============================================================================
; .data — initialised read-only strings
; ============================================================================
section .data

    ; Path to the input file (null-terminated for sys_open)
    filename        db 'temperature_data.txt', 0

    ; Output label strings (NOT null-terminated; lengths computed with equ)
    lbl_total       db 'Total readings: '
    lbl_total_len   equ $ - lbl_total

    lbl_valid       db 'Valid readings: '
    lbl_valid_len   equ $ - lbl_valid

    ; Single newline byte used after each printed number
    char_newline    db LF

    ; Error message written to stderr when the file cannot be opened
    err_msg         db 'Error: could not open temperature_data.txt', LF
    err_msg_len     equ $ - err_msg

; ============================================================================
; .bss — uninitialised / zero-filled data
; ============================================================================
section .bss

    file_fd     resq 1              ; 8-byte slot to store the open file descriptor
    read_buf    resb BUF_SIZE       ; 64 KB buffer for the entire file contents
    bytes_read  resq 1              ; actual number of bytes returned by sys_read
    num_buf     resb 21             ; scratch buffer for integer → ASCII conversion
                                    ; 20 digits max for uint64 + 1 safety byte

; ============================================================================
; .text — executable code
; ============================================================================
section .text
    global _start

; ────────────────────────────────────────────────────────────────────────────
; _start — program entry point
; ────────────────────────────────────────────────────────────────────────────
_start:

    ; ════════════════════════════════════════════════════════════════════
    ; STEP 1 — OPEN THE FILE
    ;
    ; sys_open(pathname, flags, mode)
    ;   rax = SYS_OPEN (2)
    ;   rdi = pointer to null-terminated filename string
    ;   rsi = O_RDONLY (0)  — open for reading only
    ;   rdx = 0             — mode is ignored when not creating a file
    ;
    ; Return value (rax):
    ;   >= 0  success: rax is the file descriptor
    ;   <  0  failure: rax is a negated errno value
    ; ════════════════════════════════════════════════════════════════════
    mov  rax, SYS_OPEN
    lea  rdi, [filename]    ; point rdi at the filename string
    xor  rsi, rsi           ; O_RDONLY = 0
    xor  rdx, rdx           ; mode = 0 (unused)
    syscall

    ; Check for error: the sign flag (SF) is set when rax < 0
    test rax, rax
    js   .error_open        ; negative → file could not be opened

    mov  [file_fd], rax     ; save the valid file descriptor for later use

    ; ════════════════════════════════════════════════════════════════════
    ; STEP 2 — READ THE ENTIRE FILE INTO MEMORY
    ;
    ; sys_read(fd, buf, count)
    ;   rax = SYS_READ (0)
    ;   rdi = file descriptor
    ;   rsi = pointer to destination buffer
    ;   rdx = maximum number of bytes to read (BUF_SIZE = 64 KB)
    ;
    ; Return value (rax):
    ;   > 0   bytes actually read (may be less than rdx for small files)
    ;   = 0   end of file (file was empty)
    ;   < 0   read error
    ;
    ; NOTE: A single sys_read call is used here.  For files larger than
    ; 64 KB a read-loop would be required; for typical temperature logs
    ; this is more than sufficient.
    ; ════════════════════════════════════════════════════════════════════
    mov  rax, SYS_READ
    mov  rdi, [file_fd]     ; file descriptor saved above
    lea  rsi, [read_buf]    ; destination buffer in .bss
    mov  rdx, BUF_SIZE      ; read up to 64 KB
    syscall

    mov  [bytes_read], rax  ; save the number of bytes actually read

    ; ════════════════════════════════════════════════════════════════════
    ; STEP 3 — CLOSE THE FILE
    ;
    ; sys_close(fd) — we have everything we need in read_buf, so
    ; close the file descriptor immediately to free the kernel resource.
    ; ════════════════════════════════════════════════════════════════════
    mov  rax, SYS_CLOSE
    mov  rdi, [file_fd]
    syscall
    ; (return value of close is not checked — non-critical on read-only fd)

    ; ════════════════════════════════════════════════════════════════════
    ; STEP 4 — TRAVERSE THE BUFFER AND COUNT LINES
    ;
    ; Register allocation for the counting loop:
    ;
    ;   rsi  — pointer to the current byte being examined
    ;   rcx  — bytes remaining in the buffer (counts down to 0)
    ;   r8   — total_lines  : incremented for every line ending found,
    ;                          and once more if the file ends without a
    ;                          trailing newline
    ;   r9   — valid_lines  : incremented only when a line ending is
    ;                          reached AND line_chars > 0
    ;   r10  — line_chars   : counts non-newline bytes on the current
    ;                          line; reset to 0 after each line ending
    ;
    ; Line ending rules (handles both Unix and Windows files):
    ;
    ;   LF  only (\n)    → one line ending
    ;   CR  only (\r)    → one line ending (old Mac style)
    ;   CRLF (\r\n)      → one line ending; the \n is consumed (skipped)
    ;                       so it is NOT double-counted
    ;
    ; A line is considered "valid" (non-empty) if line_chars > 0 when
    ; the line ending is detected.  The CR and LF bytes themselves do
    ; NOT contribute to line_chars.
    ; ════════════════════════════════════════════════════════════════════
    lea  rsi, [read_buf]        ; rsi → first byte of file data
    mov  rcx, [bytes_read]      ; rcx = number of bytes to process
    xor  r8,  r8                ; total_lines  = 0
    xor  r9,  r9                ; valid_lines  = 0
    xor  r10, r10               ; line_chars   = 0

    ; If the file was empty (bytes_read == 0), skip the loop entirely
    test rcx, rcx
    jz   .output

; ── Main byte-by-byte scan loop ──────────────────────────────────────────────
.scan_loop:
    ; Loop termination: exit when all bytes have been consumed
    test  rcx, rcx
    jz    .eof_check            ; no more bytes → check for trailing line

    movzx eax, byte [rsi]       ; load current byte (zero-extend to avoid
    inc   rsi                   ;   stale high bits); advance pointer
    dec   rcx                   ; one fewer byte remaining

    ; ── CRLF / CR detection ──────────────────────────────────────────────────
    ; Check for Carriage Return (\r, 0x0D) first, because CRLF must be
    ; detected as a unit: if we see \r followed by \n we must consume
    ; BOTH bytes so the \n is not processed as a second line ending.
    cmp   al, CR
    jne   .check_lf             ; not CR → check for bare LF

    ; We have a CR.  Peek at the very next byte (if one exists).
    test  rcx, rcx
    jz    .newline_found        ; CR at end-of-file → treat as one line ending

    cmp   byte [rsi], LF        ; is the next byte a LF?
    jne   .newline_found        ; bare CR (no following LF) → line ending

    ; CRLF pair: consume (skip over) the LF so it is not seen again.
    inc   rsi                   ; skip the LF byte
    dec   rcx
    jmp   .newline_found        ; treat the CR+LF pair as a single line ending

    ; ── LF-only detection ────────────────────────────────────────────────────
.check_lf:
    cmp   al, LF
    je    .newline_found        ; bare LF → line ending

    ; ── Regular (non-newline) character ──────────────────────────────────────
    ; Any byte that is not CR or LF is content — increment the line
    ; character counter so we can determine whether this line is empty.
    inc   r10                   ; line_chars++
    jmp   .scan_loop

    ; ── A complete line ending was detected ──────────────────────────────────
.newline_found:
    inc   r8                    ; total_lines++ (every line ending, incl. empty)

    ; Check whether the line contained any non-newline characters
    test  r10, r10
    jz    .reset_line           ; line_chars == 0 → empty line, skip valid count

    inc   r9                    ; valid_lines++ (at least one content byte)

.reset_line:
    xor   r10, r10              ; reset line_chars for the next line
    jmp   .scan_loop

    ; ── End-of-file check ────────────────────────────────────────────────────
    ; If the file does not end with a newline character, the last line
    ; will never trigger .newline_found.  We detect this by checking
    ; whether line_chars > 0 after the loop exits.
    ;
    ; Example:  "23.5\n18.1"   ← the "18.1" part never gets a newline
    ;           r10 == 4 here, so we manually count it as one more line.
.eof_check:
    test  r10, r10              ; any buffered chars since the last newline?
    jz    .output               ; no → file ended exactly on a newline

    inc   r8                    ; count the unterminated trailing line
    inc   r9                    ; it contains content, so it is valid

    ; ════════════════════════════════════════════════════════════════════
    ; STEP 5 — DISPLAY RESULTS
    ;
    ; Format:
    ;   Total readings: <r8>\n
    ;   Valid readings: <r9>\n
    ;
    ; itoa_and_print (defined below) converts a 64-bit unsigned integer
    ; in rdi to decimal ASCII and writes it directly to stdout.
    ; ════════════════════════════════════════════════════════════════════
.output:

    ; ── "Total readings: " label ─────────────────────────────────────────────
    mov  rax, SYS_WRITE
    mov  rdi, STDOUT
    lea  rsi, [lbl_total]
    mov  rdx, lbl_total_len
    syscall

    mov  rdi, r8                ; pass total_lines as the integer to print
    call itoa_and_print

    mov  rax, SYS_WRITE         ; newline after the number
    mov  rdi, STDOUT
    lea  rsi, [char_newline]
    mov  rdx, 1
    syscall

    ; ── "Valid readings: " label ─────────────────────────────────────────────
    mov  rax, SYS_WRITE
    mov  rdi, STDOUT
    lea  rsi, [lbl_valid]
    mov  rdx, lbl_valid_len
    syscall

    mov  rdi, r9                ; pass valid_lines as the integer to print
    call itoa_and_print

    mov  rax, SYS_WRITE         ; newline after the number
    mov  rdi, STDOUT
    lea  rsi, [char_newline]
    mov  rdx, 1
    syscall

    ; ── Clean exit ───────────────────────────────────────────────────────────
    mov  rax, SYS_EXIT
    xor  rdi, rdi               ; exit code 0 = success
    syscall

    ; ════════════════════════════════════════════════════════════════════
    ; ERROR PATH — file could not be opened
    ;
    ; Writes the error message to stderr (fd 2) and exits with code 1
    ; so the calling shell can detect failure via $?.
    ; ════════════════════════════════════════════════════════════════════
.error_open:
    mov  rax, SYS_WRITE
    mov  rdi, STDERR
    lea  rsi, [err_msg]
    mov  rdx, err_msg_len
    syscall

    mov  rax, SYS_EXIT
    mov  rdi, 1                 ; exit code 1 = failure
    syscall


; ============================================================================
; itoa_and_print
; ────────────────────────────────────────────────────────────────────────────
; Converts an unsigned 64-bit integer to a decimal ASCII string and
; writes it to stdout (fd 1) using a single sys_write call.
;
; Input:
;   rdi — the unsigned 64-bit integer value to print
;
; Output:
;   The decimal digits are written directly to stdout.
;   Nothing is returned in a register.
;
; Scratch registers (modified; caller is responsible for preserving
; anything it needs across the call):
;   rax, rcx, rdx, rsi, rdi, r11
;
; Algorithm:
;   Build the decimal representation BACKWARDS into num_buf, starting
;   from the rightmost position, by repeatedly dividing by 10 and
;   storing remainders as ASCII digits.  Then use sys_write on the
;   substring [r11 .. num_buf+20).
;
;   num_buf layout (21 bytes):
;       [0]...[19] — digit characters, filled right-to-left
;       (the "start" pointer r11 moves left with each digit)
; ============================================================================
itoa_and_print:

    lea  r11, [num_buf]
    add  r11, 20                ; r11 → one byte past the rightmost digit slot
                                ; (we build the string backwards from here)

    mov  rax, rdi               ; rax = value to convert
    mov  rcx, 10                ; divisor constant

    ; Special case: value is zero
    test rax, rax
    jnz  .extract_digits

    dec  r11                    ; move pointer one byte to the left
    mov  byte [r11], '0'        ; store the single '0' digit
    jmp  .write_number

    ; ── Digit extraction loop ────────────────────────────────────────────────
    ; Each iteration:
    ;   xdx:rax ÷ rcx  →  rax = quotient,  rdx = remainder (0–9)
    ; The remainder is the LEAST-significant digit, so we store it first
    ; (right side of the number) and move the pointer left each time.
.extract_digits:
    test rax, rax               ; have we extracted all digits?
    jz   .write_number

    xor  rdx, rdx               ; zero rdx before dividing (dividend is rdx:rax)
    div  rcx                    ; rax = rax / 10,  rdx = rax % 10
    add  dl, '0'                ; convert remainder to ASCII digit character
    dec  r11                    ; advance pointer leftward
    mov  [r11], dl              ; store the digit
    jmp  .extract_digits

    ; ── Write the completed decimal string ───────────────────────────────────
    ; r11  = pointer to the first (most-significant) digit
    ; length = (num_buf + 20) - r11
.write_number:
    lea  rdx, [num_buf]
    add  rdx, 20                ; rdx = one-past-end address
    sub  rdx, r11               ; rdx = number of digit bytes to write

    mov  rsi, r11               ; rsi = start of digit string
    mov  rax, SYS_WRITE
    mov  rdi, STDOUT
    syscall

    ret
