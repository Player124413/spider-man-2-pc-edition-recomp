// Харнесс дифф-тестов: загружает рекомпилированные модули и по командам из stdin
// исполняет гостевые функции. Протокол — tests/difftest/difftest.py.
#include <dlfcn.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/loader.h"
#include "runtime/rt.h"

#include "difftest_modules.h"

static sigjmp_buf g_jmp;
static u32 g_trap_addr;
static char g_trap_msg[256];

static int g_verbose;

static void on_trap(CPU *c, u32 addr, const char *what)
{
    (void)c;
    if (g_verbose)
        rt_dump_trace(24);
    g_trap_addr = addr;
    snprintf(g_trap_msg, sizeof g_trap_msg, "%s", what);
    siglongjmp(g_jmp, 1);
}

static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    g_trap_addr = (u32)((u8 *)si->si_addr - g_mem);
    if (g_verbose) {
        rt_dump_trace(24);
#if defined(__APPLE__) && defined(__aarch64__)
        ucontext_t *u = uc;
        void *pc = (void *)u->uc_mcontext->__ss.__pc;
        Dl_info di;
        if (dladdr(pc, &di))
            fprintf(stderr, "  host pc=%p %s+0x%lx (image base %p)\n", pc, di.dli_sname ? di.dli_sname : "?",
                    (unsigned long)((char *)pc - (char *)di.dli_saddr), di.dli_fbase);
#endif
    }
    snprintf(g_trap_msg, sizeof g_trap_msg, "host fault");
    siglongjmp(g_jmp, 2);
}

// Сторожевой таймер: рекомпилированная функция, зациклившаяся на случайных данных, не вешает прогон.
static void on_alarm(int sig)
{
    (void)sig;
    g_trap_addr = 0;
    snprintf(g_trap_msg, sizeof g_trap_msg, "timeout");
    siglongjmp(g_jmp, 4);
}

static void watchdog(int seconds)
{
    struct itimerval t = {{0, 0}, {seconds, 0}};
    setitimer(ITIMER_REAL, &t, NULL);
}

static u32 rd_u32(void);

// rdtsc: та же последовательность, что у unicorn в difftest.py (TSC0, шаг TSC_STEP, сброс перед тестом)
#define TSC0 0x0000000100000000ull
#define TSC_STEP 0x1000ull
static u64 g_tsc;
static u64 det_rdtsc(void) { return g_tsc += TSC_STEP; }

// Команда P: оставить доступными только перечисленные области окна гостя (как отображено в unicorn),
// чтобы бегущая запись на случайных данных падала здесь так же, как там, а не затирала образы.
static void protect_regions(void)
{
    u32 n = rd_u32();
    size_t pg = (size_t)getpagesize();
    mprotect(g_mem + 0x10000, RT_WINDOW - 0x10000, PROT_NONE);
    for (u32 i = 0; i < n; i++) {
        u32 a = rd_u32(), size = rd_u32();
        size_t lo = a & ~(pg - 1), hi = ((size_t)a + size + pg - 1) & ~(pg - 1);
        mprotect(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE);
    }
    // затем — страницы только для чтения (.text/.rdata образов, как в Windows). Округление внутрь:
    // страница хоста (16 КБ на arm64 macOS) крупнее гостевой, а .data может начинаться на границе 4 КБ
    n = rd_u32();
    for (u32 i = 0; i < n; i++) {
        u32 a = rd_u32(), size = rd_u32();
        size_t lo = ((size_t)a + pg - 1) & ~(pg - 1), hi = ((size_t)a + size) & ~(pg - 1);
        if (hi > lo)
            mprotect(g_mem + lo, hi - lo, PROT_READ);
    }
}

static void rd_exact(void *p, size_t n)
{
    if (fread(p, 1, n, stdin) != n)
        exit(0);
}

static u32 rd_u32(void)
{
    u32 v;
    rd_exact(&v, 4);
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game/System>\n", argv[0]);
        return 2;
    }
    if (rt_mem_init() != 0) {
        fprintf(stderr, "mem init failed\n");
        return 1;
    }
    for (int i = 0; difftest_modules[i]; i++)
        if (rt_load_module(difftest_modules[i], argv[1]) != 0)
            return 1;
    rt_trap_hook = on_trap;
    rt_rdtsc_hook = det_rdtsc;
    g_verbose = getenv("DIFFTEST_VERBOSE") != NULL;
    if (getenv("RT_BLOCKLOG")) {
        extern int rt_trace_blocks;
        rt_trace_blocks = 1;
    }
    if (getenv("DIFFTEST_WATCH")) {
        extern u32 rt_trace_watch;
        rt_trace_watch = (u32)strtoul(getenv("DIFFTEST_WATCH"), NULL, 16);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    signal(SIGALRM, on_alarm);
    // Подтверждение готовности
    u32 ok = 0x4B4F4B4Fu;
    fwrite(&ok, 4, 1, stdout);
    fflush(stdout);
    for (;;) {
        int cmd = fgetc(stdin);
        if (cmd == EOF || cmd == 'Q')
            return 0;
        if (cmd == 'W') {
            u32 a = rd_u32(), n = rd_u32();
            rd_exact(g_mem + a, n);
        } else if (cmd == 'R') {
            u32 a = rd_u32(), n = rd_u32();
            fwrite(g_mem + a, 1, n, stdout);
            fflush(stdout);
        } else if (cmd == 'P') {
            protect_regions();
        } else if (cmd == 'X') {
            CPU cpu;
            memset(&cpu, 0, sizeof cpu);
            u32 regs[8];
            rd_exact(regs, sizeof regs);
            memcpy(&cpu.eax, regs, sizeof regs);
            cpu.fpucw = rd_u32();
            cpu.fs_base = rd_u32();
            u32 fn_addr = rd_u32();
            u32 status = 0;
            g_tsc = TSC0;
            g_trap_addr = 0;
            g_trap_msg[0] = 0;
            rt_eh_reset();  // кадры EH прерванного прошлого теста
            int j = sigsetjmp(g_jmp, 1);
            if (j == 0) {
                guest_fn fn = rt_lookup(fn_addr);
                if (!fn) {
                    status = 3;
                } else {
                    watchdog(2);
                    fn(&cpu);
                }
            } else {
                status = (u32)j;
            }
            watchdog(0);
            memcpy(regs, &cpu.eax, sizeof regs);
            fwrite(&status, 4, 1, stdout);
            fwrite(regs, 4, 8, stdout);
            fwrite(&cpu.st_ret, 8, 1, stdout);
            fwrite(&g_trap_addr, 4, 1, stdout);
            u32 len = (u32)strlen(g_trap_msg);
            fwrite(&len, 4, 1, stdout);
            fwrite(g_trap_msg, 1, len, stdout);
            fflush(stdout);
        }
    }
}
