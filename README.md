# debuxOS

Minimal x86 Multiboot kernel — VGA GUI, PS/2 CLI, RAM VFS, mock network.

## Build

- [ ] Install tools: `nasm`, `i686-elf-gcc`, `i686-elf-ld`, `qemu-system-i386`
- [ ] `make` — assembles + compiles + links → `debuxos.bin`
- [ ] `make run` — boots the binary in QEMU; type `help`, `ls`, or `ping`
