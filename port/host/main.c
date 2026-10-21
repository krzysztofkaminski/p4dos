/*
 * Linux host port of the emulator core: an interactive mode on any ANSI terminal, and a
 * scripted headless mode (deterministic virtual time) used by the test suite, which can
 * also save the picture exactly as the Guition panel path renders it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "host.h"
#include "hostdisk.h"
#include "pc.h"
#include "term.h"
#include "video.h"

static FILE *log_file;
static int headless;
static int quit_requested;
static struct termios saved_tio;
static int tio_saved;

/* --- HAL -------------------------------------------------------------------- */

uint64_t hal_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void hal_idle(uint32_t ns)
{
    if (host_sdl_active()) {
        struct timespec ts = {0, ns > 2000000 ? 2000000 : (long)ns};
        nanosleep(&ts, 0);
        return;
    }
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    struct timespec ts = {0, (long)ns};
    ppoll(&p, 1, &ts, 0);
}

void hal_log(const char *fmt, ...)
{
    va_list ap;
    FILE *f = log_file ? log_file : (headless ? stderr : 0);
    if (!f) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

void hal_rtc_get(hal_rtc_t *t)
{
    time_t now = headless ? (time_t)(1791547200 + pc_now() / 1000000000ull) : time(0);
    struct tm tm;
    if (headless) {
        gmtime_r(&now, &tm);
    } else {
        localtime_r(&now, &tm);
    }
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day = tm.tm_mday;
    t->hour = tm.tm_hour;
    t->min = tm.tm_min;
    t->sec = tm.tm_sec;
}

void hal_rtc_set(const hal_rtc_t *t)
{
    hal_log("RTC set request ignored: %04d-%02d-%02d %02d:%02d:%02d\n", t->year, t->month,
            t->day, t->hour, t->min, t->sec);
}

void hal_lpt_putc(uint8_t c)
{
    static FILE *f;
    if (!f) {
        f = fopen("lpt1.out", "ab");
    }
    if (f) {
        fputc(c, f);
        fflush(f);
    }
}

void hal_disks_refresh(void) {}
void hal_input_lock(void) {}
void hal_input_unlock(void) {}

/* --- image files ------------------------------------------------------------ */

static int file_read(blkdev_t *d, uint32_t lba, uint32_t n, uint8_t *buf)
{
    int fd = (int)(intptr_t)d->ctx;
    return pread(fd, buf, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)n * 512 ? 0 : -1;
}

static int file_write(blkdev_t *d, uint32_t lba, uint32_t n, const uint8_t *buf)
{
    int fd = (int)(intptr_t)d->ctx;
    return pwrite(fd, buf, (size_t)n * 512, (off_t)lba * 512) == (ssize_t)n * 512 ? 0 : -1;
}

static blkdev_t *open_image(const char *path, int readonly)
{
    struct stat st;
    int fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fd < 0 && !readonly) {
        fd = open(path, O_RDONLY);
        readonly = 1;
    }
    if (fd < 0 || fstat(fd, &st)) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        exit(2);
    }
    blkdev_t *d = calloc(1, sizeof(*d));
    d->read = file_read;
    d->write = file_write;
    d->sectors = (uint32_t)(st.st_size / 512);
    d->readonly = readonly;
    d->ctx = (void *)(intptr_t)fd;
    return d;
}

/* --- screenshots ------------------------------------------------------------ */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) {
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            }
            crc_table[i] = v;
        }
    }
    while (n--) {
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    }
    return c;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc32_update(0xFFFFFFFFu, hdr + 4, 4);
    crc = ~crc32_update(crc, data, len);
    uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    fwrite(tail, 1, 4, f);
}

