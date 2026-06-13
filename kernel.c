/*
 * kernel.c — debuxOS core
 *
 * Runs entirely in 32-bit protected mode.  No stdlib, no OS calls — every byte
 * of I/O goes straight to hardware ports or the VGA framebuffer at 0xB8000.
 *
 * Layout of this file:
 *   1. VGA text-mode helpers (colour constants, putchar, string writer)
 *   2. Box-drawing GUI renderer (top bar + bordered window)
 *   3. Minimal VFS  (static array of "files" returned by 'ls')
 *   4. Mock network stack  (fake ping with simulated latency strings)
 *   5. Keyboard driver  (PS/2 port polling, US scancode → ASCII)
 *   6. CLI command parser
 *   7. kernel_main entry point
 */

/* ── 1. VGA TEXT-MODE HELPERS ─────────────────────────────────────────────── */

/* The VGA text framebuffer starts here in the identity-mapped low 4 GB. */
#define VGA_BASE   ((volatile unsigned short *)0xB8000)
#define VGA_COLS   80
#define VGA_ROWS   25

/* Attribute byte = (background << 4) | foreground.  We use IBM CGA colours. */
#define COL_WHITE_ON_BLUE   0x1F   /* top bar   */
#define COL_CYAN_ON_BLACK   0x0B   /* borders   */
#define COL_WHITE_ON_BLACK  0x0F   /* body text */
#define COL_YELLOW_ON_BLACK 0x0E   /* prompt    */
#define COL_GREEN_ON_BLACK  0x0A   /* OK output */
#define COL_RED_ON_BLACK    0x0C   /* errors    */

/* Cursor position tracked in software (we leave the hardware cursor alone). */
static int cur_row = 0;
static int cur_col = 0;

/* Write a single character + attribute directly into the VGA buffer. */
static void vga_putc_at(int row, int col, char c, unsigned char attr)
{
    /* Each cell is 16 bits: high byte = attribute, low byte = ASCII. */
    VGA_BASE[row * VGA_COLS + col] = (unsigned short)((attr << 8) | (unsigned char)c);
}

/* Clear the whole screen with spaces in a given attribute. */
static void vga_clear(unsigned char attr)
{
    for (int r = 0; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vga_putc_at(r, c, ' ', attr);
}

/* Emit one character at the software cursor, advancing it appropriately. */
static void kputc(char c, unsigned char attr)
{
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        return;
    }
    if (cur_col >= VGA_COLS) { cur_col = 0; cur_row++; }
    if (cur_row >= VGA_ROWS)   cur_row = VGA_ROWS - 1; /* clamp — no scroll */
    vga_putc_at(cur_row, cur_col++, c, attr);
}

/* Write a NUL-terminated string at the cursor. */
static void kputs(const char *s, unsigned char attr)
{
    while (*s) kputc(*s++, attr);
}

/* Minimal integer-to-decimal string (no libc sprintf available). */
static void kput_uint(unsigned int n, unsigned char attr)
{
    if (n == 0) { kputc('0', attr); return; }
    char buf[12];
    int  i = 0;
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i--) kputc(buf[i], attr);   /* digits are reversed in buf */
}

/* Write a string centred within 'width' columns at an absolute position. */
static void vga_puts_centered(int row, int width, const char *s, unsigned char attr)
{
    /* Measure string length (no strlen). */
    int len = 0;
    const char *p = s;
    while (*p++) len++;
    int col = (width - len) / 2;
    if (col < 0) col = 0;
    for (const char *q = s; *q; q++)
        vga_putc_at(row, col++, *q, attr);
}

/* Fill a horizontal span with a single repeated character. */
static void vga_hline(int row, int col_start, int col_end, char c, unsigned char attr)
{
    for (int c2 = col_start; c2 <= col_end; c2++)
        vga_putc_at(row, c2, c, attr);
}


/* ── 2. BOX-DRAWING GUI RENDERER ──────────────────────────────────────────── */

/*
 * We use the CP437 box-drawing characters that VGA text mode exposes natively.
 * These are single-box glyphs from the upper half of the 8-bit IBM charset:
 *
 *   0xDA ┌   0xBF ┐   0xC0 └   0xD9 ┘
 *   0xC4 ─   0xB3 │   0xC3 ├   0xB4 ┤
 *   0xD0 ╘   0xCA ╧   0xD2 ╓   etc.
 *
 * A cast through (char) lets us embed them as literals in C source.
 */
