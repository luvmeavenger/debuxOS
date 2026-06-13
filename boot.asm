; ─────────────────────────────────────────────────────────────────────────────
; boot.asm  —  debuxOS stage-0 bootstrap
; GRUB/Multiboot expects a magic header in the first 8 KB of the binary.
; We park it in its own section so the linker script can pin it at 0x100000.
; ─────────────────────────────────────────────────────────────────────────────

bits 32                          ; we enter in 32-bit protected mode (GRUB did
                                 ; the real-mode → PM switch for us)

; ── Multiboot 1 constants ────────────────────────────────────────────────────
MAGIC     equ 0x1BADB002         ; bootloader scans for this dword
FLAGS     equ 0x00000003         ; bit 0 = align modules on 4 KB pages,
                                 ; bit 1 = pass memory map to kernel
CHECKSUM  equ -(MAGIC + FLAGS)   ; magic + flags + checksum must sum to 0

; ── Multiboot header block (must appear within first 8 KB) ───────────────────
section .multiboot               ; own section → linker places it first
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

; ── BSS: our 16 KB kernel stack (zero-initialised by the loader) ──────────────
section .bss
align 16
stack_bottom:
    resb 16384                   ; 16 KB is more than enough for our toy kernel
stack_top:                       ; grows downward, so top address = initial ESP

; ── .text: the CPU lands here after GRUB hands off control ───────────────────
section .text
global _start                    ; linker entry symbol
_start:
    ; Point the stack pointer at the top of our reserved BSS region.
    ; No EFLAGS / segment fiddling needed — GRUB already put us in a sane
    ; 32-bit flat-memory environment.
    mov  esp, stack_top

    ; Call the C kernel — this should never return, but we halt just in case.
    extern kernel_main
    call kernel_main

.hang:
    cli                          ; disable interrupts
    hlt                          ; halt the CPU
    jmp .hang                    ; if an NMI wakes us, loop back