/* Minimal PNG writer (stored deflate blocks) for RGB565 pixels. */
static int write_png(const char *path, const uint16_t *px, int w, int h, int stride, int step)
{
    int ow = w / step, oh = h / step;
    size_t rowlen = (size_t)ow * 3 + 1, rawlen = rowlen * (size_t)oh;
    uint8_t *raw = malloc(rawlen), *z = malloc(rawlen + rawlen / 65535 * 5 + 16);
    FILE *f = fopen(path, "wb");
    if (!f || !raw || !z) {
        return -1;
    }
    for (int y = 0; y < oh; y++) {
        uint8_t *r = raw + (size_t)y * rowlen;
        *r++ = 0;
        for (int x = 0; x < ow; x++) {
            uint16_t c = px[(size_t)(y * step) * (size_t)stride + (size_t)(x * step)];
            *r++ = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            *r++ = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            *r++ = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    size_t zl = 0;
    uint32_t a = 1, b = 0;
    z[zl++] = 0x78;
    z[zl++] = 0x01;
    for (size_t off = 0; off < rawlen;) {
        size_t n = rawlen - off > 65535 ? 65535 : rawlen - off;
        z[zl++] = off + n == rawlen;
        z[zl++] = (uint8_t)n;
        z[zl++] = (uint8_t)(n >> 8);
        z[zl++] = (uint8_t)~n;
        z[zl++] = (uint8_t)(~n >> 8);
        memcpy(z + zl, raw + off, n);
        zl += n;
        off += n;
    }
    for (size_t i = 0; i < rawlen; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    z[zl++] = (uint8_t)(b >> 8);
    z[zl++] = (uint8_t)b;
    z[zl++] = (uint8_t)(a >> 8);
    z[zl++] = (uint8_t)a;

    uint8_t ihdr[13] = {(uint8_t)(ow >> 24), (uint8_t)(ow >> 16), (uint8_t)(ow >> 8), (uint8_t)ow,
                        (uint8_t)(oh >> 24), (uint8_t)(oh >> 16), (uint8_t)(oh >> 8), (uint8_t)oh,
                        8, 2, 0, 0, 0};
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zl);
    png_chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

/* Render through the panel path (portrait frame buffer, rotated) and save either that
 * raw frame buffer or the upright picture recovered from it. */
static int screenshot(const char *path, int raw, int rotate, int step)
{
    static uint16_t fb[800 * 1280], up[SCREEN_W * SCREEN_H];
    render_target_t t = {.fb = fb, .stride = 800, .rotate = rotate};
    int r0, r1;
    render_update(&t, pc_now(), 1, &r0, &r1);
    if (raw) {
        return write_png(path, fb, 800, 1280, 800, step);
    }
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            up[y * SCREEN_W + x] = rotate == ROT_CW ? fb[x * 800 + (799 - y)]
                                                    : fb[(1279 - x) * 800 + y];
        }
    }
    return write_png(path, up, SCREEN_W, SCREEN_H, SCREEN_W, step);
}

/* --- text screen helpers ---------------------------------------------------- */

static void screen_text(char *out, int trim)
{
    const int cols = video.kind == VK_TEXT40 ? 40 : 80;
    const uint32_t start = ((uint32_t)(video.crtc[0x0C] & 0x3F) << 8) | video.crtc[0x0D];
    char *p = out;
    if (video.kind > VK_TEXT80) {
        strcpy(out, "[graphics mode]\n");
        return;
    }
    for (int row = 0; row < 25; row++) {
        char *line = p;
        for (int col = 0; col < cols; col++) {
            uint8_t c = cpu_mem[0xB8000 + (((start + (uint32_t)(row * cols + col)) * 2) & 0x7FFE)];
            *p++ = (c < 32 || c > 126) ? (c == 0 ? ' ' : '?') : (char)c;
        }
        if (trim) {
            while (p > line && p[-1] == ' ') {
                p--;
            }
        }
        *p++ = '\n';
    }
    *p = 0;
}

/* --- scripted (headless) mode ----------------------------------------------- */

/* Debugging aid for the `trace` script command: log software interrupts and port I/O. */
static FILE *trace_file;

static void trace_exc(int vector, uint32_t code)
{
    static unsigned n;
    if (trace_file && (++n <= 200 || (n & (n - 1)) == 0)) {
        const uint8_t *p = cpu_lin_ptr(cpu.sg[SEG_CS].base + cpu.insn_ip, 8, 0);
        fprintf(trace_file, "EXC %02X code %X at %04X:%08X cr0=%08X cpl=%d ss:esp=%04X:%08X  ", vector,
                (unsigned)code, cpu.seg[SEG_CS], (unsigned)cpu.insn_ip, (unsigned)cpu.cr0, cpu.cpl,
                cpu.seg[SEG_SS], (unsigned)cpu.r.d[REG_SP]);
        for (int i = 0; p && i < 8; i++) {
            fprintf(trace_file, "%02X ", p[i]);
        }
        fprintf(trace_file, "(#%u)\n", n);
    }
}

static uint32_t trace_io_count[0x10000];

static void trace_io(int is_write, uint16_t port, uint8_t v)
{
    /* Keep the log readable: the first accesses to each port, then a sample. */
    uint32_t n = ++trace_io_count[port];
    if (n <= 40 || (n & (n - 1)) == 0) {
        fprintf(trace_file, "%s %03X %02X   (%04X:%04X #%u)\n", is_write ? "out" : "in ", port, v,
                cpu.seg[SEG_CS], cpu.ip, (unsigned)n);
    }
}

static void trace_run(unsigned secs, const char *path)
{
    uint64_t end = pc_now() + (uint64_t)secs * 1000000000ull;
    trace_file = fopen(path, "w");
    if (!trace_file) {
        return;
    }
    pc_io_trace = trace_io;
    cpu_exc_trace = trace_exc;
    pc_batch = 1;
    uint32_t ret_at = 0xFFFFFFFF; /* where the DOS call being watched returns to */
    while (pc_now() < end) {
        uint32_t a = (((uint32_t)cpu.seg[SEG_CS] << 4) + cpu.ip) & CPU_MEM_MASK;
        if (a == ret_at) {
            fprintf(trace_file, "    -> AX=%04X BX=%04X %s\n", cpu.r.w[REG_AX * 2], cpu.r.w[REG_BX * 2],
                    (cpu_get_flags() & FL_CF) ? "CF" : "ok");
            ret_at = 0xFFFFFFFF;
        }
        if (!cpu.halted && cpu_mem[a] == 0xCD && cpu.seg[SEG_CS] != 0xF000) {
            uint8_t n = cpu_mem[(a + 1) & CPU_MEM_MASK];
            uint8_t ah = cpu.r.b[1];
            int quiet = (n == 0x21 && (ah == 0x02 || ah == 0x06 || ah == 0x0B || ah == 0x2C ||
                                       ah == 0x3F || ah == 0x40 || ah == 0x42)) ||
                        n == 0x16 || n == 0x28 || n == 0x2A || n == 0x1C;
            if (!quiet) {
                fprintf(trace_file, "INT %02X AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X "
                        "(%04X:%04X)\n", n, cpu.r.w[REG_AX * 2], cpu.r.w[REG_BX * 2], cpu.r.w[REG_CX * 2],
                        cpu.r.w[REG_DX * 2], cpu.seg[SEG_DS], cpu.seg[SEG_ES], cpu.seg[SEG_CS],
                        cpu.ip);
                if (n == 0x21 && (ah == 0x3D || ah == 0x4B || ah == 0x3C || ah == 0x4E)) {
                    uint32_t p = ((uint32_t)cpu.seg[SEG_DS] << 4) + cpu.r.w[REG_DX * 2];
                    fprintf(trace_file, "    \"%.64s\"\n", (const char *)cpu_mem + (p & CPU_MEM_MASK));
                }
                if (n == 0x21 && (ah == 0x48 || ah == 0x4A || ah == 0x4B || ah == 0x3D)) {
                    ret_at = (a + 2) & CPU_MEM_MASK;
                }
            }
        }
        pc_step();
    }
    pc_batch = 4096;
    pc_io_trace = 0;
    cpu_exc_trace = 0;
    fclose(trace_file);
}

/* `wav FILE` records what the audio output would play, paced by emulated time. */
static FILE *wav_file;
static uint32_t wav_frames;
static uint64_t wav_time, wav_frac;

static void wav_header(void)
{
    uint32_t bytes = wav_frames * 2, rate = AUDIO_RATE, byte_rate = AUDIO_RATE * 2, v;
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x01\0........\x02\0\x10\0data....";
    v = bytes + 36;
    memcpy(h + 4, &v, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 40, &bytes, 4);
    fseek(wav_file, 0, SEEK_SET);
    fwrite(h, 1, 44, wav_file);
}

static void wav_pump(void)
{
    int16_t buf[512];
    uint64_t now = pc_now();
    uint64_t f = (now - wav_time) * AUDIO_RATE + wav_frac;
    wav_time = now;
    wav_frac = f % 1000000000ull;
    for (uint64_t n = f / 1000000000ull; n;) {
        int k = n > 512 ? 512 : (int)n;
        audio_render(buf, k);
        fwrite(buf, 2, (size_t)k, wav_file);
        wav_frames += (uint32_t)k;
        n -= (uint64_t)k;
    }
}

static void run_ms(unsigned ms)
{
    uint64_t end = pc_now() + (uint64_t)ms * 1000000ull;
    while (pc_now() < end) {
        pc_step();
        if (wav_file) {
            wav_pump();
        }
    }
}

static void type_string(const char *s)
{
    for (; *s; s++) {
        int c = (unsigned char)*s;
        int lone_esc = 0;
        if (c == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'r': c = '\r'; break;
            case 'n': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'e': /* the Esc key, or the start of a terminal escape sequence */
                c = 0x1B;
                lone_esc = s[1] != '[' && s[1] != 'O';
                break;
            case 'a': /* Alt + the next character */
                kbd_term_char(0x1B);
                c = s[1] ? (unsigned char)*++s : 0x1B;
                break;
            default: c = (unsigned char)*s; break;
            }
        }
        kbd_term_char(c);
        if (lone_esc) {
            kbd_term_char(-1);
        }
        run_ms(40);
    }
    kbd_term_char(-1);
}