#define BOX_TL (char)0xDA   /* ┌ */
#define BOX_TR (char)0xBF   /* ┐ */
#define BOX_BL (char)0xC0   /* └ */
#define BOX_BR (char)0xD9   /* ┘ */
#define BOX_H  (char)0xC4   /* ─ */
#define BOX_V  (char)0xB3   /* │ */

/* Draw a single-line box; row/col are the top-left corner, inclusive. */
static void draw_box(int row, int col, int rows, int cols, unsigned char attr)
{
    int r_end = row + rows - 1;
    int c_end = col + cols - 1;

    /* Corners */
    vga_putc_at(row,   col,   BOX_TL, attr);
    vga_putc_at(row,   c_end, BOX_TR, attr);
    vga_putc_at(r_end, col,   BOX_BL, attr);
    vga_putc_at(r_end, c_end, BOX_BR, attr);

    /* Horizontal edges */
    for (int c2 = col + 1; c2 < c_end; c2++) {
        vga_putc_at(row,   c2, BOX_H, attr);
        vga_putc_at(r_end, c2, BOX_H, attr);
    }

    /* Vertical edges */
    for (int r2 = row + 1; r2 < r_end; r2++) {
        vga_putc_at(r2, col,   BOX_V, attr);
        vga_putc_at(r2, c_end, BOX_V, attr);
    }
}

/*
 * Render the initial GUI:
 *
 *   Row 0    : top bar (full-width, blue+white)
 *   Row 1-4  : status/info panel (bordered box)
 *   Row 5-24 : CLI window (bordered box, cursor inside)
 */
#define CLI_BOX_ROW   5
#define CLI_BOX_COL   0
#define CLI_BOX_ROWS  19   /* rows 5-23 */
#define CLI_BOX_COLS  80

/* First row the CLI can actually print text into. */
#define CLI_INNER_ROW  (CLI_BOX_ROW + 1)
#define CLI_INNER_COL  (CLI_BOX_COL + 1)

static void gui_init(void)
{
    vga_clear(COL_WHITE_ON_BLACK);

    /* ── Top bar (row 0) ── */
    vga_hline(0, 0, VGA_COLS - 1, ' ', COL_WHITE_ON_BLUE);
    vga_puts_centered(0, VGA_COLS, "debuxOS v0.1  |  32-bit Protected Mode  |  VGA 80x25",
                      COL_WHITE_ON_BLUE);

    /* ── Info panel (rows 1-4) ── */
    draw_box(1, 0, 4, VGA_COLS, COL_CYAN_ON_BLACK);
    /* Static status lines inside the box */
    const char *info[] = {
        " CPU : x86 32-bit  |  RAM : 32 MB (multiboot)  |  VFS : RAM-based",
        " NET : mock stack  |  KB  : PS/2 polling        |  TTY : VGA 0xB8000",
    };
    for (int i = 0; i < 2; i++) {
        int r = 2 + i;
        const char *s = info[i];
        int col = 1;
        while (*s) vga_putc_at(r, col++, *s++, COL_WHITE_ON_BLACK);
    }

    /* ── CLI window (rows 5-23) ── */
    draw_box(CLI_BOX_ROW, CLI_BOX_COL, CLI_BOX_ROWS, CLI_BOX_COLS, COL_CYAN_ON_BLACK);

    /* Title inside top border of CLI box */
    const char *title = " Terminal ";
    int tc = (VGA_COLS - 10) / 2;
    for (const char *s = title; *s; s++)
        vga_putc_at(CLI_BOX_ROW, tc++, *s, COL_CYAN_ON_BLACK);

    /* Park the software cursor on the first usable inner row. */
    cur_row = CLI_INNER_ROW;
    cur_col = CLI_INNER_COL;
}


/* ── 3. MINIMAL VFS ────────────────────────────────────────────────────────── */

/* Every "file" in our RAM-based VFS is just a name + type tag + size. */
typedef struct {
    const char *name;
    const char *type;
    unsigned int size_bytes;
} vfs_entry_t;

