/*
 * linker.ld — debuxOS linker script
 *
 * The x86 PC memory map reserves 0x00000000–0x000FFFFF for firmware and
 * real-mode legacy space.  Multiboot kernels are traditionally loaded at
 * the 1 MB boundary (0x00100000), which sits in free RAM on any PC with
 * more than 1 MB.
 *
 * Section layout:
 *   .multiboot  — Multiboot 1 header (must appear in the first 8 KB of the
 *                 binary so GRUB can find the magic dword).
 *   .text       — executable code
 *   .rodata     — read-only data (string literals, const tables)
 *   .data       — initialised read-write data
 *   .bss        — zero-initialised data (including our stack)
 */

/* Tell the linker where execution begins. */
ENTRY(_start)

SECTIONS {

    /* ── Load address: 1 MB physical ──────────────────────────────────── */
    . = 0x00100000;

    /* ── Multiboot header must come first ─────────────────────────────── */
    .multiboot : {
        *(.multiboot)       /* boot.asm's .multiboot section */
    }

    /* ── Executable code ──────────────────────────────────────────────── */
    .text ALIGN(4096) : {
        *(.text)            /* all .text sections from all objects */
    }

    /* ── Read-only data (string literals, const arrays) ───────────────── */
    .rodata ALIGN(4096) : {
        *(.rodata)
        *(.rodata.*)
    }

    /* ── Initialised data ─────────────────────────────────────────────── */
    .data ALIGN(4096) : {
        *(.data)
    }

    /* ── Zero-initialised data (stack lives here) ─────────────────────── */
    .bss ALIGN(4096) : {
        *(COMMON)           /* unresolved common symbols */
        *(.bss)
    }

    /* ── Discard debug and metadata sections ──────────────────────────── */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