static int wait_text(const char *needle, unsigned timeout_s)
{
    static char scr[25 * 81 + 1];
    uint64_t end = pc_now() + (uint64_t)timeout_s * 1000000000ull;
    for (;;) {
        screen_text(scr, 0);
        if (strstr(scr, needle)) {
            return 0;
        }
        if (pc_now() >= end) {
            return -1;
        }
        run_ms(20);
    }
}

static int run_script(FILE *f)
{
    char line[1024], scr[25 * 81 + 1];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        char *arg;
        lineno++;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') {
            continue;
        }
        arg = strchr(line, ' ');
        if (arg) {
            *arg++ = 0;
        } else {
            arg = line + strlen(line);
        }
        if (!strncmp(line, "wait", 4)) {
            unsigned timeout = line[4] == ':' ? (unsigned)atoi(line + 5) : 60;
            if (wait_text(arg, timeout)) {
                screen_text(scr, 1);
                fprintf(stderr, "script line %d: timed out waiting for \"%s\"; screen:\n%s",
                        lineno, arg, scr);
                return 1;
            }
        } else if (!strcmp(line, "type")) {
            type_string(arg);
        } else if (!strcmp(line, "sleep")) {
            run_ms((unsigned)atoi(arg));
        } else if (!strcmp(line, "dump")) {
            screen_text(scr, 1);
            fputs(scr, stdout);
            fflush(stdout);
        } else if (!strcmp(line, "shot") || !strcmp(line, "shotraw") ||
                   !strcmp(line, "shothalf")) {
            if (screenshot(arg, line[4] == 'r', ROT_CW, line[4] == 'h' ? 2 : 1)) {
                fprintf(stderr, "script line %d: cannot write %s\n", lineno, arg);
                return 1;
            }
        } else if (!strcmp(line, "frames")) {
            /* frames MS N: N display refreshes MS apart, drawing only what changed as the
             * board does, then a check against the whole picture drawn afresh. */
            static uint16_t fb[800 * 1280], ref[800 * 1280];
            unsigned ms = 25, n = 40;
            int r0, r1;
            sscanf(arg, "%u %u", &ms, &n);
            render_target_t t = {.fb = fb, .stride = 800, .rotate = ROT_CW};
            for (unsigned i = 0; i < n; i++) {
                run_ms(ms);
                render_update(&t, pc_now(), 0, &r0, &r1);
            }
            t.fb = ref;
            render_update(&t, pc_now(), 1, &r0, &r1);
            for (unsigned i = 0; i < 800 * 1280; i++) {
                if (fb[i] != ref[i]) {
                    printf("frames: differ at frame buffer row %u, column %u\n", i / 800, i % 800);
                    break;
                }
                if (i == 800 * 1280 - 1) {
                    printf("frames: ok\n");
                }
            }
            fflush(stdout);
        } else if (!strcmp(line, "mouse")) {
            int dx = 0, dy = 0, b = 0;
            sscanf(arg, "%d %d %d", &dx, &dy, &b);
            mouse_event(dx, dy, b);
            run_ms(30);
        } else if (!strcmp(line, "state")) {
            fprintf(stderr, "CS:IP=%04X:%04X SS:SP=%04X:%04X AX=%04X BX=%04X CX=%04X DX=%04X "
                    "DS=%04X ES=%04X FL=%04X halted=%d icount=%llu\n",
                    cpu.seg[SEG_CS], cpu.ip, cpu.seg[SEG_SS], cpu.r.w[REG_SP * 2], cpu.r.w[REG_AX * 2],
                    cpu.r.w[REG_BX * 2], cpu.r.w[REG_CX * 2], cpu.r.w[REG_DX * 2], cpu.seg[SEG_DS],
                    cpu.seg[SEG_ES], cpu_get_flags(), cpu.halted,
                    (unsigned long long)cpu.icount);
        } else if (!strcmp(line, "zerotrace")) {
            /* Debug aid: single-step until the CPU runs into zero-filled memory, then
             * show how it got there. */
            static struct { uint16_t cs, ip, ax, sp; uint8_t op[4]; } hist[4096];
            unsigned h = 0, zeros = 0;
            unsigned secs = 5, watch = 0;
            sscanf(arg, "%u %x", &secs, &watch);
            uint64_t end = pc_now() + (uint64_t)secs * 1000000000ull;
            uint8_t watched = cpu_mem[watch], armed = 0;
            pc_batch = 1;
            while (pc_now() < end && zeros < 6) {
                uint32_t a = ((uint32_t)cpu.seg[SEG_CS] << 4) + cpu.ip;
                if (watch && cpu_mem[watch] != watched) {
                    fprintf(stderr, "[%05X] %02X -> %02X\n", watch, watched, cpu_mem[watch]);
                    watched = cpu_mem[watch];
                    if (armed++ >= 1) {
                        break;
                    }
                }
                if (!cpu.halted) {
                    hist[h % 4096].cs = cpu.seg[SEG_CS];
                    hist[h % 4096].ip = cpu.ip;
                    hist[h % 4096].ax = cpu.r.w[REG_AX * 2];
                    hist[h % 4096].sp = cpu.r.w[REG_SP * 2];
                    memcpy(hist[h % 4096].op, cpu_mem + (a & CPU_MEM_MASK), 4);
                    h++;
                    zeros = (cpu_mem[a & CPU_MEM_MASK] | cpu_mem[(a + 1) & CPU_MEM_MASK]) ? 0 : zeros + 1;
                }
                pc_step();
            }
            pc_batch = 4096;
            fprintf(stderr, "ES=%04X DI=%04X CX=%04X BP=%04X DS=%04X SS=%04X\n", cpu.seg[SEG_ES],
                    cpu.r.w[REG_DI * 2], cpu.r.w[REG_CX * 2], cpu.r.w[REG_BP * 2], cpu.seg[SEG_DS],
                    cpu.seg[SEG_SS]);
            for (unsigned i = h > 4000 ? h - 4000 : 0; i < h; i++) {
                fprintf(stderr, "%04X:%04X  %02X %02X %02X %02X  AX=%04X SP=%04X\n",
                        hist[i % 4096].cs, hist[i % 4096].ip, hist[i % 4096].op[0],
                        hist[i % 4096].op[1], hist[i % 4096].op[2], hist[i % 4096].op[3],
                        hist[i % 4096].ax, hist[i % 4096].sp);
            }
        } else if (!strcmp(line, "wav")) { /* wav FILE starts recording, wav alone stops */
            if (wav_file) {
                wav_header();
                fclose(wav_file);
                wav_file = 0;
            }
            if (*arg && (wav_file = fopen(arg, "wb")) != 0) {
                wav_frames = 0;
                wav_frac = 0;
                wav_time = pc_now();
                wav_header();
            }
        } else if (!strcmp(line, "trace")) { /* trace SECONDS FILE */
            unsigned secs = 1;
            char path[512] = "trace.log";
            sscanf(arg, "%u %511s", &secs, path);
            trace_run(secs, path);
        } else if (!strcmp(line, "savemem")) {
            FILE *m = fopen(arg, "wb");
            if (m) {
                fwrite(cpu_mem, 1, CPU_MEM_SIZE, m);
                fclose(m);
            }
        } else if (!strcmp(line, "scan")) { /* raw set-1 scancodes in hex, 30 ms apart */
            char *end;
            for (char *p = arg; *p; p = end) {
                unsigned long v = strtoul(p, &end, 16);
                if (end == p) {
                    break;
                }
                kbd_push((uint8_t)v);
                run_ms(30);
            }
        } else if (!strcmp(line, "hid")) { /* hid MODS [KEY...]: a USB boot keyboard report */
            uint8_t keys[6] = {0};
            unsigned mods = 0, k[6] = {0};
            int n = sscanf(arg, "%x %x %x %x %x %x %x", &mods, &k[0], &k[1], &k[2], &k[3], &k[4],
                           &k[5]);
            for (int i = 0; i + 1 < n; i++) {
                keys[i] = (uint8_t)k[i];
            }
            kbd_hid_report((uint8_t)mods, keys, 6);
            run_ms(50);
        } else if (!strcmp(line, "cad")) { /* Ctrl+Alt+Del */
            static const uint8_t seq[] = {0x1D, 0x38, 0x53, 0xD3, 0xB8, 0x9D};
            for (unsigned i = 0; i < sizeof(seq); i++) {
                kbd_push(seq[i]);
            }
            run_ms(100);
        } else if (!strcmp(line, "reset")) {
            pc_reset();
        } else if (!strcmp(line, "quit")) {
            break;
        } else {
            fprintf(stderr, "script line %d: unknown command \"%s\"\n", lineno, line);
            return 1;
        }
    }
    if (wav_file) {
        wav_header();
        fclose(wav_file);
        wav_file = 0;
    }
    return 0;
}