/* The static directory listing returned by 'ls'. */
static const vfs_entry_t vfs_root[] = {
    { "kernel.bin",  "ELF32",  49152  },
    { "boot.asm",    "source",  1024  },
    { "README.md",   "text",     512  },
    { "net.cfg",     "config",   128  },
    { "motd.txt",    "text",      64  },
};
#define VFS_ENTRY_COUNT  (sizeof(vfs_root) / sizeof(vfs_root[0]))


/* ── 4. MOCK NETWORK STACK ─────────────────────────────────────────────────── */

/* Simulate a ping response without any real hardware.
 * We "busy-wait" by spinning a counter to give the illusion of RTT. */
static volatile unsigned int rtt_counter = 0;   /* touched to prevent optimisation */

static void mock_ping(const char *host)
{
    kputs("PING ", COL_GREEN_ON_BLACK);
    kputs(host, COL_GREEN_ON_BLACK);
    kputs(" (10.0.0.1): 56 bytes of data\n", COL_GREEN_ON_BLACK);

    /* Three simulated echo replies with fake RTT values (ms). */
    unsigned int rtts[3] = { 14, 9, 11 };
    for (int i = 0; i < 3; i++) {
        /* Busy-wait ~50 M iterations to give a visible pause. */
        for (unsigned int j = 0; j < 5000000; j++) rtt_counter++;

        kputs("64 bytes from 10.0.0.1: icmp_seq=", COL_GREEN_ON_BLACK);
        kput_uint(i + 1, COL_GREEN_ON_BLACK);
        kputs(" ttl=64 time=", COL_GREEN_ON_BLACK);
        kput_uint(rtts[i], COL_GREEN_ON_BLACK);
        kputs(" ms\n", COL_GREEN_ON_BLACK);
    }
    kputs("--- ", COL_WHITE_ON_BLACK);
    kputs(host, COL_WHITE_ON_BLACK);
    kputs(" ping stats: 3 sent, 3 received, 0% loss\n", COL_WHITE_ON_BLACK);
}


/* ── 5. PS/2 KEYBOARD DRIVER ──────────────────────────────────────────────── */

/* PS/2 controller I/O ports. */
#define KB_DATA_PORT    0x60   /* read: scancode, write: command */
#define KB_STATUS_PORT  0x64   /* bit 0 = output buffer full (data ready) */

