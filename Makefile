# ─────────────────────────────────────────────────────────────────────────────
# Makefile — debuxOS build system
#
# Toolchain requirements (must be on PATH):
#   nasm          — Netwide Assembler  (any modern version)
#   i686-elf-gcc  — freestanding GCC cross-compiler for 32-bit x86
#   i686-elf-ld   — matching cross-linker
#   qemu-system-i386  (optional, for the 'run' target)
#
# Typical cross-compiler build:
#   https://wiki.osdev.org/GCC_Cross-Compiler
# Or install via package manager on many distros:
#   sudo apt install gcc-i686-linux-gnu binutils-i686-linux-gnu   (Debian/Ubuntu)
# ─────────────────────────────────────────────────────────────────────────────

# ── Tool names (override on the command line if your prefix differs) ──────────
AS      := nasm
CC      := i686-elf-gcc
LD      := i686-elf-ld
QEMU    := qemu-system-i386

# ── Output binary ─────────────────────────────────────────────────────────────
TARGET  := debuxos.bin

# ── Source files ──────────────────────────────────────────────────────────────
ASM_SRC := boot.asm
C_SRC   := kernel.c

# ── Object files (placed alongside sources for simplicity) ───────────────────
ASM_OBJ := boot.o
C_OBJ   := kernel.o

# ── Assembler flags ───────────────────────────────────────────────────────────
# -f elf32   : output 32-bit ELF object (required by the LD cross-linker)
ASFLAGS := -f elf32

# ── Compiler flags ────────────────────────────────────────────────────────────
# -m32              : generate 32-bit code
# -ffreestanding    : no host OS headers or startup files
# -O2               : optimise; removes dead stores our volatile counters protect
# -Wall -Wextra     : catch obvious mistakes early
# -nostdlib         : never link against host libc
# -std=gnu11        : C11 + GNU extensions (for inline asm)
# -fno-stack-protector : no __stack_chk_fail reference; we have no libc to link
CFLAGS  := -m32 -ffreestanding -O2 -Wall -Wextra \
           -nostdlib -std=gnu11 -fno-stack-protector

# ── Linker flags ──────────────────────────────────────────────────────────────
# -T linker.ld       : use our custom linker script
# -nostdlib          : don't pull in crt*.o or libc
# --oformat binary   : raw flat binary, not ELF (GRUB parses the Multiboot header)
LDFLAGS := -T linker.ld -nostdlib --oformat binary

# ── QEMU flags ────────────────────────────────────────────────────────────────
# -kernel            : load a Multiboot-compliant binary directly (no ISO needed)
# -m 32M             : give the guest 32 MB of RAM
# -nographic         : redirect VGA to the terminal (remove if you want a window)
QFLAGS  := -kernel $(TARGET) -m 32M

# ─────────────────────────────────────────────────────────────────────────────
# Targets
# ─────────────────────────────────────────────────────────────────────────────

.PHONY: all clean run

## Default: build the kernel binary
all: $(TARGET)

## Link object files into the final raw binary
$(TARGET): $(ASM_OBJ) $(C_OBJ)
	$(LD) $(LDFLAGS) -o $@ $^
	@echo ">>> Built: $@  ($$(wc -c < $@) bytes)"

## Assemble boot.asm → boot.o
$(ASM_OBJ): $(ASM_SRC)
	$(AS) $(ASFLAGS) -o $@ $<

## Compile kernel.c → kernel.o
$(C_OBJ): $(C_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

## Run the kernel under QEMU (boots in ~1 second, no ISO required)
run: $(TARGET)
	$(QEMU) $(QFLAGS)

## Remove build artefacts
clean:
	rm -f $(ASM_OBJ) $(C_OBJ) $(TARGET)
	@echo ">>> Clean."