/* --- interactive terminal mode ---------------------------------------------- */

static int term_out(const char *buf, int len)
{
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, buf, (size_t)len);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return 1;
        }
        buf += n;
        len -= (int)n;
    }
    return 0;
}

static void tty_restore(void)
{
    if (tio_saved) {
        term_close();
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        tio_saved = 0;
    }
}

static uint64_t term_input_at;
static int term_esc_pending;

static void term_key(int ch)
{
    if (ch == 0x1D) { /* Ctrl-] */
        quit_requested = 1;
    } else if (ch == 0x0C) { /* Ctrl-L repaints the terminal */
        term_refresh(1);
        return;
    }
    kbd_term_char(ch);
}

/* Takes in what the terminal sent: more than 0 if there was something, 0 at its end. */
static ssize_t term_read(void)
{
    uint8_t buf[4096];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    for (ssize_t i = 0; i < n; i++) {
        hostdisk_input(buf[i]);
    }
    if (n > 0) {
        term_esc_pending = 1;
        term_input_at = hal_time_ns();
    } else if (n == 0) {
        quit_requested = 1; /* stdin closed */
    }
    return n;
}

static int frame_out(const void *buf, int len)
{
    return term_out(buf, len);
}

static void answer_wait(int ms)
{
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    if (quit_requested) {
        poll(0, 0, ms);
    } else if (poll(&p, 1, ms) > 0) {
        term_read();
    }
}