/* Inline port I/O helpers (no <sys/io.h> in freestanding mode). */
static inline unsigned char inb(unsigned short port)
{
    unsigned char val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

/*
 * Minimal US QWERTY scancode-set-1 → ASCII table.
 * Index = make-code (key-press scancode).  0x00 = "no printable mapping".
 * Only the printable range we actually need is populated.
 */
static const char kb_scancode_ascii[58] = {
/*00*/  0,    0,   '1', '2', '3', '4', '5', '6',
/*08*/ '7',  '8', '9', '0', '-', '=',  8,   '\t',  /* 8=backspace */
/*10*/ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
/*18*/ 'o',  'p', '[', ']', '\n', 0,  'a', 's',
/*20*/ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
/*28*/ '\'', '`',  0,  '\\','z', 'x', 'c', 'v',
/*30*/ 'b',  'n', 'm', ',', '.', '/',  0,   '*',
/*38*/  0,   ' '                                    /* 0x39 = space */
};

/* Block until one key-press scancode is available, return its ASCII value.
 * Break-codes (key-release, bit 7 set) are discarded. */
static char kb_getchar(void)
{
    while (1) {
        /* Wait until the keyboard controller has data. */
        while (!(inb(KB_STATUS_PORT) & 0x01))
            ;                                /* spin */

        unsigned char sc = inb(KB_DATA_PORT);

        /* Ignore break codes (key-release events have bit 7 set). */
        if (sc & 0x80) continue;

        /* Map to ASCII; skip unmapped scancodes. */
        if (sc < sizeof(kb_scancode_ascii)) {
            char c = kb_scancode_ascii[sc];
            if (c) return c;
        }
    }
}


/* ── 6. CLI COMMAND PARSER ─────────────────────────────────────────────────── */

#define CMD_BUF_LEN 64   /* max command line length */

/* Naive NUL-terminated string compare (no libc). */
static int kstrcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Naive strncmp for prefix matching. */
static int kstrncmp(const char *a, const char *b, int n)
{
    while (n-- && *b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return 0;
}

static void cli_newline(void)
{
    cur_col = CLI_INNER_COL;
    cur_row++;
    /* Clamp cursor inside the CLI box (no scrolling implemented). */
    if (cur_row >= CLI_BOX_ROW + CLI_BOX_ROWS - 1)
        cur_row = CLI_BOX_ROW + CLI_BOX_ROWS - 2;
}

static void cli_prompt(void)
{
    cli_newline();
    kputs("debux> ", COL_YELLOW_ON_BLACK);
}

static void cmd_help(void)
{
    cli_newline();
    kputs("Available commands:", COL_WHITE_ON_BLACK); cli_newline();
    kputs("  help  — show this message",              COL_WHITE_ON_BLACK); cli_newline();
    kputs("  ls    — list the root VFS directory",    COL_WHITE_ON_BLACK); cli_newline();
    kputs("  ping  — simulate an ICMP echo request",  COL_WHITE_ON_BLACK);
}

static void cmd_ls(void)
{
    cli_newline();
    kputs("NAME          TYPE    SIZE", COL_CYAN_ON_BLACK); cli_newline();
    kputs("──────────────────────────", COL_CYAN_ON_BLACK);
    for (unsigned int i = 0; i < VFS_ENTRY_COUNT; i++) {
        cli_newline();
        /* Name, padded to 14 chars */
        const char *n = vfs_root[i].name;
        int nc = 0;
        while (*n) { kputc(*n++, COL_WHITE_ON_BLACK); nc++; }
        while (nc++ < 14) kputc(' ', COL_WHITE_ON_BLACK);
        /* Type, padded to 8 chars */
        const char *t = vfs_root[i].type;
        int tc = 0;
        while (*t) { kputc(*t++, COL_WHITE_ON_BLACK); tc++; }
        while (tc++ < 8) kputc(' ', COL_WHITE_ON_BLACK);
        /* Size in bytes */
        kput_uint(vfs_root[i].size_bytes, COL_WHITE_ON_BLACK);
        kputs(" B", COL_WHITE_ON_BLACK);
    }
}

static void cmd_ping(void)
{
    cli_newline();
    mock_ping("debux.local");
}

static void cmd_unknown(const char *cmd)
{
    cli_newline();
    kputs("Unknown command: ", COL_RED_ON_BLACK);
    kputs(cmd, COL_RED_ON_BLACK);
    kputs("  (type 'help')", COL_RED_ON_BLACK);
}

/* Read one line from the keyboard, echo it, then dispatch. */
static void cli_readline_and_exec(void)
{
    char buf[CMD_BUF_LEN];
    int  idx = 0;

    while (1) {
        char c = kb_getchar();

        if (c == '\n') {                     /* Enter → execute */
            buf[idx] = '\0';
            break;
        } else if (c == 8) {                 /* Backspace */
            if (idx > 0) {
                idx--;
                /* Erase the character visually. */
                cur_col--;
                vga_putc_at(cur_row, cur_col, ' ', COL_WHITE_ON_BLACK);
            }
        } else if (idx < CMD_BUF_LEN - 1) { /* Normal printable character */
            buf[idx++] = c;
            kputc(c, COL_WHITE_ON_BLACK);
        }
    }

    /* Dispatch on trimmed buffer. */
    if (idx == 0) return;   /* blank line — just re-prompt */

    if (kstrcmp(buf, "help") == 0)
        cmd_help();
    else if (kstrcmp(buf, "ls") == 0)
        cmd_ls();
    else if (kstrncmp(buf, "ping", 4) == 0)
        cmd_ping();
    else
        cmd_unknown(buf);
}


/* ── 7. KERNEL ENTRY POINT ─────────────────────────────────────────────────── */

void kernel_main(void)
{
    gui_init();

    /* Welcome banner inside the CLI box. */
    kputs("Welcome to debuxOS — type 'help' to begin.", COL_GREEN_ON_BLACK);

    /* Event loop: prompt → read → execute, forever. */
    while (1) {
        cli_prompt();
        cli_readline_and_exec();
    }
}