static void answer_wake(void)
{
}

static const hostdisk_port_t hostdisk_port = {
    .send = frame_out, .key = term_key, .wait = answer_wait, .wake = answer_wake};

static void run_terminal(void)
{
    struct termios tio;
    uint64_t last_refresh = 0;

    if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
        tio_saved = 1;
        atexit(tty_restore);
        tio = saved_tio;
        cfmakeraw(&tio);
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    term_init(term_out);
    hostdisk_init(&hostdisk_port);

    while (!quit_requested) {
        uint64_t now = hal_time_ns();
        if (term_read() < 0 && term_esc_pending && now - term_input_at > 50000000ull) {
            hostdisk_input(-1);
            term_esc_pending = 0;
        }
        for (int i = 0; i < 4; i++) {
            pc_step();
        }
        if (now - last_refresh > 33000000ull) {
            last_refresh = now;
            term_refresh(0);
        }
    }
    tty_restore();
}

/* --- main ------------------------------------------------------------------- */

static void usage(void)
{
    fputs("usage: p4dos [options]\n"
          "  -a FILE, -b FILE   floppy images (A:, B:)\n"
          "  -c FILE, -d FILE   hard disk images (C:, D:)\n"
          "  --ro               open images read-only\n"
          "  --ems KB           expanded memory for EMM.SYS (default 4096, 0 = none)\n"
          "  --xms KB           XMS memory on a 186 or 8086, by XMS.SYS (default 4096)\n"
          "  --mouse            plug a serial mouse into COM1\n"
          "  --cpu 386|186|8086 CPU model (default 386)\n"
          "  --ram MB           memory of the 80386 machine (default 16)\n"
          "  --fpu 0|1          80387 coprocessor next to the 80386 (default 1)\n"
          "  --script FILE      run a test script headless, in virtual time ('-' = stdin)\n"
          "  --realtime         with --script: run against the real clock instead\n"
          "  --sdl              open a 1280x800 window (if built with SDL2)\n"
          "  --log FILE         write emulator log messages to FILE\n"
          "  --exc-log FILE     write every CPU exception (80386) to FILE\n"
          "Interactive terminal mode: Ctrl-] quits, Ctrl-L repaints.\n",
          stderr);
}

int main(int argc, char **argv)
{
    static uint8_t vram[VGA_RAM_SIZE];
    pc_config_t cfg = {.vram = vram, .cpu_model = CPU_80386, .fpu = 1, .ram_kb = 16384,
                       .ems_kb = 4096, .xms_kb = 4096};
    const char *images[DRIVE_COUNT] = {0};
    const char *script = 0;
    int readonly = 0, sdl = 0, mouse = 0, realtime = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "-a") && v) {
            images[DRIVE_FD0] = v, i++;
        } else if (!strcmp(a, "-b") && v) {
            images[DRIVE_FD1] = v, i++;
        } else if (!strcmp(a, "-c") && v) {
            images[DRIVE_HD0] = v, i++;
        } else if (!strcmp(a, "-d") && v) {
            images[DRIVE_HD1] = v, i++;
        } else if (!strcmp(a, "--ro")) {
            readonly = 1;
        } else if (!strcmp(a, "--ems") && v) {
            cfg.ems_kb = (uint32_t)atoi(v), i++;
        } else if (!strcmp(a, "--xms") && v) {
            cfg.xms_kb = (uint32_t)atoi(v), i++;
        } else if (!strcmp(a, "--mouse")) {
            mouse = 1;
        } else if (!strcmp(a, "--cpu") && v) {
            cfg.cpu_model = !strcmp(v, "8086") ? CPU_8086 : !strcmp(v, "186") ? CPU_80186
                                                                              : CPU_80386;
            i++;
        } else if (!strcmp(a, "--fpu") && v) {
            cfg.fpu = atoi(v), i++;
        } else if (!strcmp(a, "--ram") && v) {
            cfg.ram_kb = (uint32_t)atoi(v) * 1024, i++;
        } else if (!strcmp(a, "--script") && v) {
            script = v, i++;
        } else if (!strcmp(a, "--realtime")) {
            realtime = 1;
        } else if (!strcmp(a, "--sdl")) {
            sdl = 1;
        } else if (!strcmp(a, "--exc-log") && v) {
            /* debugging aid: every exception the 80386 raises, without the slowdown and
             * the change in timing of the script's `trace` */
            trace_file = fopen(v, "w"), i++;
            cpu_exc_trace = trace_exc;
        } else if (!strcmp(a, "--log") && v) {
            log_file = fopen(v, "w"), i++;
        } else {
            usage();
            return 2;
        }
    }

    headless = script != 0;
    cfg.virtual_time = headless && !realtime;
    if (cfg.cpu_model < CPU_80386 || cfg.ram_kb < 1024) {
        cfg.ram_kb = 1024;
    } else {
        cfg.xms_kb = 0; /* extended memory is real: HIMEMX hands it out */
    }
    cfg.ram = calloc(cfg.ram_kb + 1, 1024);
    if (cfg.ems_kb) {
        cfg.ems_ram = calloc(cfg.ems_kb, 1024);
    }
    if (cfg.xms_kb) {
        cfg.xms_ram = calloc(cfg.xms_kb, 1024);
    }
    pc_init(&cfg);
    for (int d = 0; d < DRIVE_COUNT; d++) {
        if (images[d]) {
            pc_attach(d, open_image(images[d], readonly));
        }
    }
    mouse_present = mouse;
    pc_reset();

    if (script) {
        FILE *f = strcmp(script, "-") ? fopen(script, "r") : stdin;
        if (!f) {
            perror(script);
            return 2;
        }
        return run_script(f);
    }
    if (sdl) {
        return host_sdl_run();
    }
    run_terminal();
    return 0;
}
