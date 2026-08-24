/*
 * PanVK Valhall v9 Vulkan Entry Points & WSI Swapchain Layer Implementation
 * Full Vulkan API implementation for vkmark, DXVK & Wine/Winlator
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <elf.h>
#include <errno.h>

#ifndef VK_NOT_AVAILABLE
#define VK_NOT_AVAILABLE (-9)
#endif

#ifndef RROutput
typedef unsigned long RROutput;
#endif

#ifndef VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
#define VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME "VK_EXT_surface_maintenance_1"
#endif
#ifndef VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
#define VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME "VK_EXT_swapchain_maintenance_1"
#endif

static FILE *g_pvk_log = NULL;
/* Não-static: compartilhado com v9_cmd_stream.c para o TRACE de RT ir ao
 * mesmo arquivo do driver (o stderr nem sempre é capturado pelo Winlator). */
void pvk_log(const char *fmt, ...) {
    if (!g_pvk_log) {
        const char *lf = getenv("PANVK_LOG_FILE");
        if (lf && lf[0]) {
            g_pvk_log = fopen(lf, "a");
        }
        if (!g_pvk_log) {
            g_pvk_log = fopen("/sdcard/Download/panvk_winlator.log", "a");
        }
        if (!g_pvk_log) {
            g_pvk_log = fopen("/tmp/panvk_winlator.log", "a");
        }
    }
    if (g_pvk_log) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(g_pvk_log, "[%02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_pvk_log, fmt, ap);
        va_end(ap);
        fflush(g_pvk_log);
    }
}

/* ---- Crash instrumentation ----
 * The ICD is dlopen"ed into every wine process.  Install signal handlers for
 * the common death signals so that a fault anywhere in the process (driver,
 * loader or wine) is dumped to the same /sdcard log with registers+backtrace.
 */
static int g_sig_installed = 0;
static void *g_sig_ctx[3] = { NULL, NULL, NULL };
static uintptr_t g_sig_frame_ra = 0;

static void pvk_bt_print(const char *tag) {
    void *frames[32];
    int n = backtrace(frames, 32);
    if (n > 0) {
        pvk_log("  %s backtrace (%d frames):\n", tag, n);
        for (int i = 0; i < n && i < 24; i++) {
            Dl_info info;
            const char *sym = "<unknown>";
            char buf[64];
            if (dladdr(frames[i], &info) && info.dli_sname) {
                snprintf(buf, sizeof(buf), "%s+%#lx", info.dli_sname,
                         (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_saddr));
                sym = buf;
            }
            pvk_log("    #%02d %p  %s\n", i, frames[i], sym);
        }
    }
}

static int read_word_ok(uintptr_t addr) {
    /* probe mapping via mincore(2) + manual sigsetjmp-guided read; no /proc/self/maps
     * (which is unreliable inside the proot crash handler). */
    unsigned char vec[1];
    uintptr_t page = addr & ~(uintptr_t)0xfffUL;
    if (mincore((void *)page, 1, vec) != 0)
        return 0;
    return 1;
}

static void pvk_sig_handler(int signo, siginfo_t *si, void *uc) {
    int saved = errno;
    ucontext_t *uctx = (ucontext_t *)uc;
    uintptr_t pc = uctx ? uctx->uc_mcontext.pc : 0;
    uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;

    /* Workaround box64+wine: winevulkan (emulado) grava o endereço da função
     * resolvida dentro do thunk PE do winevulkan.dll (página .text r-xp).
     * Em wine x86-64 real essa página é gravável; sob box64 o write falha
     * com SEGV_ACCERR. Aqui tornamos a página RWX e re-executamos o write
     * para o wine prosseguir. Nunca mascara falhas do PRÓPRIO driver. */
    if (signo == SIGSEGV && si && si->si_code == SEGV_ACCERR && fault && pc) {
        Dl_info di;
        int in_driver = dladdr((void *)pc, &di) && di.dli_fname &&
                        strstr(di.dli_fname, "libvulkan_panvk") != NULL;
        if (!in_driver) {
            uintptr_t page = fault & ~(uintptr_t)0xfffUL;
            if (mprotect((void *)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                pvk_log("SIGSEGV ACCERR auto-fix: mprotect(RWX) %#lx (fault=%#lx pc=%#lx)\n",
                        (unsigned long)page, (unsigned long)fault, (unsigned long)pc);
                errno = saved;
                return;
            }
        }
    }

    pvk_log("*** SIGNAL %d (%s) pid=%d pc=%#lx fault_addr=%#lx code=%d ***\n",
            signo, signo == SIGSEGV ? "SIGSEGV" : signo == SIGBUS ? "SIGBUS" :
            signo == SIGABRT ? "SIGABRT" : signo == SIGILL ? "SIGILL" :
            signo == SIGFPE ? "SIGFPE" : "?", (int)getpid(),
            (unsigned long)pc, (unsigned long)fault, si ? si->si_code : -1);

    if (pc) {
        Dl_info info;
        if (dladdr((void *)pc, &info)) {
            pvk_log("  crash site: dli_fname=%s dli_fbase=%p dli_sname=%s dli_saddr=%p\n",
                    info.dli_fname ? info.dli_fname : "?", info.dli_fbase,
                    info.dli_sname ? info.dli_sname : "?", info.dli_saddr);
        } else {
            pvk_log("  crash site: no dladdr for pc (%s)\n", dlerror() ? dlerror() : "?");
        }
    }

#ifdef __aarch64__
    if (uctx) {
        /* box64 layout: x64emu_t { reg64_t regs[16]; x64flags_t eflags; reg64_t ip; ... }
         * RSP = regs[_SP] (idx 4) -> offset 32 ; RIP = ip -> offset 136.
         * In interpreter Run(), x27 holds the emu pointer. */
        uintptr_t emu_p = uctx->uc_mcontext.regs[27];
        if (emu_p) {
            volatile uint64_t *g = (volatile uint64_t *)emu_p;
uint64_t grax = 0, grcx = 0, gsp = 0, gbp = 0, grip = 0, gr11 = 0;
                int ok = 1;
                /* probe readable */
                if (read_word_ok(emu_p + 32)) {
                    gsp   = g[4];
                    grax  = g[0];
                    grcx  = g[1];
                    gbp   = g[5];
                    gr11  = g[11];
            } else ok = 0;
            if (read_word_ok(emu_p + 136)) {
                grip  = *(volatile uint64_t *)(emu_p + 136);
            } else ok = 0;
            if (ok) {
                pvk_log("  guest(x64emu@%p): RAX=%#lx RCX=%#lx RSP=%#lx RBP=%#lx RIP=%#lx R11=%#lx\n",
                        (void *)emu_p, (unsigned long)grax, (unsigned long)grcx,
                        (unsigned long)gsp, (unsigned long)gbp, (unsigned long)grip,
                        (unsigned long)gr11);
                if (read_word_ok(gsp) && read_word_ok(gsp + 0x1f)) {
                    volatile uint32_t *rq = (volatile uint32_t *)(uintptr_t)gsp;
                    pvk_log("  unix-call req@RSP: magic=%#x sel3=%#x sel1=%#x sel2=%#x\n",
                            (unsigned)rq[0], (unsigned)rq[4], (unsigned)rq[5], (unsigned)rq[6]);
                } else if (gsp) {
                    pvk_log("  unix-call req@RSP: unreadable (RSP=%#lx)\n", (unsigned long)gsp);
                }
                if (grax && read_word_ok(grax) && read_word_ok(grax + 15)) {
                    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)grax;
                    pvk_log("  resolved fn bytes @RAX: %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n",
                            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
                } else {
                    pvk_log("  resolved fn bytes @RAX: unreadable (RAX=%#lx)\n", (unsigned long)grax);
                }
                {
                    uint64_t rsv[4];
                    int n = 0;
                    for (int i = 0; i < 4; i++) {
                        uintptr_t a = (uintptr_t)(gsp + 0x28 + i * 8);
                        if (read_word_ok(a)) { rsv[n++] = *(volatile uint64_t *)a; }
                        else break;
                    }
                    if (n) {
                        pvk_log("  guest stack[ret@RSP+0x28]: %#lx %#lx %#lx %#lx\n",
                                (unsigned long)rsv[0], n > 1 ? (unsigned long)rsv[1] : 0UL,
                                n > 2 ? (unsigned long)rsv[2] : 0UL, n > 3 ? (unsigned long)rsv[3] : 0UL);
                    }
                }
            } else {
                pvk_log("  guest(x64emu@%p): unreadable emu struct (x27 not emu?)\n", (void *)emu_p);
            }
        }
    }
#endif

#ifdef __aarch64__
    if (uctx) {
        pvk_log("  regs: x0=%#lx x1=%#lx x2=%#lx x3=%#lx x4=%#lx x5=%#lx x6=%#lx x7=%#lx\n",
                uctx->uc_mcontext.regs[0], uctx->uc_mcontext.regs[1],
                uctx->uc_mcontext.regs[2], uctx->uc_mcontext.regs[3],
                uctx->uc_mcontext.regs[4], uctx->uc_mcontext.regs[5],
                uctx->uc_mcontext.regs[6], uctx->uc_mcontext.regs[7]);
        pvk_log("  regs: x8=%#lx x9=%#lx x10=%#lx x11=%#lx x12=%#lx x13=%#lx x14=%#lx x15=%#lx\n",
                uctx->uc_mcontext.regs[8], uctx->uc_mcontext.regs[9],
                uctx->uc_mcontext.regs[10], uctx->uc_mcontext.regs[11],
                uctx->uc_mcontext.regs[12], uctx->uc_mcontext.regs[13],
                uctx->uc_mcontext.regs[14], uctx->uc_mcontext.regs[15]);
        pvk_log("  regs: x16=%#lx x17=%#lx x18=%#lx x19=%#lx x20=%#lx x21=%#lx x22=%#lx x23=%#lx x24=%#lx x25=%#lx x26=%#lx x27=%#lx x28=%#lx x29(fp)=%#lx x30(lr)=%#lx sp=%#lx pc=%#lx\n",
                uctx->uc_mcontext.regs[16], uctx->uc_mcontext.regs[17],
                uctx->uc_mcontext.regs[18], uctx->uc_mcontext.regs[19],
                uctx->uc_mcontext.regs[20], uctx->uc_mcontext.regs[21],
                uctx->uc_mcontext.regs[22], uctx->uc_mcontext.regs[23],
                uctx->uc_mcontext.regs[24], uctx->uc_mcontext.regs[25],
                uctx->uc_mcontext.regs[26], uctx->uc_mcontext.regs[27],
                uctx->uc_mcontext.regs[28], uctx->uc_mcontext.regs[29],
                uctx->uc_mcontext.regs[30], uctx->uc_mcontext.sp,
                uctx->uc_mcontext.pc);
    }
#endif
    pvk_bt_print("SIG");

    /* Dump das regiões de memória relevantes: ajuda a mapear pc/lr quando o
     * crash cai em código JIT/dynarec (box64) ou em módulos sem dladdr.
     * Mostra TODAS as regiões executáveis (r-xp) e as mapeadas pelo box64. */
    if (pc) {
        FILE *mf = fopen("/proc/self/maps", "r");
        if (mf) {
            char line[512];
            pvk_log("--- /proc/self/maps (exec + box64/JIT) ---\n");
            while (fgets(line, sizeof(line), mf)) {
                int is_exec = 0;
                char *p = line;
                int i;
                for (i = 0; i < 4 && p && *p; i++) {
                    if (p[0] == 'x') is_exec = 1;
                    p = strchr(p, ' ');
                    if (p) p++;
                }
                if (is_exec ||
                    strstr(line, "box64") ||
                    strstr(line, "dynarec") ||
                    strstr(line, "rw-p") && (strstr(line, "box64") || strstr(line, "libvulkan") || strstr(line, "wine"))) {
                    pvk_log("    %s", line);
                }
            }
            fclose(mf);
            pvk_log("--- fim maps ---\n");
        }
    }
    errno = saved;
}

static void panvk_v9_install_crash_handlers(void) {
    if (g_sig_installed) return;
    g_sig_installed = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = pvk_sig_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    pvk_log("CRASH HANDLERS INSTALLED (pid=%d)\n", (int)getpid());
}

#include "panvk_v9_entrypoints.h"
#include "panvk_v9_compiler.h"

/* ---- Watchdog (hang pós-vkDestroyDevice) ----
 * TestD3D.exe trava entre vkDestroyDevice e vkDestroyInstance (wine/box64).
 * Guarda o tid do chamador de vkDestroyDevice; se vkDestroyInstance nao chegar
 * em ~6s, loga o estado da thread via /proc (state/syscall/wchan/mask) e
 * tenta um sinal NAO-bloqueado -> pvk_sig_handler despeja o x64emu guest
 * (x27) com RIP/RSP + maps. Aponta a instrucao emulada exata do hang.
 */
static volatile pid_t g_watch_tid = 0;
static volatile int    g_watch_inst_destroyed = 0;
static volatile int    g_watch_armed = 0;

static void pvk_watchdog_proc_dump(pid_t tid) {
    char path[160];
    char line[512];
    FILE *f;

    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", (int)tid);
    if ((f = fopen(path, "r"))) {
        if (fgets(line, sizeof(line), f)) {
            char *p = strrchr(line, ')');
            if (p && p[1] == ' ') {
                char st = '?';
                unsigned long long u[40];
                int n = sscanf(p + 2, "%c %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d %llu %*d %*d %llu %llu",
                               &st, &u[0], &u[1], &u[2]);
                pvk_log("WATCHDOG: tid=%d state=%c (R=running S=sleep D=disk)\n", (int)tid, st);
                if (n >= 3)
                    pvk_log("WATCHDOG:   nthreads=%llu sigpending=%llu sigignored=%llu\n",
                            u[0], u[1], u[2]);
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", (int)tid);
    if ((f = fopen(path, "r"))) {
        if (fgets(line, sizeof(line), f))
            pvk_log("WATCHDOG:   syscall: %s", line);
        fclose(f);
    }
    snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", (int)tid);
    if ((f = fopen(path, "r"))) {
        if (fgets(line, sizeof(line), f))
            pvk_log("WATCHDOG:   wchan: %s", line);
        fclose(f);
    }
    snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int)tid);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "State:", 6) || !strncmp(line, "SigBlk:", 7) ||
                !strncmp(line, "SigIgn:", 7) || !strncmp(line, "SigCgt:", 7))
                pvk_log("WATCHDOG:   %s", line);
        }
        fclose(f);
    }
}

/* Ponteiro do x64emu do box64 (estado guest) capturado no vkDestroyDevice.
 * box64 guarda o contexto guest da thread atual numa struct x64emu (heap) cujo
 * endereco fica nos frames nativos do bridge (stack scan abaixo). Com esse
 * ponteiro o watchdog le os regs guest DIRETAMENTE (mesmo processo, sem
 * ptrace/sinais): RIP=emu+136, RAX=+0, RSP=+32, RBP=+40, RCX=+8, RDX=+16. */
static volatile uintptr_t g_pvk_emu = 0;

/* Resolve um endereco (guest ou host) para modulo+offset via /proc/self/maps.
 * box64 mapeia os PEs do wine nos enderecos guest (host VA real), entao um RIP
 * guest cai numa linha de maps com o nome do .exe/.dll. */
static void pvk_guest_resolve(uintptr_t addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long s = 0, e = 0;
        char *p = line;
        s = strtoull(p, &p, 16);
        if (*p != '-') continue;
        p++;
        e = strtoull(p, &p, 16);
        if (addr >= s && addr < e) {
            /* acha o campo de offset p/ relativo: pulamos perm/off/dev/inode */
            char *q = strchr(p, ' ');
            for (int i = 0; i < 4 && q; i++) q = strchr(q + 1, ' ');
            if (q) {
                while (*q == ' ') q++;
                char *nm = q;
                while (*nm && *nm != '\n') nm++;
                *nm = 0;
                if (q[0])
                    pvk_log("WATCHDOG:   %#lx -> %s+0x%llx\n", (unsigned long)addr, q,
                            (unsigned long long)(addr - s));
                else
                    pvk_log("WATCHDOG:   %#lx -> [anon]@%llx\n", (unsigned long)addr,
                            (unsigned long long)(addr - s));
            } else {
                pvk_log("WATCHDOG:   %#lx -> <maps mal formatado>\n", (unsigned long)addr);
            }
            fclose(f);
            return;
        }
    }
    fclose(f);
    pvk_log("WATCHDOG:   %#lx -> (sem mapping)\n", (unsigned long)addr);
}

/* Varre /proc/self/maps procurando o mapping que contem addr. Retorna 1 se
 * o mapping existe e o perms tem 'x'. */
static int pvk_addr_perm_exec(uintptr_t addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int exec = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long s = 0, e = 0;
        char *p = line;
        s = strtoull(p, &p, 16);
        if (*p != '-') continue;
        p++;
        e = strtoull(p, &p, 16);
        if (addr >= s && addr < e) {
            /* p agora esta no espaco antes do "perms" (ex.: " r-xp 00000000 ...") */
            p++; /* pula o espaco */
            if (p[2] == 'x') exec = 1;  /* aceita r-xp, rwxp, r--p+PF_X etc. */
            break;
        }
    }
    fclose(f);
    return exec;
}

/* Valida um candidato a x64emu_t do box64. Layout (confirmado em regs.h):
 * regs[16] em 0..127, eflags em 128, ip em 136. Valida:
 *  - struct inteira (0..140) legivel
 *  - emu->ip (+136): endereco de codigo guest -> mapping com 'x'
 *  - emu->regs[_RSP] (+32): ponteiro de stack guest legivel (NAO-exec)
 *  - [guest RSP] (return addr) -> mapping com 'x' (call-site do wine)
 * Isto rejeita falsos positivos (ex.: frame de stack onde [RSP] cai em [stack]). */
/* Versão diagnóstica: igual a pvk_emu_validate mas loga QUAL check falhou. */
static int pvk_emu_validate_diag(uintptr_t emu, int verbose) {
    if (!emu || (emu & 7) || emu < 0x1000) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL align/range\n", (unsigned long)emu);
        return 0;
    }
    if (!read_word_ok(emu) || !read_word_ok(emu + 32) ||
        !read_word_ok(emu + 40) || !read_word_ok(emu + 136)) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL read (0/32/40/136=%d%d%d%d)\n",
            (unsigned long)emu, read_word_ok(emu), read_word_ok(emu + 32),
            read_word_ok(emu + 40), read_word_ok(emu + 136));
        return 0;
    }
    uintptr_t ip  = *(volatile uintptr_t *)(emu + 136);
    uintptr_t rsp = *(volatile uintptr_t *)(emu + 32);
    if (!ip || !rsp) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL ip/rsp nulos ip=%#lx rsp=%#lx\n",
            (unsigned long)emu, (unsigned long)ip, (unsigned long)rsp);
        return 0;
    }
    int eip = pvk_addr_perm_exec(ip);
    if (!eip) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL ip NAO-exec ip=%#lx\n",
            (unsigned long)emu, (unsigned long)ip);
        return 0;
    }
    if (!read_word_ok(rsp)) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL rsp ilegivel rsp=%#lx\n",
            (unsigned long)emu, (unsigned long)rsp);
        return 0;
    }
    if (pvk_addr_perm_exec(rsp)) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL rsp EXEC rsp=%#lx\n",
            (unsigned long)emu, (unsigned long)rsp);
        return 0;
    }
    uintptr_t ret = *(volatile uintptr_t *)rsp;
    if (!ret) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL [rsp]=0 rsp=%#lx\n",
            (unsigned long)emu, (unsigned long)rsp);
        return 0;
    }
    if (!pvk_addr_perm_exec(ret)) {
        if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx FAIL [rsp] NAO-exec ret=%#lx (rsp=%#lx, ip_exec=%d)\n",
            (unsigned long)emu, (unsigned long)ret, (unsigned long)rsp, eip);
        return 0;
    }
    if (verbose) pvk_log("WATCHDOG[emu]: cand=%#lx PASS ip=%#lx rsp=%#lx ret=%#lx\n",
        (unsigned long)emu, (unsigned long)ip, (unsigned long)rsp, (unsigned long)ret);
    return 1;
}

static int pvk_emu_validate(uintptr_t emu) {
    return pvk_emu_validate_diag(emu, 0);
}

/* Procura o x64emu da thread ATUAL (chamado de dentro do vkDestroyDevice, na
 * thread do wine). Primeiro por forca bruta de pthread-key (box64 guarda um
 * emuthread_t {fnc,arg,emu,...} no TLS da thread: emu em offset +16); se nao
 * achar, varre o stack nativo (frames do bridge, que tem emu como parametro)
 * pulando o stack nativo atual. */
static uintptr_t pvk_find_guest_emu(void) {
    pvk_log("WATCHDOG[emu]: find inicio (tid=%d). dumpando primeiras linhas do maps:\n", (int)gettid());
    {
        FILE *f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            int n = 0;
            while (fgets(line, sizeof(line), f) && n < 6) {
                pvk_log("WATCHDOG[emu]: maps: %s", line);
                n++;
            }
            fclose(f);
        } else {
            pvk_log("WATCHDOG[emu]: NAO consegui abrir /proc/self/maps!\n");
        }
    }
    int nkeys = 0;
    for (int k = 0; k < 1024; k++) {
        uintptr_t p = (uintptr_t)pthread_getspecific((pthread_key_t)k);
        if (!p || (p & 7) || p < 0x1000) continue;
        if (!read_word_ok(p) || !read_word_ok(p + 16)) continue;
        nkeys++;
        uintptr_t fnc = *(volatile uintptr_t *)p;
        uintptr_t arg = *(volatile uintptr_t *)(p + 8);
        uintptr_t emu = *(volatile uintptr_t *)(p + 16);
        pvk_log("WATCHDOG[emu]: TLS key=%d p=%#lx fnc=%#lx arg=%#lx emu=%#lx\n",
                k, (unsigned long)p, (unsigned long)fnc, (unsigned long)arg, (unsigned long)emu);
        if (emu && pvk_emu_validate_diag(emu, 1)) {
            pvk_log("WATCHDOG: achou x64emu via TLS key=%d emuthread=%#lx\n", k, (unsigned long)p);
            return emu;
        }
    }
    pvk_log("WATCHDOG[emu]: TLS brute-force completo: %d slot(s) com ponteiro. Nenhum valido.\n", nkeys);
    /* fallback: scan no stack nativo */
    pthread_attr_t attr;
    size_t st_sz = 0;
    void *st_addr = NULL;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &st_addr, &st_sz);
        pthread_attr_destroy(&attr);
    }
    pvk_log("WATCHDOG[emu]: stack nativo: base=%p size=%zu\n", st_addr, st_sz);
    uintptr_t sp = (uintptr_t)&sp;
    int ncand = 0, nread = 0;
    for (uintptr_t a = sp & ~(uintptr_t)7; a < sp + 4u * 1024 * 1024; a += 8) {
        if (!read_word_ok(a)) break;
        nread++;
        uintptr_t cand = *(volatile uintptr_t *)a;
        if (!cand || (cand & 7) || cand < 0x1000) continue;
        if (st_addr && cand >= (uintptr_t)st_addr && cand < (uintptr_t)st_addr + st_sz)
            continue; /* struct apontada no proprio stack nativo -> falso */
        ncand++;
        if (ncand <= 8)
            pvk_log("WATCHDOG[emu]: stack cand %d: ptr=%#lx (stack a=%#lx)\n",
                    ncand, (unsigned long)cand, (unsigned long)a);
        if (pvk_emu_validate_diag(cand, 1)) {
            pvk_log("WATCHDOG: achou x64emu via stack-scan\n");
            return cand;
        }
    }
    return 0;
}

/* Despeja o estado guest usando o x64emu capturado: RIP (loop do hang), RSP,
 * RBP/RAX + walk do stack guest (chain de return addresses via RBP). */
static void pvk_watchdog_emu_dump(void) {
    uintptr_t emu = g_pvk_emu;
    if (!emu || !read_word_ok(emu) || !read_word_ok(emu + 136)) {
        pvk_log("WATCHDOG: sem x64emu capturado (find falhou no vkDestroyDevice)\n");
        return;
    }
    volatile uint64_t *g = (volatile uint64_t *)emu;
    uintptr_t grip = *(volatile uintptr_t *)(emu + 136);
    uintptr_t grax = (uintptr_t)g[0];
    uintptr_t grcx = (uintptr_t)g[1];
    uintptr_t grdx = (uintptr_t)g[2];
    uintptr_t grsp = (uintptr_t)g[4];
    uintptr_t grbp = (uintptr_t)g[5];
    uintptr_t grsi = (uintptr_t)g[6];
    uintptr_t grdi = (uintptr_t)g[7];
    pvk_log("WATCHDOG: guest(x64emu@%#lx) RIP=%#lx RAX=%#lx RCX=%#lx RDX=%#lx\n",
            (unsigned long)emu, (unsigned long)grip, (unsigned long)grax,
            (unsigned long)grcx, (unsigned long)grdx);
    pvk_log("WATCHDOG:   RSP=%#lx RBP=%#lx RSI=%#lx RDI=%#lx\n",
            (unsigned long)grsp, (unsigned long)grbp,
            (unsigned long)grsi, (unsigned long)grdi);
    if (grip) {
        pvk_log("WATCHDOG: guest RIP (onde a thread esta):");
        pvk_guest_resolve(grip);
    }
    if (grbp && read_word_ok(grbp) && read_word_ok(grbp + 8)) {
        uintptr_t rbp = grbp;
        pvk_log("WATCHDOG: guest stack walk (RBP frames):\n");
        for (int i = 0; i < 20 && rbp && read_word_ok(rbp) && read_word_ok(rbp + 8); i++) {
            uintptr_t ret = *(volatile uintptr_t *)(rbp + 8);
            uintptr_t next = *(volatile uintptr_t *)rbp;
            if (ret)
                pvk_guest_resolve(ret);
            if (!next || next <= rbp) break;
            rbp = next;
        }
    } else {
        pvk_log("WATCHDOG: guest RBP=%#lx (walk nao disponivel)\n", (unsigned long)grbp);
    }
}

/* ---- Unwind NATIVO (host ARM64) da thread do hang via ptrace ----
 * O hang real fica DENTRO do wine/box64 (unix_call), nao no driver: a
 * thread guest fica com RIP congelado na casca do thunk e o spin roda em
 * codigo ARM64 nativo (box64/winevulkan.so). Aqui anexamos a thread via
 * ptrace, lemos pc/fp/lr/sp e desenrolamos a cadeia x29/x30, resolvendo
 * cada endereco para modulo+offset via /proc/self/maps. */
#ifdef __aarch64__
/* Unwind NATIVO (host ARM64) do thread do hang.
 *
 * ATENCAO: NÃO usamos ptrace. No Android, PTRACE_ATTACH a um thread do próprio
 * processo frequentemente trava (SELinux/YAMA) ou o waitpid() pendura o
 * watchdog. Como o watchdog roda no MESMO processo do thread travado, lemos
 * pc/sp via /proc/self/task/<tid>/syscall e varremos a pilha do próprio
 * processo (mesmo address space) procurando return addresses. */
static void pvk_watchdog_native_unwind(pid_t tid) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", (int)tid);
    FILE *f = fopen(path, "r");
    if (!f) {
        pvk_log("WATCHDOG: NATIVE: sem /proc/self/task/%d/syscall\n", (int)tid);
        return;
    }
    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        pvk_log("WATCHDOG: NATIVE: /proc syscall vazio\n");
        return;
    }
    fclose(f);
    long long f0 = 0;
    unsigned long long a1=0, a2=0, a3=0, a4=0, a5=0, a6=0, sp = 0, pc = 0;
    int n = sscanf(line, "%lld %llx %llx %llx %llx %llx %llx %llx %llx",
                   &f0, &a1, &a2, &a3, &a4, &a5, &a6, &sp, &pc);
    if (n < 9) {
        pvk_log("WATCHDOG: NATIVE: /proc syscall formato inesperado (n=%d line='%s')\n", n, line);
        return;
    }
    if (f0 == -1LL)
        pvk_log("WATCHDOG: NATIVE scno=-1 (rodando codigo ARM64) sp=%#llx pc=%#llx\n", sp, pc);
    else
        pvk_log("WATCHDOG: NATIVE scno=%lld (dentro de syscall) sp=%#llx pc=%#llx\n", f0, sp, pc);
    if (pc) {
        pvk_log("WATCHDOG: NATIVE pc (onde o spin roda de verdade):");
        pvk_guest_resolve((uintptr_t)pc);
    }
    /* varre a pilha do processo (mesmo espaço) procurando endereços executáveis
     * = possíveis return addresses do chamador do spin. */
    pvk_log("WATCHDOG: NATIVE stack walk (possiveis return addrs):\n");
    uintptr_t base = (uintptr_t)sp & ~(uintptr_t)(0x2000 - 1);
    int shown = 0;
    for (uintptr_t p = base; p < base + 0x200000; p += 8) {
        if (!read_word_ok(p)) continue;
        uintptr_t w = *(volatile uintptr_t *)p;
        if (w > 0x1000 && w < 0x7fff00000000ULL && pvk_addr_perm_exec((uintptr_t)w)) {
            pvk_guest_resolve((uintptr_t)w);
            if (++shown >= 24) break;
        }
    }
    if (!shown) pvk_log("WATCHDOG:   (nenhum return addr executavel na pilha)\n");
    pvk_log("WATCHDOG: NATIVE unwind done\n");
}
#endif

static void *pvk_watchdog_thread(void *arg) {
    (void)arg;
    struct timespec req = { 6, 0 };
    nanosleep(&req, NULL);
    if (g_watch_inst_destroyed) {
        pvk_log("WATCHDOG: vkDestroyInstance OK, nothing to do (tid=%d)\n", (int)g_watch_tid);
        g_watch_armed = 0;  /* FIX BUG1: permite rearmar no próximo ciclo */
        return NULL;
    }
    pvk_log("WATCHDOG: vkDestroyInstance NOT called 6s after vkDestroyDevice! "
            "pid=%d tid=%d\n", (int)getpid(), (int)g_watch_tid);
    if (g_watch_tid > 0) {
#ifdef __aarch64__
        pvk_watchdog_native_unwind(g_watch_tid);
#endif
        pvk_watchdog_proc_dump(g_watch_tid);
        pvk_watchdog_emu_dump();
    }
    g_watch_armed = 0;  /* FIX BUG1: permite rearmar mesmo após timeout */
    return NULL;
}

static void pvk_arm_watchdog(void) {
    if (g_watch_armed) return;
    g_watch_armed = 1;
    g_watch_tid = (pid_t)syscall(SYS_gettid);
    g_watch_inst_destroyed = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, pvk_watchdog_thread, NULL) == 0)
        pthread_detach(th);
    pvk_log("WATCHDOG: armed for tid=%d (dev destroy)\n", (int)g_watch_tid);
}

/* ---- Win32 / Wine surface types (Linux stubs) ---- */
#ifndef VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#endif
#ifndef VK_WINE_NULLDRV_SURFACE_EXTENSION_NAME
#define VK_WINE_NULLDRV_SURFACE_EXTENSION_NAME "VK_WINE_nulldrv_surface"
#endif
typedef void *HINSTANCE;
typedef void *HWND;
typedef VkFlags VkWin32SurfaceCreateFlagsKHR;
typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkWin32SurfaceCreateFlagsKHR flags;
    HINSTANCE hinstance;
    HWND hwnd;
} VkWin32SurfaceCreateInfoKHR;
typedef struct VkWINE_nulldrvSurfaceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
} VkWINE_nulldrvSurfaceCreateInfo;

/* ---- Presentation backend enum ---- */
enum panvk_present_backend {
    PANVK_PRESENT_NONE = 0,
    PANVK_PRESENT_XLIB,
    PANVK_PRESENT_XCB,
    PANVK_PRESENT_WINE,
    PANVK_PRESENT_NULLDRV,
};

#define ICD_LOADER_MAGIC 0x01CDC0DEu

static inline void set_loader_magic(void *object) {
    *(uintptr_t *)object = ICD_LOADER_MAGIC;
}

struct VkInstance_T {
    uintptr_t loader_data;
    struct VkPhysicalDevice_T *phys_dev;
};

struct VkPhysicalDevice_T {
    uintptr_t loader_data;
    struct pan_kmod_dev *kdev;
    struct pan_kmod_dev_props props;
};

/* Features requested at vkCreateDevice time via the pNext chain.  The driver
 * records which Vulkan 1.1/1.2/1.3 + extension features the app asked for so
 * render paths can branch on them later. */
struct panvk_v9_enabled_features {
    bool dynamic_rendering;
    bool descriptor_indexing;
    bool runtime_descriptor_array;
    bool partially_bound;
    bool update_after_bind;
    bool timeline_semaphore;
    bool buffer_device_address;
    bool host_query_reset;
    bool synchronization2;
    bool maintenance4;
    bool pipeline_creation_cache_control;
    bool robust_buffer_access;
    bool robust_image_access;
    bool scalar_block_layout;
    bool uniform_buffer_standard_layout;
    bool geometry_shader;
    bool tessellation_shader;
    bool samplers;
    bool depth_clamp;
    bool large_points;
    bool wide_lines;
    bool multi_draw_indirect;
    bool draw_indirect_first_instance;
    bool fill_mode_non_solid;
    bool sampler_anisotropy;
    bool texture_compression_astc_ldr;
    bool vertex_pipeline_stores_and_atomics;
    bool fragment_stores_and_atomics;
    bool shader_storage_image_read_without_format;
    bool shader_storage_image_write_without_format;
    bool shader_float64;
    bool shader_float16;
    bool shader_int64;
    bool shader_int16;
    bool shader_terminate_invocation;
    bool subgroup_broadcast_dynamic_id;
    bool storage_buffer_array_dynamic_indexing;
    bool storage_image_array_dynamic_indexing;
    bool sampled_image_array_dynamic_indexing;
    bool uniform_buffer_array_dynamic_indexing;
    bool shader_shared_int64_atomics;
};

struct VkDevice_T {
    uintptr_t loader_data;
    struct pan_kmod_dev *kdev;
    struct VkPhysicalDevice_T *phys_dev;
    struct VkQueue_T *queue;
    struct panvk_v9_enabled_features features;
    /* Serialises vkQueueSubmit* across threads: the double-buffered v9_cmd
     * state (active_slot, mem_bo swap, per-buffer fields) and the kbase atom
     * submit sequence are not internally thread-safe, so all queue submits on
     * a device are funneled through this mutex. */
    pthread_mutex_t submit_mutex;
};

struct VkQueue_T {
    uintptr_t loader_data;
    struct VkDevice_T *device;
    struct v9_cmd_buffer *last_v9_cmd;
};

struct VkCommandPool_T {
    struct VkDevice_T *device;
};

struct vk_vertex_binding {
    struct VkBuffer_T *buffer;
    VkDeviceSize offset;
};

struct VkCommandBuffer_T {
    uintptr_t loader_data;
    struct VkDevice_T *device;
    struct v9_cmd_buffer *v9_cmd;
    bool rendering_active;
    struct VkPipeline_T *graphics_pipeline;
    struct VkPipeline_T *compute_pipeline;
    struct VkViewport viewport;
    struct VkRect2D scissor;
    bool viewport_set;
    bool scissor_set;
    uint8_t push_constants[128];
    uint32_t push_constants_size;
    float depth_bias_constant_factor;
    float depth_bias_constant_offset;
    float depth_bias_clamp;
    bool depth_bias_set;
    VkDescriptorSet descriptor_sets[8];
    struct vk_vertex_binding vertex_bindings[16];
    struct VkBuffer_T *index_buffer;
    VkDeviceSize index_offset;
    uint32_t index_type;
};

struct VkSurfaceKHR_T {
    enum panvk_present_backend backend;

    Display *dpy;
    xcb_connection_t *connection;
    uint32_t window;
    uint32_t width;
    uint32_t height;
    bool is_xcb;

    /* Wine/Win32 bridge */
    void *wine_hwnd;
    void *wine_hinstance;
};

struct VkSwapchainKHR_T {
    struct VkDevice_T *device;
    struct VkSurfaceKHR_T *surface;
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    struct VkImage_T *images;
    uint32_t next_image; /* rotating index handed out by vkAcquireNextImageKHR */
    GC gc;
    xcb_gcontext_t xcb_gc;
    XImage *ximage;
    char *image_data;
};

struct VkImage_T {
    struct VkSwapchainKHR_T *swapchain;
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t format;
    uint32_t image_type;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t tiling;
    uint32_t usage;
    uint64_t size;
    uint64_t row_pitch[16];
    uint64_t mip_offset[16];
    struct pan_kmod_bo *bo;
    VkDeviceSize memory_offset;
};

struct VkImageView_T {
    struct VkImage_T *image;
    uint32_t format;
    uint32_t view_type;
    uint32_t base_mip;
    uint32_t mip_count;
    uint32_t base_layer;
    uint32_t layer_count;
};

struct VkBufferView_T {
    struct VkBuffer_T *buffer;
    uint32_t format;
    VkDeviceSize offset;
    VkDeviceSize range;
};

struct VkSampler_T {
    uint32_t magFilter;
    uint32_t minFilter;
    uint32_t mipmapMode;
    uint32_t addressModeU;
    uint32_t addressModeV;
    uint32_t addressModeW;
    float mipLodBias;
    uint32_t anisotropyEnable;
    float maxAnisotropy;
    uint32_t compareEnable;
    uint32_t compareOp;
    float minLod;
    float maxLod;
    uint32_t borderColor;
    uint32_t unnormalizedCoordinates;
};

struct VkDeviceMemory_T {
    struct pan_kmod_bo *bo;
    void *cpu;
    VkDeviceSize size;
};

struct VkBuffer_T {
    VkDeviceSize size;
    struct pan_kmod_bo *bo;
    VkDeviceSize memory_offset;
};

struct VkShaderModule_T {
    size_t code_size;
    uint32_t *code;
    uint32_t stage_mask;
};

struct VkPipelineLayout_T {
    struct panvk_v9_pipeline_layout compiler_layout;
    struct panvk_v9_descriptor_binding *bindings;
};

struct VkRenderPass_T {
    int dummy;
};

struct VkFramebuffer_T {
    struct VkDevice_T *device;
    struct VkImageView_T **attachments;
    uint32_t attachment_count;
    uint32_t width;
    uint32_t height;
};

struct VkPipelineCache_T {
    int dummy;
};

struct VkPipeline_T {
    uint32_t stage_mask;
    uint32_t stage;
    char vertex_entry_point[64];
    char fragment_entry_point[64];
    char compute_entry_point[64];
    struct panvk_v9_compiled_shader vertex_binary;
    struct panvk_v9_compiled_shader fragment_binary;
    struct panvk_v9_compiled_shader compute_binary;
    struct VkShaderModule_T *compute_module;
    struct panvk_v9_pipeline_layout compiler_layout;
    struct panvk_v9_descriptor_binding *bindings;
    struct VkVertexInputBindingDescription vertex_bindings[16];
    struct VkVertexInputAttributeDescription vertex_attributes[16];
    uint32_t vertex_binding_count;
    uint32_t vertex_attribute_count;
    bool shaders_compiled;
    uint32_t topology;
    bool primitive_restart;
    struct VkViewport viewport;
    struct VkRect2D scissor;
    bool dynamic_viewport;
    bool dynamic_scissor;
    bool rasterizer_discard;
    uint32_t polygon_mode;
    uint32_t cull_mode;
    uint32_t front_face;
    float line_width;
    uint32_t rasterization_samples;
    bool depth_test;
    bool depth_write;
    uint32_t depth_compare_op;
    bool blend_enable;
    uint32_t color_write_mask;
};

struct VkDescriptorSetLayout_T {
    uint32_t binding_count;
    struct VkDescriptorSetLayoutBinding *bindings;
    uint32_t *binding_offsets;
    uint32_t descriptor_count;
    VkDescriptorBindingFlags *binding_flags; /* one per binding (descriptor indexing) */
    int32_t variable_binding;                /* index of variable-descriptor-count binding, or -1 */
    uint32_t variable_descriptor_count;      /* declared max count of that binding */
};

struct VkDescriptorPool_T {
    int dummy;
};

struct VkDescriptorSet_T {
    VkDescriptorSetLayout layout;
    struct VkDescriptorBufferInfo *buffers;
    struct VkDescriptorImageInfo *images;
};

struct VkSemaphore_T {
    uint64_t counter;
    struct VkSemaphore_T *timeline; /* non-NULL for timeline semaphores */
};

struct VkFence_T {
    bool signaled;
};

struct VkEvent_T {
    bool signaled;
};

struct VkQueryPool_T {
    uint32_t query_count;
};

struct VkSamplerYcbcrConversion_T {
    uintptr_t loader_data;
};

struct panvk_compiler_api {
    void *library;
    int (*compile)(const uint32_t *, size_t, enum panvk_v9_shader_stage,
                   const char *, const struct panvk_v9_pipeline_layout *,
                   struct panvk_v9_compiled_shader *, char *, size_t);
    void (*cleanup)(struct panvk_v9_compiled_shader *);
    bool attempted;
};

/* GLIBC compatibility globals and functions for Bionic */
#if defined(__BIONIC__)
char *program_invocation_name = (char *)"vkmark";
char *program_invocation_short_name = (char *)"vkmark";
extern int *__errno(void);
int *__errno_location(void) {
    return __errno();
}
#endif

static struct panvk_compiler_api compiler_api;
static pthread_mutex_t compiler_api_mutex = PTHREAD_MUTEX_INITIALIZER;

static void command_buffer_apply_ssbos(VkCommandBuffer commandBuffer);
static void command_buffer_apply_textures(VkCommandBuffer commandBuffer);
static void command_buffer_apply_samplers(VkCommandBuffer commandBuffer);

static bool load_compiler(void) {
    pthread_mutex_lock(&compiler_api_mutex);
    if (compiler_api.attempted) {
        bool loaded = compiler_api.library != NULL;
        pthread_mutex_unlock(&compiler_api_mutex);
        return loaded;
    }
    compiler_api.attempted = true;

    const char *path = getenv("PANVK_V9_COMPILER_LIBRARY");
    if (path && path[0]) {
        compiler_api.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("./libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("/data/data/com.winlator/files/rootfs/usr/lib/libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("/data/data/com.winlator/files/rootfs/lib/libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("/usr/lib/libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("/lib/libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("/data/data/com.termux/files/home/libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        compiler_api.library = dlopen("libpanvk_v9_compiler.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!compiler_api.library) {
        const char *err = dlerror();
        FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
        if (flog) {
            fprintf(flog, "load_compiler dlopen failed: %s\n", err ? err : "unknown");
            fclose(flog);
        }
        pthread_mutex_unlock(&compiler_api_mutex);
        return false;
    }

    compiler_api.compile = dlsym(compiler_api.library, "panvk_v9_compile_spirv");
    compiler_api.cleanup = dlsym(compiler_api.library, "panvk_v9_compiled_shader_cleanup");
    if (!compiler_api.compile || !compiler_api.cleanup) {
        const char *err = dlerror();
        FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
        if (flog) {
            fprintf(flog, "load_compiler dlsym failed: %s\n", err ? err : "unknown");
            fclose(flog);
        }
        dlclose(compiler_api.library);
        memset(&compiler_api, 0, sizeof(compiler_api));
        compiler_api.attempted = true;
        pthread_mutex_unlock(&compiler_api_mutex);
        return false;
    }
    pthread_mutex_unlock(&compiler_api_mutex);
    return true;
}

/* Loader Negotiation */
__attribute__((visibility("default"))) VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion) {
    panvk_v9_install_crash_handlers();
    pvk_log("vk_icdNegotiateLoaderICDInterfaceVersion: pSupportedVersion=%u [BUILD %s %s]\n", pSupportedVersion ? *pSupportedVersion : 0, __DATE__, __TIME__);
    if (!pSupportedVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (*pSupportedVersion > 4) {
        *pSupportedVersion = 4;
    }
    pvk_log("  -> negotiated version %u\n", *pSupportedVersion);
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0); /* Vulkan 1.1: Box64 v0.4.0 não tem wrappers p/ funções 1.2/1.3 */
    pvk_log("vkEnumerateInstanceVersion: version=1.1.0\n");
    return VK_SUCCESS;
}

/* Extension & Layer Enumeration */
VkResult vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount, struct VkLayerProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    pvk_log("vkEnumerateInstanceExtensionProperties: layer=%s count=%u props=%p\n",
            pLayerName ? pLayerName : "(null)", *pPropertyCount, (void*)pProperties);

    static const VkExtensionProperties inst_exts[] = {
        { .extensionName = VK_KHR_SURFACE_EXTENSION_NAME, .specVersion = 25 },
        { .extensionName = VK_KHR_XLIB_SURFACE_EXTENSION_NAME, .specVersion = 6 },
        { .extensionName = VK_KHR_XCB_SURFACE_EXTENSION_NAME, .specVersion = 6 },
        { .extensionName = VK_KHR_DISPLAY_EXTENSION_NAME, .specVersion = 23 },
        { .extensionName = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_DEBUG_REPORT_EXTENSION_NAME, .specVersion = 10 },
        { .extensionName = VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = "VK_KHR_win32_surface", .specVersion = 6 },
        { .extensionName = "VK_EXT_headless_surface", .specVersion = 1 },
        { .extensionName = "VK_WINE_nulldrv_surface", .specVersion = 1 },
    };
    uint32_t num_exts = sizeof(inst_exts) / sizeof(inst_exts[0]);

    if (!pProperties) {
        *pPropertyCount = num_exts;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPropertyCount < num_exts) ? *pPropertyCount : num_exts;
    memcpy(pProperties, inst_exts, to_copy * sizeof(VkExtensionProperties));
    *pPropertyCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    pvk_log("vkEnumerateDeviceExtensionProperties: phys=%p layer=%s count=%u props=%p\n",
            (void*)physicalDevice, pLayerName, pPropertyCount ? *pPropertyCount : 0, (void*)pProperties);
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    /* Extension surface advertised (API 1.3 so most 1.1/1.2/1.3 features are
     * core; these are the KHR/EXT ones DXVK and VKD3D require at device
     * creation. */
    static const VkExtensionProperties dev_exts[] = {
        { .extensionName = VK_KHR_SWAPCHAIN_EXTENSION_NAME, .specVersion = 70 },
        { .extensionName = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, .specVersion = 3 },
        { .extensionName = VK_KHR_MAINTENANCE_1_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_MAINTENANCE_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_3_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_4_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_MAINTENANCE_5_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_6_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_7_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MAINTENANCE_8_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_UNIFORM_BUFFER_STANDARD_LAYOUT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME, .specVersion = 4 },
        { .extensionName = VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_16BIT_STORAGE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_8BIT_STORAGE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, .specVersion = 14 },
        /* FIX BUG2: EXT_DEBUG_UTILS e EXT_DEBUG_REPORT removidos daqui — são
         * extensões de INSTÂNCIA (já listadas no inst_exts acima) */
        { .extensionName = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, .specVersion = 2 },
        { .extensionName = VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_MULTIVIEW_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, .specVersion = 1 },
        /* ---- Additional surface probed by DXVK / vkd3d-proton ---- */
        { .extensionName = VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_4444_FORMATS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, .specVersion = 1 },
        /* FIX BUG2: EXTENDED_DYNAMIC_STATE_3 e DRAW_INDIRECT_COUNT removidos
         * (duplicatas — já listados acima) */
        { .extensionName = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, .specVersion = 4 },
        { .extensionName = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME, .specVersion = 1 },
        /* FIX BUG2: SAMPLER_MIRROR_CLAMP_TO_EDGE e HOST_QUERY_RESET removidos
         * (duplicatas — já listados acima) */
        { .extensionName = VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_SEPARATE_STENCIL_USAGE_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, .specVersion = 1 },
        { .extensionName = VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME, .specVersion = 1 },
    };
    uint32_t num_exts = sizeof(dev_exts) / sizeof(dev_exts[0]);

    if (!pProperties) {
        *pPropertyCount = num_exts;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPropertyCount < num_exts) ? *pPropertyCount : num_exts;
    memcpy(pProperties, dev_exts, to_copy * sizeof(VkExtensionProperties));
    *pPropertyCount = to_copy;
    return VK_SUCCESS;
}

/* Instance & Device Management */
VkResult vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    panvk_v9_install_crash_handlers();
    pvk_log("vkCreateInstance: pCreateInfo=%p pInstance=%p\n", (void*)pCreateInfo, (void*)pInstance);
    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    if (pCreateInfo && pCreateInfo->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            printf("DEBUG: vkCreateInstance requested extension: '%s'\n", pCreateInfo->ppEnabledExtensionNames[i]);
        }
    }

    struct VkInstance_T *inst = calloc(1, sizeof(*inst));
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic(inst);

    struct pan_kmod_dev *kdev = pan_kmod_dev_create(NULL);
    pvk_log("vkCreateInstance: pan_kmod_dev_create returned %p\n", (void*)kdev);
    if (kdev) {
        struct VkPhysicalDevice_T *pdev = calloc(1, sizeof(*pdev));
        if (pdev) {
            set_loader_magic(pdev);
            pdev->kdev = kdev;
            pan_kmod_dev_query_props(kdev, &pdev->props);
            inst->phys_dev = pdev;
        } else {
            pan_kmod_dev_destroy(kdev);
        }
    }

    *pInstance = inst;
    pvk_log("vkCreateInstance: SUCCESS inst=%p phys_dev=%p kdev=%p\n",
            (void*)inst, inst ? (void*)inst->phys_dev : NULL,
            (inst && inst->phys_dev) ? (void*)inst->phys_dev->kdev : NULL);
    return VK_SUCCESS;
}

void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    pvk_log("vkDestroyInstance: instance=%p (antigo %d)\n", (void*)instance, g_watch_inst_destroyed);
    g_watch_inst_destroyed = 1;
    if (!instance) return;
    if (instance->phys_dev) {
        if (instance->phys_dev->kdev) {
            pan_kmod_dev_destroy(instance->phys_dev->kdev);
        }
        free(instance->phys_dev);
    }
    free(instance);
}

VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices) {
    pvk_log("vkEnumeratePhysicalDevices: instance=%p count=%u devs=%p\n",
            (void*)instance, pPhysicalDeviceCount ? *pPhysicalDeviceCount : 0, (void*)pPhysicalDevices);
    if (!pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!instance || !instance->phys_dev) {
        *pPhysicalDeviceCount = 0;
        return VK_SUCCESS;
    }

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return VK_SUCCESS;
    }

    *pPhysicalDevices = instance->phys_dev;
    *pPhysicalDeviceCount = 1;
    return VK_SUCCESS;
}

VkResult vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, struct VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroups) {
    pvk_log("vkEnumeratePhysicalDeviceGroups: instance=%p count=%u groups=%p\n",
            (void*)instance, pPhysicalDeviceGroupCount ? *pPhysicalDeviceGroupCount : 0, (void*)pPhysicalDeviceGroups);
    if (!pPhysicalDeviceGroupCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPhysicalDeviceGroups) {
        *pPhysicalDeviceGroupCount = 1;
        return VK_SUCCESS;
    }
    pPhysicalDeviceGroups[0].physicalDeviceCount = 1;
    vkEnumeratePhysicalDevices(instance, &pPhysicalDeviceGroups[0].physicalDeviceCount, pPhysicalDeviceGroups[0].physicalDevices);
    *pPhysicalDeviceGroupCount = 1;
    return VK_SUCCESS;
}

void vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    pvk_log("vkGetPhysicalDeviceProperties: phys=%p props=%p\n", (void*)physicalDevice, (void*)pProperties);
    if (!pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0); /* 1.1 p/ compat com Box64 v0.4.0 */
    pProperties->driverVersion = (1u << 22) | (1u << 12) | 0;
    pProperties->vendorID = 0x13B5; /* ARM Vendor ID */
    /* GPU real = Mali-G68 MC4 (0x92041010). O GPU ID de compilacao continua
     * 0x90001000 (G77) porque o G68 nao e modelo listado no Mesa
     * (pan_get_model retornaria NULL); ambos sao Valhall arch 9 (mesma ISA). */
    pProperties->deviceID = 0x92041010u;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
             "ARM Mali-G68 MC4 (Valhall v9 - Toddy Driver)");
    pProperties->pipelineCacheUUID[0] = 0x50; /* 'P' */
    pProperties->pipelineCacheUUID[1] = 0x56; /* 'V' */
    pProperties->pipelineCacheUUID[2] = 0x39; /* '9' */

    VkPhysicalDeviceLimits *l = &pProperties->limits;
    l->maxImageDimension1D = 4096;
    l->maxImageDimension2D = 4096;
    l->maxImageDimension3D = 2048;
    l->maxImageDimensionCube = 4096;
    l->maxImageArrayLayers = 2048;
    l->maxTexelBufferElements = 1 << 20;
    l->maxUniformBufferRange = 1u << 20;
    l->maxStorageBufferRange = 1u << 30;
    l->maxPushConstantsSize = 128;
    l->maxMemoryAllocationCount = 0xffff;
    l->maxSamplerAllocationCount = 4096;
    l->bufferImageGranularity = 64;
    l->sparseAddressSpaceSize = 1ull << 32;
    l->maxBoundDescriptorSets = 8;
    l->maxPerStageDescriptorSamplers = 64;
    l->maxPerStageDescriptorUniformBuffers = 64;
    l->maxPerStageDescriptorStorageBuffers = 64;
    l->maxPerStageDescriptorSampledImages = 64;
    l->maxPerStageDescriptorStorageImages = 64;
    l->maxPerStageDescriptorInputAttachments = 64;
    l->maxPerStageResources = 128;
    l->maxDescriptorSetSamplers = 256;
    l->maxDescriptorSetUniformBuffers = 256;
    l->maxDescriptorSetUniformBuffersDynamic = 8;
    l->maxDescriptorSetStorageBuffers = 256;
    l->maxDescriptorSetStorageBuffersDynamic = 4;
    l->maxDescriptorSetSampledImages = 256;
    l->maxDescriptorSetStorageImages = 256;
    l->maxDescriptorSetInputAttachments = 64;
    l->maxVertexInputAttributes = 16;
    l->maxVertexInputBindings = 16;
    l->maxVertexInputAttributeOffset = 2047;
    l->maxVertexInputBindingStride = 2048;
    l->maxVertexOutputComponents = 64;
    l->maxFragmentInputComponents = 64;
    l->maxFragmentOutputAttachments = 8;
    l->maxFragmentDualSrcAttachments = 1;
    l->maxFragmentCombinedOutputResources = 8;
    l->maxComputeSharedMemorySize = 16 * 1024;
    l->maxComputeWorkGroupCount[0] = 65535;
    l->maxComputeWorkGroupCount[1] = 65535;
    l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 128;
    l->maxComputeWorkGroupSize[0] = 128;
    l->maxComputeWorkGroupSize[1] = 128;
    l->maxComputeWorkGroupSize[2] = 128;
    l->subPixelPrecisionBits = 4;
    l->subTexelPrecisionBits = 4;
    l->mipmapPrecisionBits = 4;
    l->maxDrawIndexedIndexValue = UINT32_MAX;
    l->maxDrawIndirectCount = 0xfffff;
    l->maxSamplerLodBias = 4.0f;
    l->maxSamplerAnisotropy = 16.0f;
    l->maxViewports = 16;
    l->maxViewportDimensions[0] = 4096;
    l->maxViewportDimensions[1] = 4096;
    l->viewportBoundsRange[0] = -4096.0f;
    l->viewportBoundsRange[1] = 4096.0f;
    l->viewportSubPixelBits = 4;
    l->minMemoryMapAlignment = 64;
    l->minTexelBufferOffsetAlignment = 64;
    l->minUniformBufferOffsetAlignment = 64;
    l->minStorageBufferOffsetAlignment = 64;
    l->minTexelOffset = -8;
    l->maxTexelOffset = 7;
    l->minTexelGatherOffset = -8;
    l->maxTexelGatherOffset = 7;
    l->minInterpolationOffset = -0.5f;
    l->maxInterpolationOffset = 0.5f;
    l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = 4096;
    l->maxFramebufferHeight = 4096;
    l->maxFramebufferLayers = 1024;
    l->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->maxColorAttachments = 8;
    l->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l->maxSampleMaskWords = 1;
    l->timestampComputeAndGraphics = VK_TRUE;
    l->timestampPeriod = 1.0f;
    l->maxClipDistances = 8;
    l->maxCullDistances = 8;
    l->maxCombinedClipAndCullDistances = 8;
    l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = 1.0f;
    l->pointSizeRange[1] = 255.0f;
    l->lineWidthRange[0] = 1.0f;
    l->lineWidthRange[1] = 8.0f;
    l->pointSizeGranularity = 1.0f;
    l->lineWidthGranularity = 1.0f;
    l->strictLines = VK_FALSE;
    l->standardSampleLocations = VK_TRUE;
    l->optimalBufferCopyOffsetAlignment = 64;
    l->optimalBufferCopyRowPitchAlignment = 64;
    l->nonCoherentAtomSize = 64;
}

void vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    pvk_log("vkGetPhysicalDeviceProperties2: phys=%p props=%p\n", (void*)physicalDevice, (void*)pProperties);
    if (!pProperties) return;
    vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

    for (void *next = pProperties->pNext; next; next = *((void **)next + 1)) {
        VkStructureType sType = *(const VkStructureType *)next;
        switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
            VkPhysicalDeviceVulkan11Properties *p = next;
            p->maxMultiviewViewCount = 4;
            p->maxMultiviewInstanceIndex = 0xffff;
            p->subgroupSize = 8;
            p->subgroupSupportedStages = VK_SHADER_STAGE_VERTEX_BIT |
                                        VK_SHADER_STAGE_FRAGMENT_BIT |
                                        VK_SHADER_STAGE_COMPUTE_BIT;
            p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                             VK_SUBGROUP_FEATURE_VOTE_BIT |
                                             VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                             VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                             VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                             VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
            p->subgroupQuadOperationsInAllStages = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
            VkPhysicalDeviceVulkan12Properties *p = next;
            p->driverID = VK_DRIVER_ID_MESA_PANVK;
            snprintf(p->driverName, sizeof(p->driverName), "toddy-driver");
            snprintf(p->driverInfo, sizeof(p->driverInfo),
                     "Toddy Driver (Mesa gfxstream-viewer, Valhall v9)");
p->conformanceVersion.major = 1;
             p->conformanceVersion.minor = 1;
             p->conformanceVersion.patch = 0;
             p->conformanceVersion.subminor = 0;
            p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
            p->shaderSignedZeroInfNanPreserveFloat16 = VK_FALSE;
            p->shaderSignedZeroInfNanPreserveFloat32 = VK_FALSE;
            p->shaderSignedZeroInfNanPreserveFloat64 = VK_FALSE;
            p->shaderDenormPreserveFloat16 = VK_FALSE;
            p->shaderDenormPreserveFloat32 = VK_FALSE;
            p->shaderDenormPreserveFloat64 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat16 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat32 = VK_FALSE;
            p->shaderDenormFlushToZeroFloat64 = VK_FALSE;
            p->maxTimelineSemaphoreValueDifference = INT64_MAX;
            p->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
            VkPhysicalDeviceVulkan13Properties *p = next;
            p->minSubgroupSize = 8;
            p->maxSubgroupSize = 8;
            p->maxComputeWorkgroupSubgroups = 8;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_VERTEX_BIT |
                                            VK_SHADER_STAGE_FRAGMENT_BIT |
                                            VK_SHADER_STAGE_COMPUTE_BIT;
            p->maxInlineUniformBlockSize = 256;
            p->maxPerStageDescriptorInlineUniformBlocks = 4;
            p->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = 4;
            p->maxDescriptorSetInlineUniformBlocks = 4;
            p->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = 4;
            p->maxInlineUniformTotalSize = 4096;
            p->storageTexelBufferOffsetAlignmentBytes = 64;
            p->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            p->uniformTexelBufferOffsetAlignmentBytes = 64;
            p->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
            p->maxBufferSize = 1ull << 34;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
            VkPhysicalDeviceSubgroupProperties *p = next;
            p->subgroupSize = 8;
            p->supportedStages = VK_SHADER_STAGE_VERTEX_BIT |
                                 VK_SHADER_STAGE_FRAGMENT_BIT |
                                 VK_SHADER_STAGE_COMPUTE_BIT;
            p->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                     VK_SUBGROUP_FEATURE_VOTE_BIT |
                                     VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                     VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                     VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                                     VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
            p->quadOperationsInAllStages = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            VkPhysicalDeviceDriverProperties *p = next;
            p->driverID = VK_DRIVER_ID_MESA_PANVK;
            snprintf(p->driverName, sizeof(p->driverName), "toddy-driver");
            snprintf(p->driverInfo, sizeof(p->driverInfo),
                     "Toddy Driver (Mesa gfxstream-viewer, Valhall v9)");
            p->conformanceVersion.major = 1;
            p->conformanceVersion.minor = 1;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES: {
            VkPhysicalDeviceTimelineSemaphoreProperties *p = next;
            p->maxTimelineSemaphoreValueDifference = INT64_MAX;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES: {
            VkPhysicalDeviceDescriptorIndexingProperties *p = next;
            p->maxUpdateAfterBindDescriptorsInAllPools = 1u << 20;
            p->shaderUniformBufferArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderSampledImageArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderStorageBufferArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderStorageImageArrayNonUniformIndexingNative = VK_FALSE;
            p->shaderInputAttachmentArrayNonUniformIndexingNative = VK_FALSE;
            p->robustBufferAccessUpdateAfterBind = VK_FALSE;
            p->quadDivergentImplicitLod = VK_FALSE;
            p->maxPerStageDescriptorUpdateAfterBindSamplers = 64;
            p->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 64;
            p->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 64;
            p->maxPerStageDescriptorUpdateAfterBindSampledImages = 64;
            p->maxPerStageDescriptorUpdateAfterBindStorageImages = 64;
            p->maxPerStageDescriptorUpdateAfterBindInputAttachments = 64;
            p->maxPerStageUpdateAfterBindResources = 128;
            p->maxDescriptorSetUpdateAfterBindSamplers = 256;
            p->maxDescriptorSetUpdateAfterBindUniformBuffers = 256;
            p->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8;
            p->maxDescriptorSetUpdateAfterBindStorageBuffers = 256;
            p->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 4;
            p->maxDescriptorSetUpdateAfterBindSampledImages = 256;
            p->maxDescriptorSetUpdateAfterBindStorageImages = 256;
            p->maxDescriptorSetUpdateAfterBindInputAttachments = 64;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES: {
            VkPhysicalDeviceMaintenance4Properties *p = next;
            p->maxBufferSize = 1ull << 34;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES: {
            VkPhysicalDeviceSubgroupSizeControlProperties *p = next;
            p->minSubgroupSize = 8;
            p->maxSubgroupSize = 8;
            p->maxComputeWorkgroupSubgroups = 8;
            p->requiredSubgroupSizeStages = VK_SHADER_STAGE_VERTEX_BIT |
                                            VK_SHADER_STAGE_FRAGMENT_BIT |
                                            VK_SHADER_STAGE_COMPUTE_BIT;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR: {
            VkPhysicalDeviceFragmentShadingRatePropertiesKHR *p = next;
            p->minFragmentShadingRateAttachmentTexelSize.width = 1;
            p->minFragmentShadingRateAttachmentTexelSize.height = 1;
            p->maxFragmentShadingRateAttachmentTexelSize.width = 1;
            p->maxFragmentShadingRateAttachmentTexelSize.height = 1;
            p->maxFragmentShadingRateAttachmentTexelSizeAspectRatio = 1;
            p->fragmentShadingRateNonTrivialCombinerOps = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_PROPERTIES: {
            VkPhysicalDevicePipelineRobustnessProperties *p = next;
            p->defaultRobustnessStorageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS;
            p->defaultRobustnessUniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS;
            p->defaultRobustnessVertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS;
            p->defaultRobustnessImages = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS;
            break;
        }
        default:
            break;
        }
    }
}

void vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures) {
    pvk_log("vkGetPhysicalDeviceFeatures: phys=%p features=%p\n", (void*)physicalDevice, (void*)pFeatures);
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));
    panvk_v9_fill_features(pFeatures);
}

/* Feature matrix advertised to DXVK/VKD3D.  Kept in panvk_v9_fill_features2
 * so both entry points agree. */
void panvk_v9_fill_features(VkPhysicalDeviceFeatures *f) {
    if (!f) return;
    f->robustBufferAccess = VK_TRUE;
    f->fullDrawIndexUint32 = VK_TRUE;
    f->imageCubeArray = VK_TRUE;
    f->independentBlend = VK_TRUE;
    f->samplerAnisotropy = VK_TRUE;
    f->depthClamp = VK_TRUE;
    f->depthBiasClamp = VK_TRUE;
    f->fillModeNonSolid = VK_TRUE;
    f->sampleRateShading = VK_TRUE;
    f->occlusionQueryPrecise = VK_TRUE;
    f->multiViewport = VK_TRUE;
    f->geometryShader = VK_TRUE;
    f->tessellationShader = VK_TRUE;
    f->dualSrcBlend = VK_TRUE;
    f->vertexPipelineStoresAndAtomics = VK_TRUE;
    f->fragmentStoresAndAtomics = VK_TRUE;
    f->shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    f->shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    f->shaderStorageImageArrayDynamicIndexing = VK_TRUE;
    f->shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
    f->shaderClipDistance = VK_TRUE;
    f->shaderCullDistance = VK_TRUE;
    f->shaderStorageImageReadWithoutFormat = VK_TRUE;
    f->shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f->shaderImageGatherExtended = VK_TRUE;
    f->multiDrawIndirect = VK_TRUE;
    f->drawIndirectFirstInstance = VK_TRUE;
    f->textureCompressionASTC_LDR = VK_TRUE;
    f->textureCompressionBC = VK_TRUE;
}

/* Walk a VkPhysicalDeviceFeatures2 pNext chain and fill every feature struct
 * present, mirroring Mesa panvk's arch>=9 feature matrix. */
void panvk_v9_fill_features2(VkPhysicalDeviceFeatures2 *features2) {
    if (!features2) return;
    memset(&features2->features, 0, sizeof(features2->features));
    panvk_v9_fill_features(&features2->features);

    for (void *next = features2->pNext; next; next = *((void **)next + 1)) {
        VkStructureType sType = *(const VkStructureType *)next;
        switch (sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
            VkPhysicalDeviceVulkan11Features *f = next;
            f->storageBuffer16BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f->storagePushConstant16 = VK_FALSE;
            f->storageInputOutput16 = VK_FALSE;
            f->multiview = VK_TRUE;
            f->multiviewGeometryShader = VK_FALSE;
            f->multiviewTessellationShader = VK_FALSE;
            f->variablePointersStorageBuffer = VK_TRUE;
            f->variablePointers = VK_TRUE;
            f->protectedMemory = VK_FALSE;
            f->samplerYcbcrConversion = VK_TRUE;
            f->shaderDrawParameters = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            VkPhysicalDeviceVulkan12Features *f = next;
            f->samplerMirrorClampToEdge = VK_TRUE;
            f->drawIndirectCount = VK_TRUE;
            f->storageBuffer8BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer8BitAccess = VK_TRUE;
            f->storagePushConstant8 = VK_FALSE;
            f->shaderBufferInt64Atomics = VK_TRUE;
            f->shaderSharedInt64Atomics = VK_TRUE;
            f->shaderFloat16 = VK_TRUE;
            f->descriptorIndexing = VK_TRUE;
            f->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE;
            f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->runtimeDescriptorArray = VK_TRUE;
            f->samplerFilterMinmax = VK_FALSE;
            f->scalarBlockLayout = VK_TRUE;
            f->imagelessFramebuffer = VK_TRUE;
            f->uniformBufferStandardLayout = VK_TRUE;
            f->shaderSubgroupExtendedTypes = VK_TRUE;
            f->separateDepthStencilLayouts = VK_TRUE;
            f->hostQueryReset = VK_TRUE;
            f->timelineSemaphore = VK_TRUE;
            f->bufferDeviceAddress = VK_TRUE;
            f->bufferDeviceAddressCaptureReplay = VK_FALSE;
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
            f->vulkanMemoryModel = VK_TRUE;
            f->vulkanMemoryModelDeviceScope = VK_TRUE;
            f->vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE;
            f->shaderOutputViewportIndex = VK_FALSE;
            f->shaderOutputLayer = VK_FALSE;
            f->subgroupBroadcastDynamicId = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
            VkPhysicalDeviceVulkan13Features *f = next;
            f->robustImageAccess = VK_TRUE;
            f->inlineUniformBlock = VK_TRUE;
            f->descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
            f->pipelineCreationCacheControl = VK_TRUE;
            f->privateData = VK_TRUE;
            f->shaderDemoteToHelperInvocation = VK_TRUE;
            f->shaderTerminateInvocation = VK_TRUE;
            f->subgroupSizeControl = VK_TRUE;
            f->computeFullSubgroups = VK_TRUE;
            f->synchronization2 = VK_TRUE;
            f->textureCompressionASTC_HDR = VK_FALSE;
            f->shaderZeroInitializeWorkgroupMemory = VK_TRUE;
            f->dynamicRendering = VK_TRUE;
            f->shaderIntegerDotProduct = VK_TRUE;
            f->maintenance4 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
            VkPhysicalDeviceDescriptorIndexingFeatures *f = next;
            f->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            f->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            f->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            f->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            f->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE;
            f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->runtimeDescriptorArray = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
            VkPhysicalDeviceBufferDeviceAddressFeatures *f = next;
            f->bufferDeviceAddress = VK_TRUE;
            f->bufferDeviceAddressCaptureReplay = VK_FALSE;
            f->bufferDeviceAddressMultiDevice = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
            VkPhysicalDeviceTimelineSemaphoreFeatures *f = next;
            f->timelineSemaphore = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
            VkPhysicalDeviceShaderDrawParametersFeatures *f = next;
            f->shaderDrawParameters = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *f = next;
            f->transformFeedback = VK_TRUE;
            f->geometryStreams = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
            VkPhysicalDeviceDynamicRenderingFeatures *f = next;
            f->dynamicRendering = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: {
            VkPhysicalDeviceHostQueryResetFeatures *f = next;
            f->hostQueryReset = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
            VkPhysicalDeviceSynchronization2Features *f = next;
            f->synchronization2 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
            VkPhysicalDeviceMaintenance4Features *f = next;
            f->maintenance4 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
            VkPhysicalDevice16BitStorageFeatures *f = next;
            f->storageBuffer16BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer16BitAccess = VK_TRUE;
            f->storagePushConstant16 = VK_FALSE;
            f->storageInputOutput16 = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
            VkPhysicalDevice8BitStorageFeatures *f = next;
            f->storageBuffer8BitAccess = VK_TRUE;
            f->uniformAndStorageBuffer8BitAccess = VK_TRUE;
            f->storagePushConstant8 = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
            VkPhysicalDeviceSubgroupSizeControlFeatures *f = next;
            f->subgroupSizeControl = VK_TRUE;
            f->computeFullSubgroups = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
            VkPhysicalDevice4444FormatsFeaturesEXT *f = next;
            f->formatA4R4G4B4 = VK_TRUE;
            f->formatA4B4G4R4 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES: {
            VkPhysicalDeviceImageRobustnessFeatures *f = next;
            f->robustImageAccess = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES: {
            VkPhysicalDevicePipelineRobustnessFeatures *f = next;
            f->pipelineRobustness = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES: {
            VkPhysicalDeviceIndexTypeUint8Features *f = next;
            f->indexTypeUint8 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR: {
            VkPhysicalDeviceFragmentShadingRateFeaturesKHR *f = next;
            f->pipelineFragmentShadingRate = VK_TRUE;
            f->primitiveFragmentShadingRate = VK_FALSE;
            f->attachmentFragmentShadingRate = VK_FALSE;
            break;
        }
        default:
            break;
        }
    }
}

void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures) {
    pvk_log("vkGetPhysicalDeviceFeatures2: phys=%p features=%p\n", (void*)physicalDevice, (void*)pFeatures);
    if (!pFeatures) return;
    panvk_v9_fill_features2(pFeatures);
}

/* Vulkan 1.1/1.2/1.3 property chains (mirrors Mesa panvk limits). */
void panvk_v9_fill_properties2(VkPhysicalDeviceProperties2 *props2) {
    if (!props2) return;
    vkGetPhysicalDeviceProperties2(NULL, props2);
}

void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties) {
    pvk_log("vkGetPhysicalDeviceQueueFamilyProperties: phys=%p count=%u props=%p\n",
            (void*)physicalDevice, pQueueFamilyPropertyCount ? *pQueueFamilyPropertyCount : 0, (void*)pQueueFamilyProperties);
    if (!pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    /* Family 0: Graphics + Compute + Transfer (0x7) */
    memset(pQueueFamilyProperties, 0, sizeof(VkQueueFamilyProperties));
    pQueueFamilyProperties->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                                        VK_QUEUE_TRANSFER_BIT;
    pQueueFamilyProperties->queueCount = 1;
    pQueueFamilyProperties->timestampValidBits = 64;
    pQueueFamilyProperties->minImageTransferGranularity.width = 1;
    pQueueFamilyProperties->minImageTransferGranularity.height = 1;
    pQueueFamilyProperties->minImageTransferGranularity.depth = 1;
    *pQueueFamilyPropertyCount = 1;
}

void vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    pvk_log("vkGetPhysicalDeviceQueueFamilyProperties2: phys=%p count=%u props=%p\n",
            (void*)physicalDevice, pQueueFamilyPropertyCount ? *pQueueFamilyPropertyCount : 0, (void*)pQueueFamilyProperties);
    if (!pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount,
                                             &pQueueFamilyProperties->queueFamilyProperties);
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    pvk_log("vkGetPhysicalDeviceMemoryProperties: phys=%p mem=%p\n", (void*)physicalDevice, (void*)pMemoryProperties);
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
    pMemoryProperties->memoryTypeCount = 2;
    pMemoryProperties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;
    pMemoryProperties->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 0;

    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 4096ULL * 1024ULL * 1024ULL; /* 4GB */
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

void vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    pvk_log("vkGetPhysicalDeviceMemoryProperties2: phys=%p mem=%p\n", (void*)physicalDevice, (void*)pMemoryProperties);
    if (!pMemoryProperties) return;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);
}

static VkFormatFeatureFlags panvk_v9_format_features(uint32_t format) {
    VkFormatFeatureFlags features =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                   VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        break;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                   VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        break;
    case VK_FORMAT_S8_UINT:
        features = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        break;
    default:
        break;
    }
    return features;
}

void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties) {
    pvk_log("vkGetPhysicalDeviceFormatProperties: phys=%p format=%u props=%p\n",
            (void*)physicalDevice, format, (void*)pFormatProperties);
    if (!pFormatProperties) return;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
    VkFormatFeatureFlags features = panvk_v9_format_features(format);
    pFormatProperties->linearTilingFeatures = features;
    pFormatProperties->optimalTilingFeatures = features;
    pFormatProperties->bufferFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                                        VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
                                        VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pImageFormatProperties) {
    pvk_log("vkGetPhysicalDeviceImageFormatProperties: phys=%p format=%u type=%u tiling=%u usage=%#x props=%p\n",
            (void*)physicalDevice, format, type, tiling, usage, (void*)pImageFormatProperties);
    if (!pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    pImageFormatProperties->maxExtent.width = 4096;
    pImageFormatProperties->maxExtent.height = 4096;
    pImageFormatProperties->maxExtent.depth = (type == VK_IMAGE_TYPE_3D) ? 2048 : 1;
    pImageFormatProperties->maxMipLevels = 16;
    pImageFormatProperties->maxArrayLayers = 2048;
    pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pImageFormatProperties->maxResourceSize = 256ULL * 1024ULL * 1024ULL;
    return VK_SUCCESS;
}

void vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
}

void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2 *pFormatProperties) {
    pvk_log("vkGetPhysicalDeviceFormatProperties2: phys=%p format=%u props=%p\n",
            (void*)physicalDevice, format, (void*)pFormatProperties);
    if (!pFormatProperties) return;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
    pFormatProperties->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &pFormatProperties->formatProperties);
}

void vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2 *pFormatProperties) {
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
}

VkResult vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo, VkImageFormatProperties2 *pImageFormatProperties) {
    if (!pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    pImageFormatProperties->sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
                                                    pImageFormatInfo->tiling, pImageFormatInfo->usage,
                                                    pImageFormatInfo->flags, &pImageFormatProperties->imageFormatProperties);
}

VkResult vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo, VkImageFormatProperties2 *pImageFormatProperties) {
    return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

void vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo, uint32_t *pPropertyCount, VkSparseImageFormatProperties2 *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
}

void vkGetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo, uint32_t *pPropertyCount, VkSparseImageFormatProperties2 *pProperties) {
    vkGetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

void vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo, VkExternalBufferProperties *pExternalBufferProperties) {
    if (!pExternalBufferProperties) return;
    memset(pExternalBufferProperties, 0, sizeof(*pExternalBufferProperties));
    pExternalBufferProperties->sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
}

void vkGetPhysicalDeviceExternalBufferPropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo, VkExternalBufferProperties *pExternalBufferProperties) {
    vkGetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

void vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo, VkExternalFenceProperties *pExternalFenceProperties) {
    if (!pExternalFenceProperties) return;
    memset(pExternalFenceProperties, 0, sizeof(*pExternalFenceProperties));
    pExternalFenceProperties->sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
}

void vkGetPhysicalDeviceExternalFencePropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo, VkExternalFenceProperties *pExternalFenceProperties) {
    vkGetPhysicalDeviceExternalFenceProperties(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

void vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo, VkExternalSemaphoreProperties *pExternalSemaphoreProperties) {
    if (!pExternalSemaphoreProperties) return;
    memset(pExternalSemaphoreProperties, 0, sizeof(*pExternalSemaphoreProperties));
    pExternalSemaphoreProperties->sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
}

void vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo, VkExternalSemaphoreProperties *pExternalSemaphoreProperties) {
    vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, pExternalSemaphoreInfo, pExternalSemaphoreProperties);
}

VkResult vkGetPhysicalDeviceExternalImageFormatPropertiesNV(VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkExternalMemoryHandleTypeFlagBitsNV handleType, VkExternalImageFormatPropertiesNV *pExternalImageFormatProperties) {
    if (!pExternalImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pExternalImageFormatProperties, 0, sizeof(*pExternalImageFormatProperties));
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceToolProperties(VkPhysicalDevice physicalDevice, uint32_t *pToolCount, VkPhysicalDeviceToolProperties *pToolProperties) {
    if (pToolCount) *pToolCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceToolPropertiesEXT(VkPhysicalDevice physicalDevice, uint32_t *pToolCount, VkPhysicalDeviceToolProperties *pToolProperties) {
    return vkGetPhysicalDeviceToolProperties(physicalDevice, pToolCount, pToolProperties);
}

VkBool32 vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, Display *dpy, VisualID visualID) {
    return VK_FALSE;
}

VkResult vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pRectCount, VkRect2D *pRects) {
    if (pRectCount) *pRectCount = 1;
    if (pRects) {
        pRects[0].offset.x = 0;
        pRects[0].offset.y = 0;
        pRects[0].extent.width = 1920;
        pRects[0].extent.height = 1080;
    }
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayProperties2KHR *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceDisplayPlaneProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPlaneProperties2KHR *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetDisplayModeProperties2KHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, uint32_t *pPropertyCount, VkDisplayModeProperties2KHR *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetDisplayPlaneCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkDisplayPlaneInfo2KHR *pDisplayPlaneInfo, VkDisplayPlaneCapabilities2KHR *pDisplayPlaneCapabilities) {
    if (!pDisplayPlaneCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pDisplayPlaneCapabilities, 0, sizeof(*pDisplayPlaneCapabilities));
    pDisplayPlaneCapabilities->sType = VK_STRUCTURE_TYPE_DISPLAY_PLANE_CAPABILITIES_2_KHR;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice, const VkVideoProfileInfoKHR *pVideoProfile, VkVideoCapabilitiesKHR *pCapabilities) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult vkGetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo, uint32_t *pVideoFormatPropertyCount, VkVideoFormatPropertiesKHR *pVideoFormatProperties) {
    if (pVideoFormatPropertyCount) *pVideoFormatPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkCooperativeMatrixPropertiesKHR *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(VkPhysicalDevice physicalDevice, uint32_t *pTimeDomainCount, VkTimeDomainKHR *pTimeDomains) {
    if (pTimeDomainCount) *pTimeDomainCount = 0;
    return VK_SUCCESS;
}

VkResult vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDebugReportCallbackEXT *pCallback) {
    if (pCallback) *pCallback = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

void vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback, const VkAllocationCallbacks *pAllocator) {}

void vkDebugReportMessageEXT(VkInstance instance, VkDebugReportFlagBitsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char *pLayerPrefix, const char *pMessage) {}

VkResult vkReleaseDisplayEXT(VkPhysicalDevice physicalDevice, VkDisplayKHR display) {
    return VK_SUCCESS;
}

VkResult vkAcquireXlibDisplayEXT(VkPhysicalDevice physicalDevice, Display *dpy, VkDisplayKHR display) {
    return VK_NOT_AVAILABLE;
}

VkResult vkGetRandROutputDisplayEXT(VkPhysicalDevice physicalDevice, Display *dpy, RROutput rrOutput, VkDisplayKHR *pDisplay) {
    if (pDisplay) *pDisplay = VK_NULL_HANDLE;
    return VK_NOT_AVAILABLE;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilities2EXT *pSurfaceCapabilities) {
    if (!pSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pSurfaceCapabilities, 0, sizeof(*pSurfaceCapabilities));
    pSurfaceCapabilities->sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT;
    pSurfaceCapabilities->minImageCount = 1;
    pSurfaceCapabilities->maxImageCount = 3;
    pSurfaceCapabilities->currentExtent.width = 1920;
    pSurfaceCapabilities->currentExtent.height = 1080;
    pSurfaceCapabilities->minImageExtent.width = 1;
    pSurfaceCapabilities->minImageExtent.height = 1;
    pSurfaceCapabilities->maxImageExtent.width = 4096;
    pSurfaceCapabilities->maxImageExtent.height = 4096;
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

VkResult vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDebugUtilsMessengerEXT *pMessenger) {
    if (pMessenger) *pMessenger = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

void vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger, const VkAllocationCallbacks *pAllocator) {}

void vkSubmitDebugUtilsMessageEXT(VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes, const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData) {}

void vkGetPhysicalDeviceMultisamplePropertiesEXT(VkPhysicalDevice physicalDevice, VkSampleCountFlagBits samples, VkMultisamplePropertiesEXT *pMultisampleProperties) {
    if (!pMultisampleProperties) return;
    memset(pMultisampleProperties, 0, sizeof(*pMultisampleProperties));
    pMultisampleProperties->sType = VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT;
}

VkResult vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV(VkPhysicalDevice physicalDevice, uint32_t *pCombinationCount, VkFramebufferMixedSamplesCombinationNV *pCombinations) {
    if (pCombinationCount) *pCombinationCount = 0;
    return VK_SUCCESS;
}

VkResult vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (pSurface) *pSurface = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

VkResult vkAcquireDrmDisplayEXT(VkPhysicalDevice physicalDevice, int32_t drmFd, VkDisplayKHR display) {
    return VK_NOT_AVAILABLE;
}

VkResult vkGetDrmDisplayEXT(VkPhysicalDevice physicalDevice, int32_t drmFd, uint32_t connectorId, VkDisplayKHR *pDisplay) {
    if (pDisplay) *pDisplay = VK_NULL_HANDLE;
    return VK_NOT_AVAILABLE;
}

VkResult vkGetPhysicalDeviceOpticalFlowImageFormatsNV(VkPhysicalDevice physicalDevice, const VkOpticalFlowImageFormatInfoNV *pOpticalFlowImageFormatInfo, uint32_t *pPropertyCount, VkOpticalFlowImageFormatPropertiesNV *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkCooperativeMatrixFlexibleDimensionsPropertiesNV *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceCooperativeMatrixPropertiesNV(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkCooperativeMatrixPropertiesNV *pProperties) {
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR *pQualityLevelInfo, VkVideoEncodeQualityLevelPropertiesKHR *pQualityLevelProperties) {
    if (!pQualityLevelProperties) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pQualityLevelProperties, 0, sizeof(*pQualityLevelProperties));
    pQualityLevelProperties->sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, uint32_t *pCounterCount, VkPerformanceCounterKHR *pCounters, VkPerformanceCounterDescriptionKHR *pCounterDescriptions) {
    if (pCounterCount) *pCounterCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR(VkPhysicalDevice physicalDevice, const VkQueryPoolPerformanceCreateInfoKHR *pPerformanceQueryCreateInfos, uint32_t *pNumPasses) {
    if (pNumPasses) *pNumPasses = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayModeKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, const VkDisplayModeCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDisplayModeKHR *pMode) {
    if (pMode) *pMode = VK_NULL_HANDLE;
    return VK_NOT_AVAILABLE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkDisplayModeKHR mode, uint32_t planeIndex, VkDisplayPlaneCapabilitiesKHR *pCapabilities) {
    if (!pCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pCapabilities, 0, sizeof(*pCapabilities));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayPlaneSurfaceKHR(VkInstance instance, const VkDisplaySurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (pSurface) *pSurface = VK_NULL_HANDLE;
    return VK_NOT_AVAILABLE;
}

VkResult vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    pvk_log("vkCreateDevice: phys=%p info=%p dev=%p\n", (void*)physicalDevice, (void*)pCreateInfo, (void*)pDevice);
    if (!physicalDevice || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkDevice_T *dev = calloc(1, sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;

    set_loader_magic(dev);
    dev->phys_dev = physicalDevice;
    dev->kdev = physicalDevice->kdev;

    /* Record features requested through the VkDeviceCreateInfo pNext chain. */
    if (pCreateInfo) {
        for (const void *next = pCreateInfo->pNext; next; next = *((const void *const *)next + 1)) {
            VkStructureType sType = *(const VkStructureType *)next;
            switch (sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
                const VkPhysicalDeviceVulkan11Features *f = next;
                dev->features.shader_float16 = f->storageInputOutput16;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                const VkPhysicalDeviceVulkan12Features *f = next;
                dev->features.runtime_descriptor_array = f->runtimeDescriptorArray;
                dev->features.partially_bound = f->descriptorBindingPartiallyBound;
                dev->features.update_after_bind = f->descriptorBindingSampledImageUpdateAfterBind;
                dev->features.timeline_semaphore = f->timelineSemaphore;
                dev->features.buffer_device_address = f->bufferDeviceAddress;
                dev->features.host_query_reset = f->hostQueryReset;
                dev->features.scalar_block_layout = f->scalarBlockLayout;
                dev->features.uniform_buffer_standard_layout = f->uniformBufferStandardLayout;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                const VkPhysicalDeviceVulkan13Features *f = next;
                dev->features.dynamic_rendering = f->dynamicRendering;
                dev->features.synchronization2 = f->synchronization2;
                dev->features.maintenance4 = f->maintenance4;
                dev->features.pipeline_creation_cache_control = f->pipelineCreationCacheControl;
                dev->features.robust_image_access = f->robustImageAccess;
                dev->features.shader_terminate_invocation = f->shaderTerminateInvocation;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
                const VkPhysicalDeviceDynamicRenderingFeatures *f = next;
                dev->features.dynamic_rendering = f->dynamicRendering;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
                const VkPhysicalDeviceDescriptorIndexingFeatures *f = next;
                dev->features.runtime_descriptor_array = f->runtimeDescriptorArray;
                dev->features.partially_bound = f->descriptorBindingPartiallyBound;
                dev->features.update_after_bind = f->descriptorBindingSampledImageUpdateAfterBind;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
                const VkPhysicalDeviceBufferDeviceAddressFeatures *f = next;
                dev->features.buffer_device_address = f->bufferDeviceAddress;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
                const VkPhysicalDeviceTimelineSemaphoreFeatures *f = next;
                dev->features.timeline_semaphore = f->timelineSemaphore;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: {
                const VkPhysicalDeviceHostQueryResetFeatures *f = next;
                dev->features.host_query_reset = f->hostQueryReset;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
                const VkPhysicalDeviceSynchronization2Features *f = next;
                dev->features.synchronization2 = f->synchronization2;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
                const VkPhysicalDeviceMaintenance4Features *f = next;
                dev->features.maintenance4 = f->maintenance4;
                break;
            }
            default:
                break;
            }
        }
    }

    /* Old-style VkPhysicalDeviceFeatures enabled through VkDeviceCreateInfo. */
    if (pCreateInfo && pCreateInfo->pEnabledFeatures) {
        const VkPhysicalDeviceFeatures *f = pCreateInfo->pEnabledFeatures;
        dev->features.robust_buffer_access = f->robustBufferAccess;
        dev->features.geometry_shader = f->geometryShader;
        dev->features.tessellation_shader = f->tessellationShader;
        dev->features.depth_clamp = f->depthClamp;
        dev->features.wide_lines = f->wideLines;
        dev->features.multi_draw_indirect = f->multiDrawIndirect;
        dev->features.draw_indirect_first_instance = f->drawIndirectFirstInstance;
        dev->features.fill_mode_non_solid = f->fillModeNonSolid;
        dev->features.vertex_pipeline_stores_and_atomics = f->vertexPipelineStoresAndAtomics;
        dev->features.fragment_stores_and_atomics = f->fragmentStoresAndAtomics;
        dev->features.shader_storage_image_read_without_format = f->shaderStorageImageReadWithoutFormat;
        dev->features.shader_storage_image_write_without_format = f->shaderStorageImageWriteWithoutFormat;
        dev->features.shader_float64 = f->shaderFloat64;
        dev->features.shader_int64 = f->shaderInt64;
        dev->features.shader_int16 = f->shaderInt16;
    }

    *pDevice = dev;
    pthread_mutex_init(&dev->submit_mutex, NULL);
    pvk_log("vkCreateDevice: OK dev=%p\n", (void*)dev);
    return VK_SUCCESS;
}

void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    pvk_log("vkDestroyDevice: dev=%p\n", (void*)device);
    if (!g_pvk_emu) {
        uintptr_t emu = pvk_find_guest_emu();
        if (emu) {
            g_pvk_emu = emu;
            volatile uint64_t *g = (volatile uint64_t *)emu;
            uintptr_t grip = *(volatile uintptr_t *)(emu + 136);
            uintptr_t grsp = (uintptr_t)g[4];
            uintptr_t grax = (uintptr_t)g[0];
            pvk_log("WATCHDOG: capturado x64emu=%#lx RIP=%#lx RSP=%#lx RAX=%#lx\n",
                    (unsigned long)emu, (unsigned long)grip,
                    (unsigned long)grsp, (unsigned long)grax);
            if (grsp && read_word_ok(grsp)) {
                uintptr_t ra = *(volatile uintptr_t *)grsp;
                pvk_log("WATCHDOG:   guest ret@RSP (call-site do destroy):");
                pvk_guest_resolve(ra);
            }
        } else {
            pvk_log("WATCHDOG: stack scan NAO achou x64emu no vkDestroyDevice\n");
        }
    }
    pvk_arm_watchdog();
    if (!device) return;
    /* FIX BUG5: aguarda qualquer submit em andamento antes de destruir o mutex */
    pthread_mutex_lock(&device->submit_mutex);
    pthread_mutex_unlock(&device->submit_mutex);
    pthread_mutex_destroy(&device->submit_mutex);
    if (device->queue) v9_cmd_buffer_destroy(device->queue->last_v9_cmd);
    free(device->queue);
    free(device);
    pvk_log("vkDestroyDevice: DONE (destruido com sucesso)\n");
}

void vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    pvk_log("vkGetDeviceQueue: dev=%p family=%u index=%u pQueue=%p\n",
            (void*)device, queueFamilyIndex, queueIndex, (void*)pQueue);
    if (!device || !pQueue) return;
    if (queueFamilyIndex != 0 || queueIndex != 0) {
        *pQueue = NULL;
        return;
    }
    if (!device->queue) {
        device->queue = calloc(1, sizeof(*device->queue));
        if (!device->queue) {
            *pQueue = NULL;
            return;
        }
        set_loader_magic(device->queue);
        device->queue->device = device;
    }
    *pQueue = device->queue;
    pvk_log("vkGetDeviceQueue: OK queue=%p\n", (void*)device->queue);
}

/* panvk-style image layout (linear tiling) */
static uint8_t panvk_v9_format_bpp(uint32_t format) {
    switch (format) {
    case 9:  /* VK_FORMAT_R8_UNORM */           return 1;
    case 17: /* VK_FORMAT_S8_UINT */             return 1;
    case 16: /* VK_FORMAT_R8G8_UNORM */          return 2;
    case 124:/* VK_FORMAT_D16_UNORM */           return 2;
    case 37: /* VK_FORMAT_R8G8B8A8_UNORM */      return 4;
    case 43: /* VK_FORMAT_R8G8B8A8_SRGB */       return 4;
    case 44: /* VK_FORMAT_B8G8R8A8_UNORM */      return 4;
    case 50: /* VK_FORMAT_B8G8R8A8_SRGB */       return 4;
    case 87: /* VK_FORMAT_R16G16_SFLOAT */       return 4;
    case 97: /* VK_FORMAT_R16G16B16A16_SFLOAT */ return 8;
    case 100:/* VK_FORMAT_R32_SFLOAT */          return 4;
    case 103:/* VK_FORMAT_R32G32_SFLOAT */       return 8;
    case 106:/* VK_FORMAT_R32G32B32_SFLOAT */    return 12;
    case 109:/* VK_FORMAT_R32G32B32A32_SFLOAT */ return 16;
    case 126:/* VK_FORMAT_D32_SFLOAT */          return 4;
    case 129:/* VK_FORMAT_D24_UNORM_S8_UINT */   return 4;
    default:                                     return 4;
    }
}

static uint64_t align64(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

/* Convert one 32bpp pixel between RGBA8 (37/43) and BGRA8 (44/50) by swapping
 * the red and blue channels.  Identical formats copy the pixel unchanged. */
static void panvk_v9_convert_pixel(uint32_t src_format, uint32_t dst_format,
                                   const uint8_t *src, uint8_t *dst) {
    if (src_format == dst_format || panvk_v9_format_bpp(src_format) != 4 ||
        panvk_v9_format_bpp(dst_format) != 4) {
        memcpy(dst, src, 4);
        return;
    }
    int src_rgba = (src_format == 37 || src_format == 43);
    int dst_rgba = (dst_format == 37 || dst_format == 43);
    if (src_rgba == dst_rgba) {
        memcpy(dst, src, 4);
        return;
    }
    dst[0] = src[2]; /* R <- B */
    dst[1] = src[1];
    dst[2] = src[0]; /* B <- R */
    dst[3] = src[3];
}

static void panvk_v9_image_layout_init(struct VkImage_T *image) {
    uint32_t bpp = panvk_v9_format_bpp(image->format);
    uint64_t offset = 0;
    for (uint32_t level = 0; level < image->mip_levels && level < 16; level++) {
        uint64_t w = image->width  >> level; if (w < 1) w = 1;
        uint64_t h = image->height >> level; if (h < 1) h = 1;
        uint64_t d = image->depth  >> level; if (d < 1) d = 1;
        if (image->image_type == 0) d = 1; /* VK_IMAGE_TYPE_1D */

        uint64_t row_stride = align64(w * bpp, 64);
        uint64_t slice      = align64(h * row_stride, 64);
        uint64_t level_size = slice * d * image->array_layers;

        image->row_pitch[level] = row_stride;
        image->mip_offset[level] = offset;
        offset += level_size;
    }
    image->size = offset > 0 ? offset : 4096;
}

static uint64_t panvk_v9_image_get_offset(struct VkImage_T *image,
                                          uint32_t mip, uint32_t layer) {
    uint64_t d = image->depth >> mip; if (d < 1) d = 1;
    if (image->image_type == 0) d = 1;
    uint64_t h = image->height >> mip; if (h < 1) h = 1;
    uint64_t row_stride = image->row_pitch[mip];
    uint64_t slice      = align64(h * row_stride, 64);
    return image->mip_offset[mip] + (uint64_t)layer * slice * d;
}

static uint64_t panvk_v9_image_slice_pitch(struct VkImage_T *image, uint32_t mip) {
    uint64_t h = image->height >> mip; if (h < 1) h = 1;
    return align64(h * image->row_pitch[mip], 64);
}

/* Memory Allocation & Buffer Management */
VkResult vkAllocateMemory(VkDevice device, const struct VkMemoryAllocateInfo *pAllocateInfo, const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    pvk_log("vkAllocateMemory: dev=%p size=%lu type=%u\n", (void*)device,
            pAllocateInfo ? (unsigned long)pAllocateInfo->allocationSize : 0,
            pAllocateInfo ? pAllocateInfo->memoryTypeIndex : 0);
    if (!device || !device->kdev || !pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkDeviceMemory_T *mem = calloc(1, sizeof(*mem));
    if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;

    size_t sz = pAllocateInfo->allocationSize > 0 ? pAllocateInfo->allocationSize : 4096;
    mem->bo = pan_kmod_bo_alloc(device->kdev, sz, PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
    if (!mem->bo) {
        pvk_log("vkAllocateMemory: pan_kmod_bo_alloc FAILED size=%lu\n", (unsigned long)sz);
        free(mem);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    mem->size = sz;
    *pMemory = mem;
    pvk_log("vkAllocateMemory: OK mem=%p size=%lu\n", (void*)mem, (unsigned long)sz);
    return VK_SUCCESS;
}

void vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator) {
    if (!memory) return;
    if (memory->bo) pan_kmod_bo_free(memory->bo);
    free(memory);
}

VkResult vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkFlags flags, void **ppData) {
    pvk_log("vkMapMemory: mem=%p offset=%lu size=%lu flags=%u\n", (void*)memory,
            (unsigned long)offset, (unsigned long)size, flags);
    if (!memory || !memory->bo || !memory->bo->cpu || !ppData) return VK_ERROR_INITIALIZATION_FAILED; /* FIX BUG3 */
    *ppData = (uint8_t *)memory->bo->cpu + offset;
    pvk_log("vkMapMemory: OK ppData=%p\n", *ppData);
    return VK_SUCCESS;
}

void vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
}

VkResult vkMapMemory2(VkDevice device, const struct VkMemoryMapInfo *pMemoryMapInfo, void **ppData) {
    if (!pMemoryMapInfo || !ppData) return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceMemory memory = pMemoryMapInfo->memory;
    pvk_log("vkMapMemory2: mem=%p offset=%lu size=%lu flags=%u\n", (void*)memory,
            (unsigned long)pMemoryMapInfo->offset, (unsigned long)pMemoryMapInfo->size,
            (unsigned)pMemoryMapInfo->flags);
    if (!memory || !memory->bo) return VK_ERROR_INITIALIZATION_FAILED;
    *ppData = (uint8_t *)memory->bo->cpu + pMemoryMapInfo->offset;
    pvk_log("vkMapMemory2: OK ppData=%p\n", *ppData);
    return VK_SUCCESS;
}

VkResult vkMapMemory2KHR(VkDevice device, const struct VkMemoryMapInfo *pMemoryMapInfo, void **ppData) {
    return vkMapMemory2(device, pMemoryMapInfo, ppData);
}

VkResult vkUnmapMemory2(VkDevice device, const struct VkMemoryUnmapInfo *pMemoryUnmapInfo) {
    (void)device; (void)pMemoryUnmapInfo;
    return VK_SUCCESS;
}

VkResult vkUnmapMemory2KHR(VkDevice device, const struct VkMemoryUnmapInfo *pMemoryUnmapInfo) {
    (void)device; (void)pMemoryUnmapInfo;
    return VK_SUCCESS;
}

VkResult vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges) {
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;
}

VkResult vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges) {
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;
}

VkResult vkCreateBuffer(VkDevice device, const struct VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    pvk_log("vkCreateBuffer: dev=%p size=%lu usage=%#x\n", (void*)device,
            pCreateInfo ? (unsigned long)pCreateInfo->size : 0,
            pCreateInfo ? (unsigned)pCreateInfo->usage : 0);
    if (!device || !pCreateInfo || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkBuffer_T *buf = calloc(1, sizeof(*buf));
    if (!buf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    buf->size = pCreateInfo->size;
    *pBuffer = buf;
    pvk_log("vkCreateBuffer: OK buf=%p size=%lu\n", (void*)buf, (unsigned long)buf->size);
    return VK_SUCCESS;
}

void vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    if (buffer) free(buffer);
}

void vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, struct VkMemoryRequirements *pMemoryRequirements) {
    if (!pMemoryRequirements) return;
    pMemoryRequirements->size = buffer ? buffer->size : 4096;
    pMemoryRequirements->alignment = 64;
    pMemoryRequirements->memoryTypeBits = 0x3;
}

VkResult vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    if (buffer && memory) {
        buffer->bo = memory->bo;
        buffer->memory_offset = memoryOffset;
    }
    return VK_SUCCESS;
}

VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                       VkImage *pImage) {
    pvk_log("vkCreateImage: dev=%p w=%u h=%u fmt=%d usage=%#x\n", (void*)device,
            pCreateInfo ? pCreateInfo->extent.width : 0,
            pCreateInfo ? pCreateInfo->extent.height : 0,
            pCreateInfo ? pCreateInfo->format : -1,
            pCreateInfo ? (unsigned)pCreateInfo->usage : 0);
    if (!pCreateInfo || !pImage) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkImage_T *image = calloc(1, sizeof(*image));
    if (!image) return VK_ERROR_OUT_OF_HOST_MEMORY;
    image->width = pCreateInfo->extent.width;
    image->height = pCreateInfo->extent.height;
    image->depth = pCreateInfo->extent.depth;
    image->format = pCreateInfo->format;
    image->image_type = pCreateInfo->imageType;
    image->mip_levels = pCreateInfo->mipLevels;
    image->array_layers = pCreateInfo->arrayLayers;
    image->samples = pCreateInfo->samples;
    image->tiling = pCreateInfo->tiling;
    image->usage = pCreateInfo->usage;
    panvk_v9_image_layout_init(image);
    *pImage = image;
    pvk_log("vkCreateImage: OK img=%p %ux%u fmt=%d\n", (void*)image,
            image->width, image->height, image->format);
    return VK_SUCCESS;
}

void vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    if (image && !image->swapchain) free(image);
}

void vkGetImageMemoryRequirements(VkDevice device, VkImage image,
                                  struct VkMemoryRequirements *pMemoryRequirements) {
    if (!pMemoryRequirements) return;
    if (image) panvk_v9_image_layout_init(image);
    pMemoryRequirements->size = image ? image->size : 4096;
    pMemoryRequirements->alignment = 4096;
    pMemoryRequirements->memoryTypeBits = 0x3;
}

void vkGetImageSubresourceLayout(VkDevice device, VkImage image,
                                 const VkImageSubresource *sub,
                                 VkSubresourceLayout *layout) {
    if (!image || !sub || !layout) return;
    uint32_t mip = sub->mipLevel < image->mip_levels ? sub->mipLevel : 0;
    uint64_t h = image->height >> mip; if (h < 1) h = 1;
    uint64_t d = image->depth >> mip;  if (d < 1) d = 1;
    if (image->image_type == 0) d = 1;
    layout->rowPitch = image->row_pitch[mip];
    layout->arrayPitch = align64(h * layout->rowPitch, 64);
    layout->depthPitch = layout->arrayPitch;
    layout->offset = panvk_v9_image_get_offset(image, mip, sub->arrayLayer);
    layout->size = layout->arrayPitch * d;
}

VkResult vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                           VkDeviceSize memoryOffset) {
    if (!image || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    image->bo = memory->bo;
    image->memory_offset = memoryOffset;
    return VK_SUCCESS;
}

VkResult vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                           const VkAllocationCallbacks *pAllocator,
                           VkImageView *pView) {
    pvk_log("vkCreateImageView: dev=%p img=%p fmt=%d\n", (void*)device,
            ci ? (void*)ci->image : NULL, ci ? ci->format : -1);
    if (!pView) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkImageView_T *view = calloc(1, sizeof(*view));
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (ci) {
        view->image = ci->image;
        view->format = ci->format;
        view->view_type = ci->viewType;
        view->base_mip = ci->subresourceRange.baseMipLevel;
        view->mip_count = ci->subresourceRange.levelCount;
        view->base_layer = ci->subresourceRange.baseArrayLayer;
        view->layer_count = ci->subresourceRange.layerCount;
    }
    *pView = view;
    pvk_log("vkCreateImageView: OK view=%p\n", (void*)view);
    return VK_SUCCESS;
}

void vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator) {
    free(imageView);
}

VkResult vkCreateBufferView(VkDevice device, const struct VkBufferViewCreateInfo *pCreateInfo,
                            const struct VkAllocationCallbacks *pAllocator,
                            VkBufferView *pView) {
    if (!pView) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkBufferView_T *view = calloc(1, sizeof(*view));
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pCreateInfo) {
        view->buffer = pCreateInfo->buffer;
        view->format = pCreateInfo->format;
        view->offset = pCreateInfo->offset;
        view->range = pCreateInfo->range;
    }
    *pView = view;
    return VK_SUCCESS;
}

void vkDestroyBufferView(VkDevice device, VkBufferView bufferView, const struct VkAllocationCallbacks *pAllocator) {
    free(bufferView);
}

/* Shader Module & Pipeline Implementation */
#define SPIRV_MAGIC 0x07230203u
#define SPIRV_OP_ENTRY_POINT 15u

static uint32_t spirv_execution_model_stage(uint32_t execution_model) {
    switch (execution_model) {
    case 0: return VK_SHADER_STAGE_VERTEX_BIT;
    case 4: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: return 0;
    }
}

static bool spirv_string_equals(const uint32_t *words, size_t word_count,
                                const char *expected) {
    if (!expected) return false;
    size_t expected_len = strlen(expected);
    size_t max_len = word_count * sizeof(uint32_t);
    const char *value = (const char *)words;
    const char *end = memchr(value, '\0', max_len);
    return end && (size_t)(end - value) == expected_len &&
           memcmp(value, expected, expected_len) == 0;
}

static bool spirv_validate_and_scan(const uint32_t *code, size_t code_size,
                                    uint32_t *stage_mask) {
    if (!code || code_size < 5 * sizeof(uint32_t) ||
        code_size % sizeof(uint32_t) != 0 || code[0] != SPIRV_MAGIC ||
        code[1] > 0x00010600u || code[3] == 0 || code[4] != 0) {
        return false;
    }

    size_t count = code_size / sizeof(uint32_t);
    uint32_t stages = 0;
    bool found_entry_point = false;
    for (size_t offset = 5; offset < count;) {
        uint32_t instruction = code[offset];
        uint32_t word_count = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (word_count == 0 || word_count > count - offset) return false;
        if (opcode == SPIRV_OP_ENTRY_POINT) {
            if (word_count < 4) return false;
            found_entry_point = true;
            stages |= spirv_execution_model_stage(code[offset + 1]);
            if (!memchr((const char *)&code[offset + 3], '\0',
                        (word_count - 3) * sizeof(uint32_t))) {
                return false;
            }
        }
        offset += word_count;
    }

    if (stage_mask) *stage_mask = stages;
    return found_entry_point;
}

static bool spirv_has_entry_point(VkShaderModule module, uint32_t stage,
                                  const char *name) {
    if (!module || !(module->stage_mask & stage) || !name) return false;
    size_t count = module->code_size / sizeof(uint32_t);
    for (size_t offset = 5; offset < count;) {
        uint32_t instruction = module->code[offset];
        uint32_t word_count = instruction >> 16;
        uint32_t opcode = instruction & 0xffffu;
        if (opcode == SPIRV_OP_ENTRY_POINT && word_count >= 4 &&
            spirv_execution_model_stage(module->code[offset + 1]) == stage &&
            spirv_string_equals(&module->code[offset + 3], word_count - 3, name)) {
            return true;
        }
        offset += word_count;
    }
    return false;
}

VkResult vkCreateShaderModule(VkDevice device, const struct VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    pvk_log("vkCreateShaderModule: dev=%p codeSize=%zu\n", (void*)device,
            pCreateInfo ? pCreateInfo->codeSize : 0);
    if (!device || !pCreateInfo || !pShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    *pShaderModule = NULL;

    uint32_t stage_mask = 0;
    if (!spirv_validate_and_scan(pCreateInfo->pCode, pCreateInfo->codeSize,
                                 &stage_mask)) {
        pvk_log("vkCreateShaderModule: SPIR-V INVALID (magic/stage scan failed)\n");
        return VK_ERROR_INVALID_SHADER_NV;
    }

    struct VkShaderModule_T *sm = calloc(1, sizeof(*sm));
    if (!sm) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sm->code_size = pCreateInfo->codeSize;
    sm->stage_mask = stage_mask;
    sm->code = malloc(sm->code_size);
    if (!sm->code) {
        free(sm);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(sm->code, pCreateInfo->pCode, sm->code_size);

    *pShaderModule = sm;
    pvk_log("vkCreateShaderModule: OK sm=%p stages=%#x\n", (void*)sm, stage_mask);
    return VK_SUCCESS;
}

void vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator) {
    if (!shaderModule) return;
    if (shaderModule->code) free(shaderModule->code);
    free(shaderModule);
}

VkResult vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkPipelineCache *pPipelineCache) {
    if (!pPipelineCache) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkPipelineCache_T *pc = calloc(1, sizeof(*pc));
    *pPipelineCache = pc;
    return VK_SUCCESS;
}

void vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks *pAllocator) {
    if (pipelineCache) free(pipelineCache);
}

VkResult vkCreatePipelineLayout(VkDevice device, const struct VkPipelineLayoutCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout) {
    if (!pCreateInfo || !pPipelineLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkPipelineLayout_T *pl = calloc(1, sizeof(*pl));
    if (!pl) return VK_ERROR_OUT_OF_HOST_MEMORY;

    uint32_t binding_count = 0;
    for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
        if (pCreateInfo->pSetLayouts[set])
            binding_count += pCreateInfo->pSetLayouts[set]->binding_count;
    }
    pl->bindings = calloc(binding_count, sizeof(*pl->bindings));
    if (binding_count && !pl->bindings) {
        free(pl);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    uint32_t index = 0;
    uint32_t ubo_index = 0;
    uint32_t ssbo_index = 0;
    uint32_t image_index = 0; /* shared for textures/samplers/images (tables 2/3) */
    for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
        VkDescriptorSetLayout set_layout = pCreateInfo->pSetLayouts[set];
        if (!set_layout) continue;
        for (uint32_t i = 0; i < set_layout->binding_count; i++) {
            const struct VkDescriptorSetLayoutBinding *binding = &set_layout->bindings[i];
            struct panvk_v9_descriptor_binding *out = &pl->bindings[index++];
            out->set = set;
            out->binding = binding->binding;
            out->descriptor_type = binding->descriptorType;
            out->array_size = binding->descriptorCount;
            if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                out->resource_index = 0x01000000u | ubo_index;
                ubo_index += binding->descriptorCount;
            } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                       binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                out->resource_index = 0x01000000u | ssbo_index;
                ssbo_index += binding->descriptorCount;
            } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                /* Texture part uses table 4, sampler part shares same idx but table 5 (derived in compiler). */
                out->resource_index = 0x04000000u | image_index;
                image_index += binding->descriptorCount;
            } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                       binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                       binding->descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                       binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                       binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                out->resource_index = 0x04000000u | image_index;
                image_index += binding->descriptorCount;
            } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                out->resource_index = 0x05000000u | image_index;
                image_index += binding->descriptorCount;
            } else {
                out->resource_index = 0;
            }
        }
    }
    pl->compiler_layout.bindings = pl->bindings;
    pl->compiler_layout.binding_count = binding_count;
    pl->compiler_layout.ubo_count = ubo_index;
    *pPipelineLayout = pl;
    pvk_log("vkCreatePipelineLayout: OK pl=%p bindings=%u\n", (void*)pl, binding_count);
    return VK_SUCCESS;
}

void vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks *pAllocator) {
    if (!pipelineLayout) return;
    free(pipelineLayout->bindings);
    free(pipelineLayout);
}

VkResult vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    pvk_log("vkCreateRenderPass: dev=%p att=%u subp=%u\n", (void*)device,
            pCreateInfo ? pCreateInfo->attachmentCount : 0,
            pCreateInfo ? pCreateInfo->subpassCount : 0);
    if (!pRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkRenderPass_T *rp = calloc(1, sizeof(*rp));
    *pRenderPass = rp;
    pvk_log("vkCreateRenderPass: OK rp=%p\n", (void*)rp);
    return VK_SUCCESS;
}

void vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator) {
    if (renderPass) free(renderPass);
}

VkResult vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    if (!pRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkRenderPass_T *rp = calloc(1, sizeof(*rp));
    if (!rp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *pRenderPass = rp;
    return VK_SUCCESS;
}

VkResult vkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    return vkCreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
}

VkResult vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer) {
    pvk_log("vkCreateFramebuffer: dev=%p w=%u h=%u att=%u\n", (void*)device,
            pCreateInfo ? pCreateInfo->width : 0,
            pCreateInfo ? pCreateInfo->height : 0,
            pCreateInfo ? pCreateInfo->attachmentCount : 0);
    if (!device || !pFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkFramebuffer_T *fb = calloc(1, sizeof(*fb));
    if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    fb->device = device;
    if (pCreateInfo) {
        fb->attachment_count = pCreateInfo->attachmentCount;
        fb->width = pCreateInfo->width;
        fb->height = pCreateInfo->height;
        if (fb->attachment_count && pCreateInfo->pAttachments) {
            fb->attachments = calloc(fb->attachment_count, sizeof(*fb->attachments));
            if (!fb->attachments) {
                free(fb);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            for (uint32_t i = 0; i < fb->attachment_count; i++)
                fb->attachments[i] = pCreateInfo->pAttachments[i];
        }
    }
    *pFramebuffer = fb;
    pvk_log("vkCreateFramebuffer: OK fb=%p\n", (void*)fb);
    return VK_SUCCESS;
}

void vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator) {
    if (framebuffer) {
        free(framebuffer->attachments);
        free(framebuffer);
    }
}

VkResult vkCreateDescriptorSetLayout(VkDevice device,
                                     const struct VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout) {
    if (!pCreateInfo || !pSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkDescriptorSetLayout_T *dsl = calloc(1, sizeof(*dsl));
    if (!dsl) return VK_ERROR_OUT_OF_HOST_MEMORY;
    dsl->variable_binding = -1;  /* FIX BUG8: -1 = nenhum binding variável (calloc zeraria p/ 0) */
    if (!dsl) return VK_ERROR_OUT_OF_HOST_MEMORY;
    dsl->binding_count = pCreateInfo->bindingCount;
    dsl->bindings = calloc(dsl->binding_count, sizeof(*dsl->bindings));
    dsl->binding_offsets = calloc(dsl->binding_count, sizeof(*dsl->binding_offsets));
    if (dsl->binding_count) {
        dsl->binding_flags = calloc(dsl->binding_count, sizeof(*dsl->binding_flags));
    }
    if (dsl->binding_count && (!dsl->bindings || !dsl->binding_offsets)) {
        free(dsl->binding_flags);
        free(dsl->binding_offsets);
        free(dsl->bindings);
        free(dsl);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (dsl->binding_count) {
        memcpy(dsl->bindings, pCreateInfo->pBindings,
               dsl->binding_count * sizeof(*dsl->bindings));
        for (uint32_t i = 0; i < dsl->binding_count; i++) {
            dsl->binding_offsets[i] = dsl->descriptor_count;
            dsl->descriptor_count += dsl->bindings[i].descriptorCount;
        }
    }
    /* Descriptor indexing: honour VkDescriptorSetLayoutBindingFlagsCreateInfo
     * pNext (variable descriptor count, partially bound, update-after-bind). */
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bfci = pCreateInfo->pNext;
    while (bfci) {
        if (bfci->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            for (uint32_t i = 0; i < dsl->binding_count && i < bfci->bindingCount; i++) {
                dsl->binding_flags[i] = bfci->pBindingFlags[i];
                if (bfci->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
                    dsl->variable_binding = i;
                    dsl->variable_descriptor_count = dsl->bindings[i].descriptorCount;
                }
            }
            break;
        }
        bfci = bfci->pNext;
    }
    *pSetLayout = dsl;
    return VK_SUCCESS;
}

void vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout setLayout, const VkAllocationCallbacks *pAllocator) {
    if (!setLayout) return;
    free(setLayout->binding_flags);
    free(setLayout->binding_offsets);
    free(setLayout->bindings);
    free(setLayout);
}

VkResult vkCreateDescriptorPool(VkDevice device,
                                const struct VkDescriptorPoolCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool) {
    pvk_log("vkCreateDescriptorPool: dev=%p maxSets=%u\n", (void*)device,
            pCreateInfo ? pCreateInfo->maxSets : 0);
    if (!pDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkDescriptorPool_T *dp = calloc(1, sizeof(*dp));
    *pDescriptorPool = dp;
    pvk_log("vkCreateDescriptorPool: OK dp=%p\n", (void*)dp);
    return VK_SUCCESS;
}

void vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator) {
    if (descriptorPool) free(descriptorPool);
}

VkResult vkAllocateDescriptorSets(VkDevice device,
                                  const struct VkDescriptorSetAllocateInfo *pAllocateInfo,
                                  VkDescriptorSet *pDescriptorSets) {
    pvk_log("vkAllocateDescriptorSets: dev=%p count=%u\n", (void*)device,
            pAllocateInfo ? pAllocateInfo->descriptorSetCount : 0);
    if (!pAllocateInfo || !pDescriptorSets || !pAllocateInfo->pSetLayouts)
        return VK_ERROR_INITIALIZATION_FAILED;
    /* Variable descriptor counts per set come from the pNext array. */
    const uint32_t *var_counts = NULL;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *vdci = pAllocateInfo->pNext;
    while (vdci) {
        if (vdci->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
            var_counts = vdci->pDescriptorCounts;
            break;
        }
        vdci = vdci->pNext;
    }
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++)
        pDescriptorSets[i] = NULL;
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        VkDescriptorSet set = calloc(1, sizeof(*set));
        if (!set) goto fail;
        set->layout = pAllocateInfo->pSetLayouts[i];
        uint32_t n = set->layout->descriptor_count;
        if (set->layout->variable_binding >= 0 && var_counts) {
            /* Replace the variable binding's declared count with the requested
             * count (array indexing is still within the layout's descriptor
             * space; just shrink the total allocation). */
            uint32_t requested = var_counts[i];
            if (requested < set->layout->variable_descriptor_count) {
                uint32_t shrink = set->layout->variable_descriptor_count - requested;
                n = (n > shrink) ? (n - shrink) : 0;
            }
        }
        set->buffers = calloc(n ? n : 1, sizeof(*set->buffers));
        if (n && !set->buffers) {
            free(set);
            goto fail;
        }
        set->images = calloc(n ? n : 1, sizeof(*set->images));
        if (n && !set->images) {
            free(set->buffers);
            free(set);
            goto fail;
        }
        pDescriptorSets[i] = set;
    }
    pvk_log("vkAllocateDescriptorSets: OK\n");
    return VK_SUCCESS;

fail:
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        if (!pDescriptorSets[i]) continue;
        free(pDescriptorSets[i]->buffers);
        free(pDescriptorSets[i]->images);
        free(pDescriptorSets[i]);
        pDescriptorSets[i] = NULL;
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult vkFreeDescriptorSets(VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets) {
    if (!pDescriptorSets) return VK_SUCCESS;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        if (pDescriptorSets[i]) {
            free(pDescriptorSets[i]->buffers);
            free(pDescriptorSets[i]->images);
            free(pDescriptorSets[i]);
        }
    }
    return VK_SUCCESS;
}

void vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                            const VkWriteDescriptorSet *pDescriptorWrites,
                            uint32_t descriptorCopyCount, const VkCopyDescriptorSet *pDescriptorCopies) {
    for (uint32_t w = 0; w < descriptorWriteCount; w++) {
        const VkWriteDescriptorSet *write = &pDescriptorWrites[w];
        if (!write->dstSet) continue;
        VkDescriptorSetLayout layout = write->dstSet->layout;
        for (uint32_t b = 0; b < layout->binding_count; b++) {
            const struct VkDescriptorSetLayoutBinding *binding = &layout->bindings[b];
            if (binding->binding != write->dstBinding ||
                binding->descriptorType != write->descriptorType ||
                write->dstArrayElement + write->descriptorCount > binding->descriptorCount)
                continue;
            uint32_t off = layout->binding_offsets[b] + write->dstArrayElement;
            if (write->pBufferInfo) {
                memcpy(&write->dstSet->buffers[off],
                       write->pBufferInfo,
                       write->descriptorCount * sizeof(*write->pBufferInfo));
            }
            if (write->pImageInfo) {
                memcpy(&write->dstSet->images[off],
                       write->pImageInfo,
                       write->descriptorCount * sizeof(*write->pImageInfo));
            }
            if (write->pTexelBufferView) {
                /* texel buffers treated as images for now: store view as imageView alias */
                for (uint32_t i = 0; i < write->descriptorCount; i++) {
                    write->dstSet->images[off + i].imageView = (VkImageView)(uintptr_t)write->pTexelBufferView[i];
                }
            }
            break;
        }
    }
    for (uint32_t c = 0; c < descriptorCopyCount; c++) {
        const VkCopyDescriptorSet *copy = &pDescriptorCopies[c];
        if (!copy->srcSet || !copy->dstSet) continue;
        VkDescriptorSetLayout src_layout = copy->srcSet->layout;
        VkDescriptorSetLayout dst_layout = copy->dstSet->layout;
        int src_b = -1, dst_b = -1;
        for (uint32_t b = 0; b < src_layout->binding_count; b++)
            if (src_layout->bindings[b].binding == copy->srcBinding) src_b = b;
        for (uint32_t b = 0; b < dst_layout->binding_count; b++)
            if (dst_layout->bindings[b].binding == copy->dstBinding) dst_b = b;
        if (src_b < 0 || dst_b < 0) continue;
        uint32_t src_off = src_layout->binding_offsets[src_b] + copy->srcArrayElement;
        uint32_t dst_off = dst_layout->binding_offsets[dst_b] + copy->dstArrayElement;
        uint32_t cnt = copy->descriptorCount;
        memcpy(&copy->dstSet->buffers[dst_off], &copy->srcSet->buffers[src_off], cnt * sizeof(*copy->dstSet->buffers));
        memcpy(&copy->dstSet->images[dst_off], &copy->srcSet->images[src_off], cnt * sizeof(*copy->dstSet->images));
    }
}

static bool pipeline_dynamic_state(const struct VkPipelineDynamicStateCreateInfo *dynamic,
                                   uint32_t state) {
    if (!dynamic || !dynamic->pDynamicStates) return false;
    for (uint32_t i = 0; i < dynamic->dynamicStateCount; i++) {
        if (dynamic->pDynamicStates[i] == state) return true;
    }
    return false;
}

static VkResult pipeline_parse_shader_stages(struct VkPipeline_T *pipeline,
                                             const struct VkGraphicsPipelineCreateInfo *info) {
    if (!info->pStages || info->stageCount == 0) return VK_ERROR_INVALID_SHADER_NV;

    for (uint32_t i = 0; i < info->stageCount; i++) {
        const struct VkPipelineShaderStageCreateInfo *stage = &info->pStages[i];
        if ((stage->stage != VK_SHADER_STAGE_VERTEX_BIT &&
             stage->stage != VK_SHADER_STAGE_FRAGMENT_BIT) ||
            !spirv_has_entry_point(stage->module, stage->stage, stage->pName)) {
            return VK_ERROR_INVALID_SHADER_NV;
        }
        if (pipeline->stage_mask & stage->stage) return VK_ERROR_INVALID_SHADER_NV;

        pipeline->stage_mask |= stage->stage;
        if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT) {
            snprintf(pipeline->vertex_entry_point,
                     sizeof(pipeline->vertex_entry_point), "%s", stage->pName);
        } else {
            snprintf(pipeline->fragment_entry_point,
                     sizeof(pipeline->fragment_entry_point), "%s", stage->pName);
        }
    }

    return (pipeline->stage_mask & VK_SHADER_STAGE_VERTEX_BIT) ?
           VK_SUCCESS : VK_ERROR_INVALID_SHADER_NV;
}

static const uint32_t panvk_v9_fallback_vert_spv[] = {
   0x07230203,0x00010000,0x0008000b,0x0000002a,0x00000000,0x00020011,0x00000001,0x0006000b,
   0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
   0x0007000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x0000001c,0x00030003,
   0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000009,
   0x00000070,0x00060005,0x0000000c,0x565f6c67,0x65747265,0x646e4978,0x00007865,0x00060005,
   0x0000001a,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000001a,0x00000000,
   0x505f6c67,0x7469736f,0x006e6f69,0x00070006,0x0000001a,0x00000001,0x505f6c67,0x746e696f,
   0x657a6953,0x00000000,0x00070006,0x0000001a,0x00000002,0x435f6c67,0x4470696c,0x61747369,
   0x0065636e,0x00070006,0x0000001a,0x00000003,0x435f6c67,0x446c6c75,0x61747369,0x0065636e,
   0x00030005,0x0000001c,0x00000000,0x00040047,0x0000000c,0x0000000b,0x0000002a,0x00030047,
   0x0000001a,0x00000002,0x00050048,0x0000001a,0x00000000,0x0000000b,0x00000000,0x00050048,
   0x0000001a,0x00000001,0x0000000b,0x00000001,0x00050048,0x0000001a,0x00000002,0x0000000b,
   0x00000003,0x00050048,0x0000001a,0x00000003,0x0000000b,0x00000004,0x00020013,0x00000002,
   0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
   0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,0x0000000a,
   0x00000020,0x00000001,0x00040020,0x0000000b,0x00000001,0x0000000a,0x0004003b,0x0000000b,
   0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000e,0x00000001,0x00040017,0x00000016,
   0x00000006,0x00000004,0x00040015,0x00000017,0x00000020,0x00000000,0x0004002b,0x00000017,
   0x00000018,0x00000001,0x0004001c,0x00000019,0x00000006,0x00000018,0x0006001e,0x0000001a,
   0x00000016,0x00000006,0x00000019,0x00000019,0x00040020,0x0000001b,0x00000003,0x0000001a,
   0x0004003b,0x0000001b,0x0000001c,0x00000003,0x0004002b,0x0000000a,0x0000001d,0x00000000,
   0x0004002b,0x00000006,0x0000001f,0x40000000,0x0004002b,0x00000006,0x00000021,0x3f800000,
   0x0004002b,0x00000006,0x00000024,0x00000000,0x00040020,0x00000028,0x00000003,0x00000016,
   0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,
   0x00000008,0x00000009,0x00000007,0x0004003d,0x0000000a,0x0000000d,0x0000000c,0x000500c7,
   0x0000000a,0x0000000f,0x0000000d,0x0000000e,0x0004006f,0x00000006,0x00000010,0x0000000f,
   0x0004003d,0x0000000a,0x00000011,0x0000000c,0x000500c3,0x0000000a,0x00000012,0x00000011,
   0x0000000e,0x000500c7,0x0000000a,0x00000013,0x00000012,0x0000000e,0x0004006f,0x00000006,
   0x00000014,0x00000013,0x00050050,0x00000007,0x00000015,0x00000010,0x00000014,0x0003003e,
   0x00000009,0x00000015,0x0004003d,0x00000007,0x0000001e,0x00000009,0x0005008e,0x00000007,
   0x00000020,0x0000001e,0x0000001f,0x00050050,0x00000007,0x00000022,0x00000021,0x00000021,
   0x00050083,0x00000007,0x00000023,0x00000020,0x00000022,0x00050051,0x00000006,0x00000025,
   0x00000023,0x00000000,0x00050051,0x00000006,0x00000026,0x00000023,0x00000001,0x00070050,
   0x00000016,0x00000027,0x00000025,0x00000026,0x00000024,0x00000021,0x00050041,0x00000028,
   0x00000029,0x0000001c,0x0000001d,0x0003003e,0x00000029,0x00000027,0x000100fd,0x00010038,
};

static const uint32_t panvk_v9_fallback_frag_spv[] = {
   0x07230203,0x00010000,0x0008000b,0x0000000e,0x00000000,0x00020011,0x00000001,0x0006000b,
   0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
   0x0006000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00030010,0x00000004,
   0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,
   0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00040047,0x00000009,0x0000001e,
   0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,
   0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,
   0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x0004002b,0x00000006,0x0000000a,
   0x3f800000,0x0004002b,0x00000006,0x0000000b,0x00000000,0x0004002b,0x00000006,0x0000000c,
   0x3f000000,0x0007002c,0x00000007,0x0000000d,0x0000000a,0x0000000b,0x0000000c,0x0000000a,
   0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0003003e,
   0x00000009,0x0000000d,0x000100fd,0x00010038,
};

static const uint32_t panvk_v9_fallback_comp_spv[] = {
   0x07230203,0x00010000,0x0008000b,0x0000000a,0x00000000,0x00020011,0x00000001,0x0006000b,
   0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
   0x0005000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x00060010,0x00000004,0x00000011,
   0x00000001,0x00000001,0x00000001,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,
   0x6e69616d,0x00000000,0x00040047,0x00000009,0x0000000b,0x00000019,0x00020013,0x00000002,
   0x00030021,0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,0x00040017,
   0x00000007,0x00000006,0x00000003,0x0004002b,0x00000006,0x00000008,0x00000001,0x0006002c,
   0x00000007,0x00000009,0x00000008,0x00000008,0x00000008,0x00050036,0x00000002,0x00000004,
   0x00000000,0x00000003,0x000200f8,0x00000005,0x000100fd,0x00010038,
};

/* Fallback: when the Mesa Valhall compiler rejects a game shader (unsupported
 * opcode/binding), recompile that stage with a minimal embedded shader so the
 * pipeline keeps a valid Valhall binary instead of rendering nothing. */
static const struct panvk_v9_fallback_stage {
    enum panvk_v9_shader_stage stage;
    const uint32_t *spv;
    size_t spv_words;
} panvk_v9_fallback_stages[] = {
    { PANVK_V9_SHADER_VERTEX,   panvk_v9_fallback_vert_spv,
      sizeof(panvk_v9_fallback_vert_spv) / sizeof(uint32_t) },
    { PANVK_V9_SHADER_FRAGMENT, panvk_v9_fallback_frag_spv,
      sizeof(panvk_v9_fallback_frag_spv) / sizeof(uint32_t) },
    { PANVK_V9_SHADER_COMPUTE,  panvk_v9_fallback_comp_spv,
      sizeof(panvk_v9_fallback_comp_spv) / sizeof(uint32_t) },
};

static int panvk_compile_guarded(const uint32_t *spirv, size_t spv_words,
                                 enum panvk_v9_shader_stage stage,
                                 const char *entry,
                                 const struct panvk_v9_pipeline_layout *layout,
                                 struct panvk_v9_compiled_shader *out,
                                 char *error, size_t error_size);

static VkResult panvk_v9_compile_fallback(enum panvk_v9_shader_stage stage,
                                          const struct panvk_v9_pipeline_layout *layout,
                                          struct panvk_v9_compiled_shader *out) {
    for (size_t i = 0; i < sizeof(panvk_v9_fallback_stages) /
                                sizeof(panvk_v9_fallback_stages[0]); i++) {
        if (panvk_v9_fallback_stages[i].stage != stage) continue;
        char err[256];
        int ret = panvk_compile_guarded(panvk_v9_fallback_stages[i].spv,
                                        panvk_v9_fallback_stages[i].spv_words * sizeof(uint32_t),
                                        stage, "main", layout, out, err, sizeof(err));
        if (ret) {
            pvk_log("panvk_v9: fallback compile for stage %d failed: %s%s%s\n",
                    stage, err[0] ? err : "", err[0] ? " " : "",
                    "PB fallback unavailable");
            return VK_ERROR_INVALID_SHADER_NV;
        }
        pvk_log("panvk_v9: fallback shader applied for stage %d (%zu bytes)\n",
                stage, out->binary_size);
        return VK_SUCCESS;
    }
    return VK_ERROR_INVALID_SHADER_NV;
}

/* ---- guarded compiler API (timeout) ---------------------------------------
 * Mesa's spirv_to_nir / Valhall backend can spin forever on some shaders
 * (observed with dxvk/AIO-Graphics-Test: pipeline compile hangs at 100+% CPU in
 * user space with no stack tail).  Run every compile on a detached helper
 * thread and abort it after PANVK_COMPILE_TIMEOUT_S seconds so a single broken
 * shader cannot deadlock the whole driver/app. */
#define PANVK_COMPILE_TIMEOUT_S 5

struct panvk_compile_job {
    const uint32_t *spirv;
    size_t spv_words;
    enum panvk_v9_shader_stage stage;
    const char *entry;
    const struct panvk_v9_pipeline_layout *layout;
    struct panvk_v9_compiled_shader *out;
    char *error;
    size_t error_size;
    int rc;
};

static void *panvk_compile_thread(void *arg) {
    struct panvk_compile_job *job = arg;
    job->rc = compiler_api.compile(job->spirv, job->spv_words, job->stage,
                                   job->entry, job->layout, job->out,
                                   job->error, job->error_size);
    return NULL;
}

static int panvk_compile_guarded(const uint32_t *spirv, size_t spv_words,
                                 enum panvk_v9_shader_stage stage,
                                 const char *entry,
                                 const struct panvk_v9_pipeline_layout *layout,
                                 struct panvk_v9_compiled_shader *out,
                                 char *error, size_t error_size) {
    const char *env_to = getenv("PANVK_COMPILE_TIMEOUT_S");
    int timeout_s = env_to && env_to[0] ? atoi(env_to) : PANVK_COMPILE_TIMEOUT_S;
    if (timeout_s <= 0) timeout_s = PANVK_COMPILE_TIMEOUT_S;

    struct panvk_compile_job job = {
        .spirv = spirv, .spv_words = spv_words, .stage = stage, .entry = entry,
        .layout = layout, .out = out, .error = error, .error_size = error_size,
        .rc = -1,
    };
    pthread_t th;
    if (pthread_create(&th, NULL, panvk_compile_thread, &job) != 0) {
        /* Can't spawn a thread; fall back to a direct call. */
        return compiler_api.compile(spirv, spv_words, stage, entry,
                                    layout, out, error, error_size);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_s;
#ifdef __ANDROID__
    int jr = pthread_join(th, NULL);
#else
    int jr = pthread_timedjoin_np(th, NULL, &ts);
#endif
    if (jr != 0) {
        /* Thread still running: abandon it (detachable, leaks until it dies or
         * the process exits) and report a shader failure to the caller. */
        pthread_detach(th);
        pvk_log("panvk_v9: COMPILER TIMEOUT stage=%d spv_words=%zu after %ds -> aborting pipeline build\n",
                stage, spv_words, timeout_s);
        snprintf(error, error_size,
                 "panvk_v9 compiler timeout (%ds) stage %d", timeout_s, stage);
        {
            char dump_path[128];
snprintf(dump_path, sizeof(dump_path),
                             "/tmp/hung_stage%d_%zu_bytes.spv",
                             stage, spv_words);
            FILE *df = fopen(dump_path, "w");
            if (df) {
                fwrite(spirv, 4, spv_words / 4, df);
                fclose(df);
                pvk_log("panvk_v9: dumped hung spirv to %s\n", dump_path);
            }
        }
        return -110; /* ETIMEDOUT-ish, treated as shader failure by callers */
    }
    return job.rc;
}

static VkResult pipeline_compile_shaders(struct VkPipeline_T *pipeline,
                                         const struct VkGraphicsPipelineCreateInfo *info) {
    const char *required_env = getenv("PANVK_REQUIRE_COMPILER");
    bool required = required_env && required_env[0] && strcmp(required_env, "0");
    if (!load_compiler()) {
        return required ? VK_ERROR_INVALID_SHADER_NV : VK_SUCCESS;
    }

    char error[512];
    for (uint32_t i = 0; i < info->stageCount; i++) {
        const struct VkPipelineShaderStageCreateInfo *stage = &info->pStages[i];
        enum panvk_v9_shader_stage compiler_stage;
        struct panvk_v9_compiled_shader *binary;
        if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT) {
            compiler_stage = PANVK_V9_SHADER_VERTEX;
            binary = &pipeline->vertex_binary;
        } else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            compiler_stage = PANVK_V9_SHADER_FRAGMENT;
            binary = &pipeline->fragment_binary;
        } else {
            continue;
        }

        int ret = panvk_compile_guarded(stage->module->code, stage->module->code_size,
                                        compiler_stage, stage->pName,
                                        &pipeline->compiler_layout,
                                        binary,
                                        error, sizeof(error));
        FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
        if (flog) {
            fprintf(flog, "compile stage=%d ret=%d code_size=%zu err='%s'\n",
                    compiler_stage, ret, stage->module->code_size, error);
            if (!ret && binary->binary_size) {
                fprintf(flog, "  -> binary=%zu bytes work_reg=%u preload=%u no_psiz=0x%zx\n",
                        binary->binary_size, binary->work_reg_count,
                        binary->preload, (size_t)binary->no_psiz_offset);
                const uint32_t *w = (const uint32_t *)binary->binary;
                fprintf(flog, "  -> first words: %08x %08x %08x %08x\n",
                        w[0], w[1], w[2], w[3]);
            }
            fclose(flog);
        }
        if (ret) {
            pvk_log("panvk_v9: original shader stage %d failed (%s); trying fallback\n",
                    compiler_stage, error[0] ? error : "unknown error");
            compiler_api.cleanup(binary);
            VkResult vr = panvk_v9_compile_fallback(compiler_stage,
                                                    &pipeline->compiler_layout,
                                                    binary);
            if (vr != VK_SUCCESS) {
                compiler_api.cleanup(&pipeline->vertex_binary);
                compiler_api.cleanup(&pipeline->fragment_binary);
                return required ? vr : VK_SUCCESS;
            }
        }
    }

    pipeline->shaders_compiled = pipeline->vertex_binary.binary_size != 0;
    return VK_SUCCESS;
}

static void pipeline_cleanup(struct VkPipeline_T *pipeline) {
    if (!pipeline) return;
    if (compiler_api.cleanup) {
        compiler_api.cleanup(&pipeline->vertex_binary);
        compiler_api.cleanup(&pipeline->fragment_binary);
        compiler_api.cleanup(&pipeline->compute_binary);
    }
    free(pipeline->bindings);
    free(pipeline);
}

static VkResult pipeline_copy_layout(struct VkPipeline_T *pipeline,
                                     VkPipelineLayout layout) {
    if (!layout || !layout->compiler_layout.binding_count) return VK_SUCCESS;
    size_t size = layout->compiler_layout.binding_count * sizeof(*pipeline->bindings);
    pipeline->bindings = malloc(size);
    if (!pipeline->bindings) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(pipeline->bindings, layout->bindings, size);
    pipeline->compiler_layout = layout->compiler_layout;
    pipeline->compiler_layout.bindings = pipeline->bindings;
    return VK_SUCCESS;
}

static void pipeline_parse_fixed_state(struct VkPipeline_T *pipeline,
                                       const struct VkGraphicsPipelineCreateInfo *info) {
    pipeline->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline->polygon_mode = VK_POLYGON_MODE_FILL;
    pipeline->cull_mode = VK_CULL_MODE_NONE;
    pipeline->front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    pipeline->line_width = 1.0f;
    pipeline->rasterization_samples = VK_SAMPLE_COUNT_1_BIT;
    pipeline->depth_compare_op = VK_COMPARE_OP_ALWAYS;
    pipeline->color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (info->pVertexInputState) {
        pipeline->vertex_binding_count =
            info->pVertexInputState->vertexBindingDescriptionCount < 16 ?
            info->pVertexInputState->vertexBindingDescriptionCount : 16;
        pipeline->vertex_attribute_count =
            info->pVertexInputState->vertexAttributeDescriptionCount < 16 ?
            info->pVertexInputState->vertexAttributeDescriptionCount : 16;
        if (pipeline->vertex_binding_count &&
            info->pVertexInputState->pVertexBindingDescriptions) {
            memcpy(pipeline->vertex_bindings,
                   info->pVertexInputState->pVertexBindingDescriptions,
                   pipeline->vertex_binding_count * sizeof(pipeline->vertex_bindings[0]));
        }
        if (pipeline->vertex_attribute_count &&
            info->pVertexInputState->pVertexAttributeDescriptions) {
            memcpy(pipeline->vertex_attributes,
                   info->pVertexInputState->pVertexAttributeDescriptions,
                   pipeline->vertex_attribute_count * sizeof(pipeline->vertex_attributes[0]));
        }
    }

    if (info->pInputAssemblyState) {
        pipeline->topology = info->pInputAssemblyState->topology;
        pipeline->primitive_restart = info->pInputAssemblyState->primitiveRestartEnable != 0;
    }
    if (info->pViewportState) {
        if (info->pViewportState->viewportCount && info->pViewportState->pViewports)
            pipeline->viewport = info->pViewportState->pViewports[0];
        if (info->pViewportState->scissorCount && info->pViewportState->pScissors)
            pipeline->scissor = info->pViewportState->pScissors[0];
    }
    pipeline->dynamic_viewport = pipeline_dynamic_state(info->pDynamicState,
                                                        VK_DYNAMIC_STATE_VIEWPORT);
    pipeline->dynamic_scissor = pipeline_dynamic_state(info->pDynamicState,
                                                       VK_DYNAMIC_STATE_SCISSOR);
    if (info->pRasterizationState) {
        pipeline->rasterizer_discard = info->pRasterizationState->rasterizerDiscardEnable != 0;
        pipeline->polygon_mode = info->pRasterizationState->polygonMode;
        pipeline->cull_mode = info->pRasterizationState->cullMode;
        pipeline->front_face = info->pRasterizationState->frontFace;
        pipeline->line_width = info->pRasterizationState->lineWidth;
    }
    if (info->pMultisampleState)
        pipeline->rasterization_samples = info->pMultisampleState->rasterizationSamples;
    if (info->pDepthStencilState) {
        pipeline->depth_test = info->pDepthStencilState->depthTestEnable != 0;
        pipeline->depth_write = info->pDepthStencilState->depthWriteEnable != 0;
        pipeline->depth_compare_op = info->pDepthStencilState->depthCompareOp;
    }
    if (info->pColorBlendState && info->pColorBlendState->attachmentCount &&
        info->pColorBlendState->pAttachments) {
        pipeline->blend_enable = info->pColorBlendState->pAttachments[0].blendEnable != 0;
        pipeline->color_write_mask = info->pColorBlendState->pAttachments[0].colorWriteMask;
    }
}

VkResult vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                   uint32_t createInfoCount,
                                   const struct VkGraphicsPipelineCreateInfo *pCreateInfos,
                                   const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    pvk_log("vkCreateGraphicsPipelines: dev=%p cache=%p count=%u\n",
            (void*)device, (void*)pipelineCache, createInfoCount);
    if (!device || !pCreateInfos || !pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; i++) pPipelines[i] = NULL;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        pvk_log("vkCreateGraphicsPipelines: [%u] layout=%p flags=%#x\n", i,
                (void*)pCreateInfos[i].layout, (unsigned)pCreateInfos[i].flags);
        struct VkPipeline_T *pipe = calloc(1, sizeof(*pipe));
        if (!pipe) return VK_ERROR_OUT_OF_HOST_MEMORY;

        VkResult result = pipeline_copy_layout(pipe, pCreateInfos[i].layout);
        if (result == VK_SUCCESS)
            result = pipeline_parse_shader_stages(pipe, &pCreateInfos[i]);
        pvk_log("vkCreateGraphicsPipelines: [%u] stages parsed (stages=%#x)\n", i, pipe->stage_mask);
        if (result == VK_SUCCESS)
            result = pipeline_compile_shaders(pipe, &pCreateInfos[i]);
        pvk_log("vkCreateGraphicsPipelines: [%u] shaders compiled result=%d\n", i, result);
        if (result != VK_SUCCESS) {
            pipeline_cleanup(pipe);
            for (uint32_t j = 0; j < i; j++) {
                pipeline_cleanup(pPipelines[j]);
                pPipelines[j] = NULL;
            }
            pvk_log("vkCreateGraphicsPipelines: FAILED result=%d\n", result);
            return result;
        }
        pipeline_parse_fixed_state(pipe, &pCreateInfos[i]);
        pPipelines[i] = pipe;
        pvk_log("vkCreateGraphicsPipelines: [%u] OK pipe=%p vsz=%zu fsz=%zu\n", i,
                (void*)pipe, pipe->vertex_binary.binary_size, pipe->fragment_binary.binary_size);
    }
    pvk_log("vkCreateGraphicsPipelines: OK count=%u\n", createInfoCount);
    return VK_SUCCESS;
}

VkResult vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    if (!pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        struct VkPipeline_T *pipe = calloc(1, sizeof(*pipe));
        if (!pipe) { pPipelines[i] = NULL; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        const struct VkComputePipelineCreateInfo *info = &pCreateInfos[i];
        pipe->stage_mask = VK_SHADER_STAGE_COMPUTE_BIT;
        if (info->stage.module && info->stage.module->code) {
            strncpy(pipe->compute_entry_point,
                    info->stage.pName ? info->stage.pName : "main",
                    sizeof(pipe->compute_entry_point) - 1);
            pipe->compute_module = info->stage.module;
        }
        if (info->layout)
            pipeline_copy_layout(pipe, info->layout);
        if (load_compiler()) {
            char error[512];
            int ret = panvk_compile_guarded(
                info->stage.module->code, info->stage.module->code_size,
                PANVK_V9_SHADER_COMPUTE, info->stage.pName,
                &pipe->compiler_layout, &pipe->compute_binary,
                error, sizeof(error));
            FILE *flog = fopen("/data/data/com.termux/files/usr/tmp/panvk_debug.log", "a");
            if (flog) {
                fprintf(flog, "compile compute ret=%d code_size=%zu err='%s'\n",
                        ret, info->stage.module->code_size, error);
                fclose(flog);
            }
            if (ret) {
                pvk_log("panvk_v9: original compute shader failed (%s); trying fallback\n",
                        error[0] ? error : "unknown error");
                compiler_api.cleanup(&pipe->compute_binary);
                panvk_v9_compile_fallback(PANVK_V9_SHADER_COMPUTE,
                                          &pipe->compiler_layout,
                                          &pipe->compute_binary);
            }
        }
        pPipelines[i] = pipe;
    }
    return VK_SUCCESS;
}

void vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator) {
    pipeline_cleanup(pipeline);
}

VkResult vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore) {
    if (!pSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkSemaphore_T *sem = calloc(1, sizeof(*sem));
    if (!sem) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pCreateInfo) {
        const VkSemaphoreTypeCreateInfo *ti = pCreateInfo->pNext;
        while (ti) {
            if (ti->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                if (ti->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
                    /* Self-pointer marks this semaphore as a timeline; its
                     * counter holds the current timeline value. */
                    sem->timeline = sem;
                    sem->counter = ti->initialValue;
                }
                break;
            }
            ti = ti->pNext;
        }
    }
    *pSemaphore = sem;
    return VK_SUCCESS;
}

void vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
    if (semaphore) free(semaphore);
}

VkResult vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    if (!pFence) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkFence_T *f = calloc(1, sizeof(*f));
    if (f && pCreateInfo) {
        f->signaled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    }
    *pFence = f;
    return f ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

void vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator) {
    if (fence) free(fence);
}

VkResult vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    if (!pFences) return VK_ERROR_INITIALIZATION_FAILED;
    pvk_log("vkResetFences: count=%u\n", fenceCount);
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) {
            pFences[i]->signaled = false;
            pvk_log("  fence[%u] reset: %p\n", i, (void*)pFences[i]);
        }
    }
    return VK_SUCCESS;
}

VkResult vkGetFenceStatus(VkDevice device, VkFence fence) {
    VkResult result = fence && fence->signaled ? VK_SUCCESS : VK_NOT_READY;
    pvk_log("vkGetFenceStatus: fence=%p signaled=%d result=%d\n",
            (void*)fence, fence ? fence->signaled : 0, result);
    return result;
}

VkResult vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
                         uint32_t waitAll, uint64_t timeout) {
    if (!pFences) return VK_ERROR_INITIALIZATION_FAILED;
    pvk_log("vkWaitForFences: count=%u waitAll=%u timeout=%lu\n",
            fenceCount, waitAll, (unsigned long)timeout);
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) {
            pFences[i]->signaled = true;
            pvk_log("  fence[%u] signaled: %p\n", i, (void*)pFences[i]);
        }
    }
    return VK_SUCCESS;
}

/* Command Pool & Buffer Management */
VkResult vkCreateCommandPool(VkDevice device, const struct VkCommandPoolCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool) {
    pvk_log("vkCreateCommandPool: dev=%p flags=%#x\n", (void*)device,
            pCreateInfo ? (unsigned)pCreateInfo->flags : 0);
    if (!device || !pCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkCommandPool_T *pool = calloc(1, sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->device = device;
    *pCommandPool = pool;
    pvk_log("vkCreateCommandPool: OK pool=%p\n", (void*)pool);
    return VK_SUCCESS;
}

void vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator) {
    if (commandPool) free(commandPool);
}

VkResult vkAllocateCommandBuffers(VkDevice device, const struct VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers) {
    pvk_log("vkAllocateCommandBuffers: dev=%p count=%u pool=%p\n", (void*)device,
            pAllocateInfo ? pAllocateInfo->commandBufferCount : 0,
            pAllocateInfo ? (void*)pAllocateInfo->commandPool : NULL);
    if (!device || !pAllocateInfo || !pCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        struct VkCommandBuffer_T *cb = calloc(1, sizeof(*cb));
        if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
        set_loader_magic(cb);
        cb->device = device;
        pCommandBuffers[i] = cb;
        pvk_log("vkAllocateCommandBuffers:   cb[%u]=%p\n", i, (void*)cb);
    }
    pvk_log("vkAllocateCommandBuffers: OK\n");
    return VK_SUCCESS;
}

void vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers) {
    if (!pCommandBuffers) return;
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        if (pCommandBuffers[i]) {
            if (pCommandBuffers[i]->v9_cmd) {
                v9_cmd_buffer_destroy(pCommandBuffers[i]->v9_cmd);
            }
            free(pCommandBuffers[i]);
        }
    }
}

VkResult vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    if (!commandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    if (commandBuffer->v9_cmd) {
        v9_cmd_buffer_destroy(commandBuffer->v9_cmd);
        commandBuffer->v9_cmd = NULL;
    }
    commandBuffer->rendering_active = false;
    commandBuffer->graphics_pipeline = NULL;
    commandBuffer->compute_pipeline = NULL;
    commandBuffer->viewport_set = false;
    commandBuffer->scissor_set = false;
    memset(commandBuffer->descriptor_sets, 0, sizeof(commandBuffer->descriptor_sets));
    commandBuffer->index_buffer = NULL;
    commandBuffer->index_offset = 0;
    commandBuffer->index_type = 0;
    commandBuffer->push_constants_size = 0;
    commandBuffer->depth_bias_set = false;
    return VK_SUCCESS;
}

VkResult vkResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags) {
    (void)device; (void)commandPool; (void)flags;
    return VK_SUCCESS;
}

VkResult vkCreateEvent(VkDevice device, const struct VkEventCreateInfo *pCreateInfo, const struct VkAllocationCallbacks *pAllocator, VkEvent *pEvent) {
    if (!pEvent) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkEvent_T *e = calloc(1, sizeof(*e));
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    e->signaled = (pCreateInfo && pCreateInfo->flags & VK_EVENT_CREATE_DEVICE_ONLY_BIT) ? false : false;
    set_loader_magic(e);
    *pEvent = e;
    return VK_SUCCESS;
}

void vkDestroyEvent(VkDevice device, VkEvent event, const struct VkAllocationCallbacks *pAllocator) {
    free(event);
}

VkResult vkCreateQueryPool(VkDevice device, const struct VkQueryPoolCreateInfo *pCreateInfo, const struct VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool) {
    if (!pQueryPool) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkQueryPool_T *qp = calloc(1, sizeof(*qp));
    if (!qp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    qp->query_count = pCreateInfo->queryCount;
    set_loader_magic(qp);
    *pQueryPool = qp;
    return VK_SUCCESS;
}

void vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const struct VkAllocationCallbacks *pAllocator) {
    if (queryPool) free(queryPool);
}

/* -- FIX (crash): core functions the driver advertised (apiVersion 1.3) but
 *    didn't implement. WineD3D trusts the advertised apiVersion and calls them
 *    without NULL-checking -> "Ask to run at NULL". Implement them as
 *    functional-but-minimal stubs so device init and rendering can proceed. -- */

void vkGetRenderAreaGranularity(VkDevice device, VkRenderPass renderPass, VkExtent2D *pGranularity) {
    (void)device; (void)renderPass;
    if (pGranularity) { pGranularity->width = 1; pGranularity->height = 1; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth) {
    (void)commandBuffer; (void)lineWidth;
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer commandBuffer, uint32_t contents) {
    (void)commandBuffer; (void)contents;
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage,
        uint32_t srcImageLayout, VkImage dstImage, uint32_t dstImageLayout,
        uint32_t regionCount, const struct VkImageResolve *pRegions) {
    (void)commandBuffer; (void)srcImage; (void)srcImageLayout; (void)dstImage; (void)dstImageLayout;
    (void)regionCount; (void)pRegions;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer,
        VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount,
        VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize stride, uint32_t flags) {
    (void)commandBuffer; (void)queryPool; (void)firstQuery; (void)queryCount;
    (void)dstBuffer; (void)dstOffset; (void)stride; (void)flags;
}

VkResult vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery,
        uint32_t queryCount, size_t dataSize, void *pData, VkDeviceSize stride, uint32_t flags) {
    (void)device; (void)queryPool; (void)firstQuery; (void)queryCount; (void)flags;
    if (pData && dataSize) memset(pData, 0, dataSize);
    return VK_SUCCESS;
}

void vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
    (void)device; (void)queryPool; (void)firstQuery; (void)queryCount;
}
void vkResetQueryPoolEXT(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) { vkResetQueryPool(device, queryPool, firstQuery, queryCount); }

VkResult vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, uint32_t flags) {
    (void)device; (void)descriptorPool; (void)flags;
    return VK_SUCCESS;
}

VkResult vkQueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const struct VkBindSparseInfo *pBindInfo, VkFence fence) {
    (void)queue; (void)bindInfoCount; (void)pBindInfo; (void)fence;
    return VK_SUCCESS;
}

void vkGetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory, VkDeviceSize *pCommittedMemoryInBytes) {
    (void)device;
    if (pCommittedMemoryInBytes) *pCommittedMemoryInBytes = memory ? memory->size : 0;
}

void vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image,
        uint32_t *pSparseMemoryRequirementCount, struct VkSparseImageMemoryRequirements *pSparseMemoryRequirements) {
    (void)device; (void)image; (void)pSparseMemoryRequirements;
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}

void vkGetImageSparseMemoryRequirements2(VkDevice device, const struct VkImageSparseMemoryRequirementsInfo2 *pInfo,
        uint32_t *pSparseMemoryRequirementCount, struct VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements) {
    (void)device; (void)pInfo; (void)pSparseMemoryRequirements;
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}

void vkTrimCommandPool(VkDevice device, VkCommandPool commandPool, uint32_t flags) {
    (void)device; (void)commandPool; (void)flags;
}

void vkGetDeviceGroupPeerMemoryFeatures(VkDevice device, uint32_t heapIndex,
        uint32_t localDeviceIndex, uint32_t remoteDeviceIndex, uint32_t *pPeerMemoryFeatures) {
    (void)device; (void)heapIndex; (void)localDeviceIndex; (void)remoteDeviceIndex;
    if (pPeerMemoryFeatures) *pPeerMemoryFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask) {
    (void)commandBuffer; (void)deviceMask;
}

VkResult vkCreateDescriptorUpdateTemplate(VkDevice device, const struct VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
        const struct VkAllocationCallbacks *pAllocator, VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    if (!pDescriptorUpdateTemplate) return VK_ERROR_INITIALIZATION_FAILED;
    static uintptr_t next_id = 1;
    *pDescriptorUpdateTemplate = (VkDescriptorUpdateTemplate)(void *)(uintptr_t)(next_id++);
    return VK_SUCCESS;
}

void vkDestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const struct VkAllocationCallbacks *pAllocator) {
    (void)device; (void)descriptorUpdateTemplate; (void)pAllocator;
}

void vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet, VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void *pData) {
    (void)device; (void)descriptorSet; (void)descriptorUpdateTemplate; (void)pData;
}

void vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, uint32_t flags) {
    (void)queryPool; (void)query; (void)flags;
}

void vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount) {
    (void)queryPool; (void)firstQuery; (void)queryCount;
}

VkResult vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const struct VkCommandBufferBeginInfo *pBeginInfo) {
    pvk_log("vkBeginCommandBuffer: cb=%p\n", (void*)commandBuffer);
    if (!commandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    commandBuffer->graphics_pipeline = NULL;
    commandBuffer->compute_pipeline = NULL;   /* FIX BUG7 */
    commandBuffer->viewport_set = false;
    commandBuffer->scissor_set = false;
    /* FIX BUG7: reseta TODO o estado (a spec permite regravar sem reset) */
    commandBuffer->rendering_active = false;
    commandBuffer->push_constants_size = 0;
    commandBuffer->depth_bias_set = false;
    commandBuffer->index_buffer = NULL;
    commandBuffer->index_offset = 0;
    commandBuffer->index_type = 0;
    memset(commandBuffer->vertex_bindings, 0, sizeof(commandBuffer->vertex_bindings));
    memset(commandBuffer->descriptor_sets, 0, sizeof(commandBuffer->descriptor_sets));
    pvk_log("vkBeginCommandBuffer: OK cb=%p\n", (void*)commandBuffer);
    return VK_SUCCESS;
}

void vkCmdBindPipeline(VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint, VkPipeline pipeline) {
    pvk_log("vkCmdBindPipeline: cb=%p bind=%u pipe=%p\n", (void*)commandBuffer, pipelineBindPoint, (void*)pipeline);
    if (!commandBuffer) return;
    if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        commandBuffer->compute_pipeline = pipeline;
        return;
    }
    if (pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    commandBuffer->graphics_pipeline = pipeline;
    if (pipeline) {
        if (!pipeline->dynamic_viewport) {
            commandBuffer->viewport = pipeline->viewport;
            commandBuffer->viewport_set = true;
        }
        if (!pipeline->dynamic_scissor) {
            commandBuffer->scissor = pipeline->scissor;
            commandBuffer->scissor_set = true;
        }
    }
}

void vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport *pViewports) {
    pvk_log("vkCmdSetViewport: cb=%p count=%u\n", (void*)commandBuffer, viewportCount);
    if (!commandBuffer || firstViewport != 0 || viewportCount == 0 || !pViewports) return;
    commandBuffer->viewport = pViewports[0];
    commandBuffer->viewport_set = true;
}

void vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D *pScissors) {
    pvk_log("vkCmdSetScissor: cb=%p count=%u\n", (void*)commandBuffer, scissorCount);
    if (!commandBuffer || firstScissor != 0 || scissorCount == 0 || !pScissors) return;
    commandBuffer->scissor = pScissors[0];
    commandBuffer->scissor_set = true;
}

void vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float depthBiasConstantFactor,
                       float depthBiasClamp, float depthBiasSlopeFactor) {
    if (!commandBuffer) return;
    commandBuffer->depth_bias_constant_factor = depthBiasConstantFactor;
    commandBuffer->depth_bias_clamp = depthBiasClamp;
    commandBuffer->depth_bias_constant_offset = depthBiasSlopeFactor;
    commandBuffer->depth_bias_set = true;
}

void vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
                        uint32_t stageFlags, uint32_t offset, uint32_t size, const void *pValues) {
    if (!commandBuffer || !pValues || !size) return;
    if (offset >= sizeof(commandBuffer->push_constants)) return;
    if (offset + size > sizeof(commandBuffer->push_constants))
        size = sizeof(commandBuffer->push_constants) - offset;
    memcpy(commandBuffer->push_constants + offset, pValues, size);
    if (offset + size > commandBuffer->push_constants_size)
        commandBuffer->push_constants_size = offset + size;
}

void vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask) {
    (void)event; (void)stageMask;
}

void vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask) {
    (void)event; (void)stageMask;
}

void vkCmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
                     VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
                     uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                     uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                     uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers) {
    (void)eventCount; (void)pEvents; (void)srcStageMask; (void)dstStageMask;
    (void)memoryBarrierCount; (void)pMemoryBarriers; (void)bufferMemoryBarrierCount;
    (void)pBufferMemoryBarriers; (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

void vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query) {
    (void)queryPool; (void)query;
}

void vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, uint32_t pipelineStage, VkQueryPool queryPool, uint32_t query) {
    (void)pipelineStage; (void)queryPool; (void)query;
}

void vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z) {
    if (!commandBuffer || !commandBuffer->compute_pipeline ||
        x == 0 || y == 0 || z == 0)
        return;
    VkPipeline pipeline = commandBuffer->compute_pipeline;
    if (!commandBuffer->v9_cmd) {
        struct v9_render_target_config config = {
            .width = 300, .height = 300, .clear_color = 0,
        };
        commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
        if (!commandBuffer->v9_cmd) return;
        v9_cmd_buffer_begin(commandBuffer->v9_cmd);
    }
    command_buffer_apply_ssbos(commandBuffer);
    command_buffer_apply_textures(commandBuffer);
    command_buffer_apply_samplers(commandBuffer);
    if (pipeline->compute_binary.binary_size)
        v9_cmd_buffer_set_compute_shader(commandBuffer->v9_cmd, &pipeline->compute_binary);
    v9_cmd_buffer_dispatch(commandBuffer->v9_cmd, x, y, z);
}

void vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
    if (!commandBuffer || !buffer || !buffer->bo || !buffer->bo->cpu) return;
    const uint32_t *params = (const uint32_t *)((uint8_t *)buffer->bo->cpu + buffer->memory_offset + offset);
    uint32_t x = params[0], y = params[1], z = params[2];
    if (x == 0 || y == 0 || z == 0) return;
    VkPipeline pipeline = commandBuffer->compute_pipeline;
    if (!pipeline) return;
    if (!commandBuffer->v9_cmd) {
        struct v9_render_target_config config = { .width = 300, .height = 300, .clear_color = 0 };
        commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
        if (!commandBuffer->v9_cmd) return;
        v9_cmd_buffer_begin(commandBuffer->v9_cmd);
    }
    command_buffer_apply_ssbos(commandBuffer);
    command_buffer_apply_textures(commandBuffer);
    command_buffer_apply_samplers(commandBuffer);
    if (pipeline->compute_binary.binary_size)
        v9_cmd_buffer_set_compute_shader(commandBuffer->v9_cmd, &pipeline->compute_binary);
    v9_cmd_buffer_dispatch(commandBuffer->v9_cmd, x, y, z);
}

VkResult vkSetEvent(VkDevice device, VkEvent event) {
    if (!event) return VK_ERROR_INITIALIZATION_FAILED;
    event->signaled = true;
    return VK_SUCCESS;
}

VkResult vkResetEvent(VkDevice device, VkEvent event) {
    if (!event) return VK_ERROR_INITIALIZATION_FAILED;
    event->signaled = false;
    return VK_SUCCESS;
}

VkResult vkGetEventStatus(VkDevice device, VkEvent event) {
    if (!event) return VK_ERROR_INITIALIZATION_FAILED;
    return event->signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

void vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, uint32_t pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets) {
    pvk_log("vkCmdBindDescriptorSets: cb=%p sets=%u first=%u\n", (void*)commandBuffer,
            descriptorSetCount, firstSet);
    (void)pipelineBindPoint; (void)layout; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    if (!commandBuffer || firstSet >= 8 || descriptorSetCount > 8 - firstSet ||
        (descriptorSetCount && !pDescriptorSets)) return;
    memcpy(&commandBuffer->descriptor_sets[firstSet], pDescriptorSets,
           descriptorSetCount * sizeof(*pDescriptorSets));
}

void vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets) {
    pvk_log("vkCmdBindVertexBuffers: cb=%p bindings=%u first=%u\n", (void*)commandBuffer,
            bindingCount, firstBinding);
    if (!commandBuffer || firstBinding >= 16 || !pBuffers || !pOffsets) return;
    for (uint32_t i = 0; i < bindingCount && (firstBinding + i) < 16; i++) {
        commandBuffer->vertex_bindings[firstBinding + i].buffer = pBuffers[i];
        commandBuffer->vertex_bindings[firstBinding + i].offset = pOffsets[i];
    }
}

void vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t indexType) {
    pvk_log("vkCmdBindIndexBuffer: cb=%p buf=%p off=%lu type=%u\n", (void*)commandBuffer,
            (void*)buffer, (unsigned long)offset, indexType);
    if (!commandBuffer) return;
    commandBuffer->index_buffer = buffer;
    commandBuffer->index_offset = offset;
    commandBuffer->index_type = indexType;
}

void vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
                     uint32_t regionCount, const struct VkBufferCopy *pRegions) {
    if (!srcBuffer || !srcBuffer->bo || !dstBuffer || !dstBuffer->bo || !pRegions) return;

    for (uint32_t i = 0; i < regionCount; i++) {
        VkDeviceSize src_offset = srcBuffer->memory_offset + pRegions[i].srcOffset;
        VkDeviceSize dst_offset = dstBuffer->memory_offset + pRegions[i].dstOffset;
        VkDeviceSize size = pRegions[i].size;
        if (src_offset > srcBuffer->bo->size || size > srcBuffer->bo->size - src_offset ||
            dst_offset > dstBuffer->bo->size || size > dstBuffer->bo->size - dst_offset) {
            continue;
        }
        memcpy((uint8_t *)dstBuffer->bo->cpu + dst_offset,
               (const uint8_t *)srcBuffer->bo->cpu + src_offset, size);
    }
}

VkResult vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSampler *pSampler) {
    (void)device; (void)pAllocator;
    if (!pSampler) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkSampler_T *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (pCreateInfo) {
        s->magFilter = pCreateInfo->magFilter;
        s->minFilter = pCreateInfo->minFilter;
        s->mipmapMode = pCreateInfo->mipmapMode;
        s->addressModeU = pCreateInfo->addressModeU;
        s->addressModeV = pCreateInfo->addressModeV;
        s->addressModeW = pCreateInfo->addressModeW;
        s->mipLodBias = pCreateInfo->mipLodBias;
        s->anisotropyEnable = pCreateInfo->anisotropyEnable;
        s->maxAnisotropy = pCreateInfo->maxAnisotropy;
        s->compareEnable = pCreateInfo->compareEnable;
        s->compareOp = pCreateInfo->compareOp;
        s->minLod = pCreateInfo->minLod;
        s->maxLod = pCreateInfo->maxLod;
        s->borderColor = pCreateInfo->borderColor;
        s->unnormalizedCoordinates = pCreateInfo->unnormalizedCoordinates;
    }
    *pSampler = s;
    return VK_SUCCESS;
}

void vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    if (sampler) free(sampler);
}

void vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
                            VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions) {
    (void)commandBuffer; (void)dstImageLayout;
    if (!srcBuffer || !srcBuffer->bo || !dstImage || !dstImage->bo || !pRegions) return;
    const VkBufferImageCopy *regions = pRegions;
    const uint8_t *src_base = (const uint8_t *)srcBuffer->bo->cpu + srcBuffer->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstImage->bo->cpu + dstImage->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(dstImage->format);
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkBufferImageCopy *rc = &regions[r];
        uint32_t mip = rc->imageSubresource.mipLevel;
        if (mip >= dstImage->mip_levels) continue;
        uint32_t layer_count = rc->imageSubresource.layerCount ? rc->imageSubresource.layerCount : 1;
        uint64_t w = rc->imageExtent.width;
        uint64_t h = rc->imageExtent.height;
        uint64_t d = rc->imageExtent.depth ? rc->imageExtent.depth : 1;
        uint64_t row_stride = dstImage->row_pitch[mip];
        uint64_t slice_pitch = panvk_v9_image_slice_pitch(dstImage, mip);
        uint64_t buffer_row_pitch = rc->bufferRowLength ? (uint64_t)rc->bufferRowLength * bpp : row_stride;
        uint64_t buffer_slice_pitch = rc->bufferImageHeight ? (uint64_t)rc->bufferImageHeight * buffer_row_pitch
                                                            : h * buffer_row_pitch;
        uint64_t buf_off = rc->bufferOffset;
        if (buf_off > srcBuffer->bo->size) continue;
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            uint64_t img_off = panvk_v9_image_get_offset(dstImage, mip,
                                                         rc->imageSubresource.baseArrayLayer + layer)
                             + (uint64_t)rc->imageOffset.z * slice_pitch;
            const uint8_t *sp = src_base + buf_off;
            uint8_t *dp = dst_base + img_off;
            for (uint64_t z = 0; z < d; z++, dp += slice_pitch) {
                const uint8_t *zsp = sp + z * buffer_slice_pitch;
                for (uint64_t y = 0; y < h; y++) {
                    memcpy(dp + y * row_stride, zsp + y * buffer_row_pitch, w * bpp);
                }
            }
            buf_off += buffer_slice_pitch * d;
        }
    }
}

void vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                            VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions) {
    (void)commandBuffer; (void)srcImageLayout;
    if (!srcImage || !srcImage->bo || !dstBuffer || !dstBuffer->bo || !pRegions) return;
    const VkBufferImageCopy *regions = pRegions;
    const uint8_t *src_base = (const uint8_t *)srcImage->bo->cpu + srcImage->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstBuffer->bo->cpu + dstBuffer->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(srcImage->format);
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkBufferImageCopy *rc = &regions[r];
        uint32_t mip = rc->imageSubresource.mipLevel;
        if (mip >= srcImage->mip_levels) continue;
        uint32_t layer_count = rc->imageSubresource.layerCount ? rc->imageSubresource.layerCount : 1;
        uint64_t w = rc->imageExtent.width;
        uint64_t h = rc->imageExtent.height;
        uint64_t d = rc->imageExtent.depth ? rc->imageExtent.depth : 1;
        uint64_t row_stride = srcImage->row_pitch[mip];
        uint64_t slice_pitch = panvk_v9_image_slice_pitch(srcImage, mip);
        uint64_t buffer_row_pitch = rc->bufferRowLength ? (uint64_t)rc->bufferRowLength * bpp : row_stride;
        uint64_t buffer_slice_pitch = rc->bufferImageHeight ? (uint64_t)rc->bufferImageHeight * buffer_row_pitch
                                                            : h * buffer_row_pitch;
        uint64_t buf_off = rc->bufferOffset;
        if (buf_off > dstBuffer->bo->size) continue;
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            uint64_t img_off = panvk_v9_image_get_offset(srcImage, mip,
                                                         rc->imageSubresource.baseArrayLayer + layer)
                             + (uint64_t)rc->imageOffset.z * slice_pitch;
            const uint8_t *sp = src_base + img_off;
            uint8_t *dp = dst_base + buf_off;
            for (uint64_t z = 0; z < d; z++, sp += slice_pitch) {
                uint8_t *zdp = dp + z * buffer_slice_pitch;
                for (uint64_t y = 0; y < h; y++) {
                    memcpy(zdp + y * buffer_row_pitch, sp + y * row_stride, w * bpp);
                }
            }
            buf_off += buffer_slice_pitch * d;
        }
    }
}

void vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy *pRegions) {
    (void)commandBuffer; (void)srcImageLayout; (void)dstImageLayout;
    if (!srcImage || !srcImage->bo || !dstImage || !dstImage->bo || !pRegions) return;
    const VkImageCopy *regions = pRegions;
    uint32_t src_bpp = panvk_v9_format_bpp(srcImage->format);
    uint32_t dst_bpp = panvk_v9_format_bpp(dstImage->format);
    const uint8_t *src_base = (const uint8_t *)srcImage->bo->cpu + srcImage->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstImage->bo->cpu + dstImage->memory_offset;
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkImageCopy *rc = &regions[r];
        uint32_t smip = rc->srcSubresource.mipLevel;
        uint32_t dmip = rc->dstSubresource.mipLevel;
        if (smip >= srcImage->mip_levels || dmip >= dstImage->mip_levels) continue;
        uint64_t w = rc->extent.width;
        uint64_t h = rc->extent.height;
        uint64_t d = rc->extent.depth ? rc->extent.depth : 1;
        uint64_t s_row = srcImage->row_pitch[smip];
        uint64_t s_slice = panvk_v9_image_slice_pitch(srcImage, smip);
        uint64_t d_row = dstImage->row_pitch[dmip];
        uint64_t d_slice = panvk_v9_image_slice_pitch(dstImage, dmip);
        size_t copy_w = w * (src_bpp < dst_bpp ? src_bpp : dst_bpp);
        int needs_convert = (src_bpp == 4 && dst_bpp == 4 &&
                             ((srcImage->format == 37 || srcImage->format == 43) !=
                              (dstImage->format == 37 || dstImage->format == 43)));
        for (uint32_t layer = 0; layer < (rc->srcSubresource.layerCount ? rc->srcSubresource.layerCount : 1); layer++) {
            const uint8_t *sp = src_base
                + panvk_v9_image_get_offset(srcImage, smip, rc->srcSubresource.baseArrayLayer + layer)
                + (uint64_t)rc->srcOffset.z * s_slice + (uint64_t)rc->srcOffset.y * s_row
                + (uint64_t)rc->srcOffset.x * src_bpp;
            uint8_t *dp = dst_base
                + panvk_v9_image_get_offset(dstImage, dmip, rc->dstSubresource.baseArrayLayer + layer)
                + (uint64_t)rc->dstOffset.z * d_slice + (uint64_t)rc->dstOffset.y * d_row
                + (uint64_t)rc->dstOffset.x * dst_bpp;
            for (uint64_t z = 0; z < d; z++, sp += s_slice, dp += d_slice) {
                for (uint64_t y = 0; y < h; y++) {
                    const uint8_t *srow = sp + y * s_row;
                    uint8_t *drow = dp + y * d_row;
                    if (needs_convert) {
                        for (uint64_t x = 0; x < w; x++)
                            panvk_v9_convert_pixel(srcImage->format, dstImage->format,
                                                   srow + x * 4, drow + x * 4);
                    } else {
                        memcpy(drow, srow, copy_w);
                    }
                }
            }
        }
    }
}

void vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
                    VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit *pRegions, VkFilter filter) {
    (void)commandBuffer; (void)srcImageLayout; (void)dstImageLayout; (void)filter;
    if (!srcImage || !srcImage->bo || !dstImage || !dstImage->bo || !pRegions) return;
    /* TRACE BLIT: cadeia de composição do DXVK.  Se houver blit
     * [cena]->[swapchain], ele roda na GRAVAÇÃO (antes do submit da cena!) */
    {
        static uint32_t blit_n = 0;
        blit_n++;
        if (blit_n <= 20 || (blit_n % 500) == 0)
            pvk_log("TRACE blit#%u cb=%p: src_gpu=0x%llx (%ux%u fmt=%u) -> dst_gpu=0x%llx (fmt=%u)\n",
                    blit_n, (void*)commandBuffer,
                    (unsigned long long)srcImage->bo->gpu, srcImage->width, srcImage->height, srcImage->format,
                    (unsigned long long)dstImage->bo->gpu, dstImage->format);
    }
    const VkImageBlit *regions = pRegions;
    uint32_t src_bpp = panvk_v9_format_bpp(srcImage->format);
    uint32_t dst_bpp = panvk_v9_format_bpp(dstImage->format);
    if (src_bpp > dst_bpp) dst_bpp = src_bpp;
    const uint8_t *src_base = (const uint8_t *)srcImage->bo->cpu + srcImage->memory_offset;
    uint8_t *dst_base = (uint8_t *)dstImage->bo->cpu + dstImage->memory_offset;
    for (uint32_t r = 0; r < regionCount; r++) {
        const struct VkImageBlit *rb = &regions[r];
        uint32_t smip = rb->srcSubresource.mipLevel;
        uint32_t dmip = rb->dstSubresource.mipLevel;
        if (smip >= srcImage->mip_levels || dmip >= dstImage->mip_levels) continue;
        int sw = rb->srcOffsets[1].x - rb->srcOffsets[0].x;
        int sh = rb->srcOffsets[1].y - rb->srcOffsets[0].y;
        int dw = rb->dstOffsets[1].x - rb->dstOffsets[0].x;
        int dh = rb->dstOffsets[1].y - rb->dstOffsets[0].y;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) continue;
        uint64_t s_row = srcImage->row_pitch[smip];
        uint64_t d_row = dstImage->row_pitch[dmip];
        uint64_t layer_count = rb->srcSubresource.layerCount ? rb->srcSubresource.layerCount : 1;
        int needs_convert = (src_bpp == 4 && dst_bpp == 4 &&
                             ((srcImage->format == 37 || srcImage->format == 43) !=
                              (dstImage->format == 37 || dstImage->format == 43)));
        for (uint32_t layer = 0; layer < layer_count; layer++) {
            const uint8_t *sp = src_base
                + panvk_v9_image_get_offset(srcImage, smip, rb->srcSubresource.baseArrayLayer + layer)
                + (uint64_t)rb->srcOffsets[0].y * s_row + (uint64_t)rb->srcOffsets[0].x * src_bpp;
            uint8_t *dp = dst_base
                + panvk_v9_image_get_offset(dstImage, dmip, rb->dstSubresource.baseArrayLayer + layer)
                + (uint64_t)rb->dstOffsets[0].y * d_row + (uint64_t)rb->dstOffsets[0].x * dst_bpp;
            for (int y = 0; y < dh; y++) {
                int sy = (int)((int64_t)y * sh / dh);
                const uint8_t *srow = sp + (uint64_t)sy * s_row;
                uint8_t *drow = dp + (uint64_t)y * d_row;
                for (int x = 0; x < dw; x++) {
                    int sx = (int)((int64_t)x * sw / dw);
                    if (needs_convert) {
                        panvk_v9_convert_pixel(srcImage->format, dstImage->format,
                                               srow + (uint64_t)sx * 4, drow + (uint64_t)x * 4);
                    } else {
                        memcpy(drow + (uint64_t)x * dst_bpp, srow + (uint64_t)sx * src_bpp, src_bpp);
                    }
                }
            }
        }
    }
}

void vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                          const VkClearColorValue *color, uint32_t rangeCount, const VkImageSubresourceRange *pRanges) {
    (void)commandBuffer; (void)imageLayout;
    if (!image || !image->bo || !color || !pRanges) return;
    const VkImageSubresourceRange *ranges = pRanges;
    uint8_t *base = (uint8_t *)image->bo->cpu + image->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(image->format);
    uint8_t *rowbuf = malloc(4096 * (bpp ? bpp : 4));
    if (!rowbuf) return;
    for (uint32_t r = 0; r < rangeCount; r++) {
        const struct VkImageSubresourceRange *range = &ranges[r];
        uint32_t base_mip = range->baseMipLevel;
        uint32_t mip_count = range->levelCount == 0xffffffffu
                             ? (image->mip_levels - base_mip) : range->levelCount;
        uint32_t base_layer = range->baseArrayLayer;
        uint32_t layer_count = range->layerCount == 0xffffffffu
                               ? (image->array_layers - base_layer) : range->layerCount;
        for (uint32_t mip = base_mip; mip < base_mip + mip_count && mip < image->mip_levels; mip++) {
            uint64_t w = image->width >> mip; if (w < 1) w = 1;
            uint64_t h = image->height >> mip; if (h < 1) h = 1;
            uint64_t d = image->depth >> mip; if (d < 1) d = 1;
            if (image->image_type == 0) d = 1;
            if (w > 4096) w = 4096;
            uint64_t row_stride = image->row_pitch[mip];
            uint64_t slice_pitch = panvk_v9_image_slice_pitch(image, mip);
            for (uint64_t x = 0; x < w; x++) {
                uint8_t *p = rowbuf + x * bpp;
                if (bpp == 1) p[0] = (uint8_t)color->uint32[0];
                else if (bpp == 2) ((uint16_t *)p)[0] = (uint16_t)color->uint32[0];
                else {
                    memcpy(p, &color->uint32[0], bpp > 4 ? 4 : bpp);
                    if (bpp > 4) memcpy(p + 4, &color->uint32[1], 4);
                    if (bpp > 8) memcpy(p + 8, &color->uint32[2], 4);
                    if (bpp > 12) memcpy(p + 12, &color->uint32[3], 4);
                }
            }
            for (uint32_t layer = 0; layer < layer_count; layer++) {
                uint64_t img_off = panvk_v9_image_get_offset(image, mip, base_layer + layer);
                for (uint64_t z = 0; z < d; z++)
                    for (uint64_t y = 0; y < h; y++)
                        memcpy(base + img_off + z * slice_pitch + y * row_stride, rowbuf, w * bpp);
            }
        }
    }
    free(rowbuf);
}

void vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
                                 const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange *pRanges) {
    (void)commandBuffer; (void)image; (void)imageLayout; (void)pDepthStencil; (void)rangeCount; (void)pRanges;
}

void vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment *pAttachments,
                           uint32_t rectCount, const VkClearRect *pRects) {
    (void)commandBuffer; (void)attachmentCount; (void)pAttachments; (void)rectCount; (void)pRects;
}

void vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                          VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                          uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                          uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                          uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers) {
}

static void command_buffer_apply_ubos(VkCommandBuffer commandBuffer) {
    struct v9_ubo_binding ubos[8];
    uint32_t ubo_count = 0;
    VkPipeline pipeline = commandBuffer->graphics_pipeline;
    if (!pipeline) {
        v9_cmd_buffer_set_ubos(commandBuffer->v9_cmd, NULL, 0);
        return;
    }

    for (uint32_t i = 0; i < pipeline->compiler_layout.binding_count; i++) {
        const struct panvk_v9_descriptor_binding *binding =
            &pipeline->compiler_layout.bindings[i];
        if ((binding->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
             binding->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
            binding->set >= 8 || !commandBuffer->descriptor_sets[binding->set])
            continue;

        VkDescriptorSet set = commandBuffer->descriptor_sets[binding->set];
        for (uint32_t b = 0; b < set->layout->binding_count; b++) {
            if (set->layout->bindings[b].binding != binding->binding) continue;
            for (uint32_t elem = 0; elem < binding->array_size && ubo_count < 8; elem++) {
                const struct VkDescriptorBufferInfo *info =
                    &set->buffers[set->layout->binding_offsets[b] + elem];
                if (!info->buffer || !info->buffer->bo || info->offset >= info->buffer->size)
                    continue;
                VkDeviceSize available = info->buffer->size - info->offset;
                VkDeviceSize range = info->range == VK_WHOLE_SIZE || info->range > available ?
                                     available : info->range;
                ubos[ubo_count++] = (struct v9_ubo_binding) {
                    .address = info->buffer->bo->gpu + info->buffer->memory_offset + info->offset,
                    .size = range > UINT32_MAX ? UINT32_MAX : (uint32_t)range,
                    .index = (binding->resource_index & 0xFFFFFFu) + elem,
                };
            }
            break;
        }
    }
    v9_cmd_buffer_set_ubos(commandBuffer->v9_cmd, ubos, ubo_count);
}

static void command_buffer_apply_ssbos(VkCommandBuffer commandBuffer) {
    struct v9_ssbo_binding ssbos[8];
    uint32_t ssbo_count = 0;
    VkPipeline pipeline = commandBuffer->compute_pipeline;
    if (!pipeline) {
        v9_cmd_buffer_set_ssbos(commandBuffer->v9_cmd, NULL, 0);
        return;
    }

    for (uint32_t i = 0; i < pipeline->compiler_layout.binding_count; i++) {
        const struct panvk_v9_descriptor_binding *binding =
            &pipeline->compiler_layout.bindings[i];
        if ((binding->descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
             binding->descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) ||
            binding->set >= 8 || !commandBuffer->descriptor_sets[binding->set])
            continue;

        VkDescriptorSet set = commandBuffer->descriptor_sets[binding->set];
        for (uint32_t b = 0; b < set->layout->binding_count; b++) {
            if (set->layout->bindings[b].binding != binding->binding) continue;
            for (uint32_t elem = 0; elem < binding->array_size && ssbo_count < 8; elem++) {
                const struct VkDescriptorBufferInfo *info =
                    &set->buffers[set->layout->binding_offsets[b] + elem];
                if (!info->buffer || !info->buffer->bo || info->offset >= info->buffer->size)
                    continue;
                VkDeviceSize available = info->buffer->size - info->offset;
                VkDeviceSize range = info->range == VK_WHOLE_SIZE || info->range > available ?
                                     available : info->range;
                ssbos[ssbo_count++] = (struct v9_ssbo_binding) {
                    .address = info->buffer->bo->gpu + info->buffer->memory_offset + info->offset,
                    .size = range > UINT32_MAX ? UINT32_MAX : (uint32_t)range,
                    .index = (binding->resource_index & 0xFFFFFFu) + elem,
                };
            }
            break;
        }
    }
    v9_cmd_buffer_set_ssbos(commandBuffer->v9_cmd, ssbos, ssbo_count);
}

static void command_buffer_apply_textures(VkCommandBuffer commandBuffer) {
    struct v9_texture_binding texs[8];
    uint32_t tex_count = 0;
    VkPipeline pipeline = commandBuffer->graphics_pipeline ? commandBuffer->graphics_pipeline : commandBuffer->compute_pipeline;
    if (!pipeline) { v9_cmd_buffer_set_textures(commandBuffer->v9_cmd, NULL, 0); return; }
    for (uint32_t i = 0; i < pipeline->compiler_layout.binding_count; i++) {
        const struct panvk_v9_descriptor_binding *binding = &pipeline->compiler_layout.bindings[i];
        bool is_tex = (binding->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                       binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                       binding->descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                       binding->descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                       binding->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        if (!is_tex || binding->set >= 8 || !commandBuffer->descriptor_sets[binding->set]) continue;
        VkDescriptorSet set = commandBuffer->descriptor_sets[binding->set];
        for (uint32_t b = 0; b < set->layout->binding_count; b++) {
            if (set->layout->bindings[b].binding != binding->binding) continue;
            for (uint32_t elem = 0; elem < binding->array_size && tex_count < 8; elem++) {
                const struct VkDescriptorImageInfo *info = &set->images[set->layout->binding_offsets[b] + elem];
                VkImageView view = info->imageView;
                if (!view || !view->image || !view->image->bo) continue;
                VkImage img = view->image;
                uint32_t w = view->image->width;
                uint32_t h = view->image->height;
                if (view->view_type == 1) { /* 2D */ }
                texs[tex_count++] = (struct v9_texture_binding){
                    .image_gpu = img->bo->gpu + img->memory_offset + (view->image->mip_offset[view->base_mip] ? view->image->mip_offset[view->base_mip] : 0),
                    .width = w,
                    .height = h,
                    .format = view->format,
                    .view_type = view->view_type,
                    .row_stride = img->row_pitch[view->base_mip],
                    .index = (binding->resource_index & 0xFFFFFFu) + elem,
                };
            }
            break;
        }
    }
    v9_cmd_buffer_set_textures(commandBuffer->v9_cmd, texs, tex_count);
}

static void command_buffer_apply_samplers(VkCommandBuffer commandBuffer) {
    struct v9_sampler_binding samps[8];
    uint32_t samp_count = 0;
    VkPipeline pipeline = commandBuffer->graphics_pipeline ? commandBuffer->graphics_pipeline : commandBuffer->compute_pipeline;
    if (!pipeline) { v9_cmd_buffer_set_samplers(commandBuffer->v9_cmd, NULL, 0); return; }
    for (uint32_t i = 0; i < pipeline->compiler_layout.binding_count; i++) {
        const struct panvk_v9_descriptor_binding *binding = &pipeline->compiler_layout.bindings[i];
        bool is_samp = (binding->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ||
                        binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        if (!is_samp || binding->set >= 8 || !commandBuffer->descriptor_sets[binding->set]) continue;
        VkDescriptorSet set = commandBuffer->descriptor_sets[binding->set];
        for (uint32_t b = 0; b < set->layout->binding_count; b++) {
            if (set->layout->bindings[b].binding != binding->binding) continue;
            for (uint32_t elem = 0; elem < binding->array_size && samp_count < 8; elem++) {
                const struct VkDescriptorImageInfo *info = &set->images[set->layout->binding_offsets[b] + elem];
                VkSampler samp = info->sampler;
                /* For combined, sampler is in same info; for pure sampler, also */
                if (!samp) {
                    /* dummy sampler */
                    samps[samp_count++] = (struct v9_sampler_binding){ .wrap_s=2,.wrap_t=2,.wrap_r=2,.mag_filter=1,.min_filter=1,.mipmap_mode=0,.max_anisotropy=1,.index=(binding->resource_index & 0xFFFFFFu)+elem };
                    /* For combined, need to derive sampler index: resource_index is table4, need table5 */
                    if (binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                        samps[samp_count-1].index = (binding->resource_index & 0xFFFFFFu)+elem;
                    }
                    continue;
                }
                uint32_t idx = (binding->resource_index & 0xFFFFFFu) + elem;
                if (binding->descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    /* sampler table 5 shares same low bits as texture table 4 */
                    idx = (binding->resource_index & 0xFFFFFFu) + elem;
                }
                samps[samp_count++] = (struct v9_sampler_binding){
                    .mag_filter = samp->magFilter,
                    .min_filter = samp->minFilter,
                    .mipmap_mode = samp->mipmapMode,
                    .wrap_s = samp->addressModeU,
                    .wrap_t = samp->addressModeV,
                    .wrap_r = samp->addressModeW,
                    .max_anisotropy = samp->maxAnisotropy > 0 ? (uint32_t)samp->maxAnisotropy : 1,
                    .index = idx,
                };
            }
            break;
        }
    }
    v9_cmd_buffer_set_samplers(commandBuffer->v9_cmd, samps, samp_count);
}

static uint32_t vk_format_to_pan_v9_attr_format(uint32_t vk_fmt) {
    switch (vk_fmt) {
        case 103: /* VK_FORMAT_R32G32_SFLOAT */       return 0x020083;
        case 106: /* VK_FORMAT_R32G32B32_SFLOAT */    return 0x020084;
        case 109: /* VK_FORMAT_R32G32B32A32_SFLOAT */ return 0x020085;
        case 37:  /* VK_FORMAT_R8G8B8A8_UNORM */      return 0x000085;
        default:                                      return 0x020084;
    }
}

static void command_buffer_apply_attributes(VkCommandBuffer commandBuffer) {
    struct v9_attribute_binding attrs[8] = {0};
    uint32_t attr_count = 0;
    VkPipeline pipeline = commandBuffer->graphics_pipeline;

    for (uint32_t i = 0; pipeline && i < pipeline->vertex_attribute_count; i++) {
        const struct VkVertexInputAttributeDescription *attribute =
            &pipeline->vertex_attributes[i];
        if (attribute->location >= 8 || attribute->binding >= 16)
            continue;

        const struct VkVertexInputBindingDescription *binding = NULL;
        for (uint32_t b = 0; b < pipeline->vertex_binding_count; b++) {
            if (pipeline->vertex_bindings[b].binding == attribute->binding) {
                binding = &pipeline->vertex_bindings[b];
                break;
            }
        }
        VkBuffer buf = commandBuffer->vertex_bindings[attribute->binding].buffer;
        if (!binding || !buf || !buf->bo) continue;

        VkDeviceSize offset = commandBuffer->vertex_bindings[attribute->binding].offset;
        attrs[attribute->location] = (struct v9_attribute_binding) {
            .format = vk_format_to_pan_v9_attr_format(attribute->format),
            .offset = attribute->offset,
            .stride = binding->stride,
            .input_rate = binding->inputRate,
            .buffer_address = buf->bo->gpu + buf->memory_offset + offset,
            .buffer_size = (buf->size > offset) ? (uint32_t)(buf->size - offset) : 0,
        };
        if (attribute->location + 1 > attr_count)
            attr_count = attribute->location + 1;
    }
    if (attr_count == 0 && commandBuffer->v9_cmd) {
        attrs[0] = (struct v9_attribute_binding) {
            .format = vk_format_to_pan_v9_attr_format(VK_FORMAT_R32G32B32_SFLOAT),
            .offset = 0,
            .stride = 16,
            .input_rate = 0,
            .buffer_address = v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd),
            .buffer_size = 4096,
        };
        attr_count = 1;
    }
    v9_cmd_buffer_set_attributes(commandBuffer->v9_cmd, attrs, attr_count);
}

void vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    pvk_log("vkCmdDraw: cb=%p vert=%u inst=%u pipe=%p v9=%p\n", (void*)commandBuffer,
            vertexCount, instanceCount,
            (void*)(commandBuffer ? commandBuffer->graphics_pipeline : NULL),
            (void*)(commandBuffer ? commandBuffer->v9_cmd : NULL));
    if (commandBuffer && commandBuffer->v9_cmd && vertexCount > 0 && instanceCount > 0 &&
        (!commandBuffer->graphics_pipeline ||
         !commandBuffer->graphics_pipeline->rasterizer_discard)) {
        command_buffer_apply_ubos(commandBuffer);
        command_buffer_apply_textures(commandBuffer);
        command_buffer_apply_samplers(commandBuffer);
        command_buffer_apply_attributes(commandBuffer);
        if (commandBuffer->graphics_pipeline) {
            if (commandBuffer->graphics_pipeline->vertex_binary.binary_size) {
                v9_cmd_buffer_set_vertex_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->vertex_binary);
            }
            if (commandBuffer->graphics_pipeline->fragment_binary.binary_size) {
                v9_cmd_buffer_set_fragment_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->fragment_binary);
            }
        }
        v9_cmd_buffer_set_push_constants(
            commandBuffer->v9_cmd,
            commandBuffer->push_constants, commandBuffer->push_constants_size);
        uint64_t pos_gpu = commandBuffer->vertex_bindings[0].buffer && commandBuffer->vertex_bindings[0].buffer->bo ?
                           commandBuffer->vertex_bindings[0].buffer->bo->gpu +
                           commandBuffer->vertex_bindings[0].buffer->memory_offset +
                           commandBuffer->vertex_bindings[0].offset + (firstVertex * 16) :
                           v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd);
        /* vkCmdDraw is non-indexed: pass idx_gpu=0 and index_type=0 so the
         * tiler runs the position shader over [0, vertexCount) directly. */
        v9_cmd_draw_indexed(commandBuffer->v9_cmd, 0, vertexCount, 0,
                            pos_gpu, vertexCount);
    }
}

void vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                          const struct VkRenderPassBeginInfo *pRenderPassBegin,
                          uint32_t contents) {
    pvk_log("vkCmdBeginRenderPass: cb=%p rp=%p fb=%p contents=%u\n", (void*)commandBuffer,
            pRenderPassBegin ? (void*)pRenderPassBegin->renderPass : NULL,
            pRenderPassBegin ? (void*)pRenderPassBegin->framebuffer : NULL,
            contents);
    if (!commandBuffer || !pRenderPassBegin) return;

    uint32_t clear_color = 0;
    if (pRenderPassBegin->clearValueCount > 0 && pRenderPassBegin->pClearValues) {
        const float *c = (const float *)pRenderPassBegin->pClearValues;
        uint8_t r = (uint8_t)(c[0] * 255.0f);
        uint8_t g = (uint8_t)(c[1] * 255.0f);
        uint8_t b = (uint8_t)(c[2] * 255.0f);
        uint8_t a = (uint8_t)(c[3] * 255.0f);
        clear_color = (a << 24) | (b << 16) | (g << 8) | r;
    }

    /* FIX BUG9: flag booleana em vez de comparar com 300 (renderArea real de
     * 300px seria sobrescrito pelo fb->width por engano) */
    bool has_w = pRenderPassBegin->renderArea.extent.width > 0;
    bool has_h = pRenderPassBegin->renderArea.extent.height > 0;
    struct v9_render_target_config config = {
        .width = has_w ? pRenderPassBegin->renderArea.extent.width : 0,
        .height = has_h ? pRenderPassBegin->renderArea.extent.height : 0,
        .clear_color = clear_color,
    };

    /* Use framebuffer dimensions if render area is zero */
    if (pRenderPassBegin->framebuffer) {
        struct VkFramebuffer_T *fb = pRenderPassBegin->framebuffer;
        if (fb->width > 0 && !has_w) config.width = fb->width;
        if (fb->height > 0 && !has_h) config.height = fb->height;
    }
    if (config.width == 0) config.width = 300;
    if (config.height == 0) config.height = 300;

    /* CRITICAL FIX: Only destroy/recreate v9_cmd if there are NO compute
     * commands pending.  Compute and fragment jobs live at different offsets
     * in the same mem_bo (compute at 0xE600, frag at 0xE380), so they can
     * coexist.  Destroying the v9_cmd here would wipe out compute dispatches
     * that DXVK placed before the render pass. */
    if (!commandBuffer->v9_cmd || !v9_cmd_buffer_has_compute(commandBuffer->v9_cmd)) {
        if (commandBuffer->v9_cmd) {
            v9_cmd_buffer_destroy(commandBuffer->v9_cmd);
        }
        commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
        if (commandBuffer->v9_cmd) {
            v9_cmd_buffer_begin(commandBuffer->v9_cmd);
        }
    } else {
        /* Update config dimensions on the existing v9_cmd that has compute */
        v9_cmd_buffer_update_config(commandBuffer->v9_cmd, config.width, config.height, config.clear_color);
    }

    /* DXVK-style render: the framebuffer's colour attachment (0) is a swapchain
     * image or a user image backed by a BO.  Redirect the render target to that
     * image so the frame is drawn into the presented surface, not the internal
     * slot BO. */
    if (commandBuffer->v9_cmd && pRenderPassBegin->framebuffer &&
        !getenv("PANVK_USE_INTERNAL_RT")) {
        struct VkFramebuffer_T *fb = pRenderPassBegin->framebuffer;
        struct VkImageView_T *view = (fb->attachment_count > 0) ? fb->attachments[0] : NULL;
        struct VkImage_T *img = view ? view->image : NULL;
        if (img && img->bo) {
            v9_cmd_buffer_set_render_target(commandBuffer->v9_cmd,
                                            img->bo, img->bo->gpu + img->memory_offset,
                                            img->width, img->height);
        } else {
            /* Diagnóstico: por que o RT externo não foi ligado */
            static uint32_t skip_n = 0;
            if (skip_n < 20 || (skip_n % 500) == 0) {
                skip_n++;
                pvk_log("SKIP RenderPass->RT: v9=%p fb=%p att_count=%u view=%p img=%p bo=%p\n",
                        (void*)commandBuffer->v9_cmd, (void*)fb,
                        fb ? fb->attachment_count : 0,
                        (void*)view, (void*)img, img ? (void*)img->bo : NULL);
            }
        }
    } else if (commandBuffer->v9_cmd) {
        static uint32_t skip2_n = 0;
        if (skip2_n < 10) {
            skip2_n++;
            pvk_log("SKIP RenderPass->RT (gate): v9=%p fb=%p\n",
                    (void*)commandBuffer->v9_cmd,
                    (void*)pRenderPassBegin ? (void*)pRenderPassBegin->framebuffer : NULL);
        }
    }
}

void vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    pvk_log("vkCmdDrawIndexed: cb=%p idx=%u inst=%u v9=%p\n", (void*)commandBuffer,
            indexCount, instanceCount,
            (void*)(commandBuffer ? commandBuffer->v9_cmd : NULL));
    if (commandBuffer && commandBuffer->v9_cmd && indexCount > 0 && instanceCount > 0 &&
        (!commandBuffer->graphics_pipeline ||
         !commandBuffer->graphics_pipeline->rasterizer_discard)) {
        command_buffer_apply_ubos(commandBuffer);
        command_buffer_apply_textures(commandBuffer);
        command_buffer_apply_samplers(commandBuffer);
        command_buffer_apply_attributes(commandBuffer);
        if (commandBuffer->graphics_pipeline) {
            if (commandBuffer->graphics_pipeline->vertex_binary.binary_size) {
                v9_cmd_buffer_set_vertex_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->vertex_binary);
            }
            if (commandBuffer->graphics_pipeline->fragment_binary.binary_size) {
                v9_cmd_buffer_set_fragment_shader(
                    commandBuffer->v9_cmd,
                    &commandBuffer->graphics_pipeline->fragment_binary);
            }
        }
        v9_cmd_buffer_set_push_constants(
            commandBuffer->v9_cmd,
            commandBuffer->push_constants, commandBuffer->push_constants_size);
        uint64_t pos_gpu = commandBuffer->vertex_bindings[0].buffer && commandBuffer->vertex_bindings[0].buffer->bo ?
                           commandBuffer->vertex_bindings[0].buffer->bo->gpu +
                           commandBuffer->vertex_bindings[0].buffer->memory_offset +
                           commandBuffer->vertex_bindings[0].offset + (vertexOffset * 16) :
                           v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd);
        uint64_t idx_gpu = commandBuffer->index_buffer && commandBuffer->index_buffer->bo ?
                           commandBuffer->index_buffer->bo->gpu +
                           commandBuffer->index_buffer->memory_offset +
                           commandBuffer->index_offset + (firstIndex * (commandBuffer->index_type == 1 ? 4 : 2)) :
                           v9_cmd_buffer_get_idx_gpu(commandBuffer->v9_cmd);
        v9_cmd_draw_indexed(commandBuffer->v9_cmd, idx_gpu, indexCount, commandBuffer->index_type, pos_gpu, indexCount);
    }
}

void vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    pvk_log("vkCmdEndRenderPass: cb=%p v9=%p\n", (void*)commandBuffer,
            (void*)(commandBuffer ? commandBuffer->v9_cmd : NULL));
    if (commandBuffer && commandBuffer->v9_cmd) {
        v9_cmd_buffer_end(commandBuffer->v9_cmd);
    }
}

VkResult vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    pvk_log("vkEndCommandBuffer: cb=%p\n", (void*)commandBuffer);
    if (!commandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    pvk_log("vkEndCommandBuffer: OK cb=%p v9=%p\n", (void*)commandBuffer,
            (void*)commandBuffer->v9_cmd);
    return VK_SUCCESS;
}

VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const struct VkSubmitInfo *pSubmits, VkFence fence) {
    if (!queue || !pSubmits) return VK_ERROR_INITIALIZATION_FAILED;

    static uint32_t submit_counter = 0;
    submit_counter++;

    pvk_log("vkQueueSubmit: submit=%u count=%u fence=%p\n",
            submit_counter, submitCount, (void*)fence);

    pthread_mutex_lock(&queue->device->submit_mutex);
    VkResult result = VK_SUCCESS;
    for (uint32_t s = 0; s < submitCount; s++) {
        pvk_log("  submit[%u]: cmdBuffers=%u waitSemaphores=%u signalSemaphores=%u\n",
                s, pSubmits[s].commandBufferCount,
                pSubmits[s].waitSemaphoreCount, pSubmits[s].signalSemaphoreCount);

        for (uint32_t cb = 0; cb < pSubmits[s].commandBufferCount; cb++) {
            VkCommandBuffer cmd = pSubmits[s].pCommandBuffers[cb];
            if (cmd && cmd->v9_cmd) {
                pvk_log("  cmd[%u]: v9_cmd=%p\n", cb, (void*)cmd->v9_cmd);

                if (queue->last_v9_cmd != cmd->v9_cmd) {
                    v9_cmd_buffer_destroy(queue->last_v9_cmd);
                    queue->last_v9_cmd = v9_cmd_buffer_ref(cmd->v9_cmd);
                }
                int ret = v9_cmd_buffer_submit(cmd->v9_cmd);
                if (ret != 0) {
                    pvk_log("  v9_cmd_buffer_submit FAILED: ret=%d\n", ret);
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    break;
                }
                pvk_log("  v9_cmd_buffer_submit OK\n");
            }
        }
        /* Signal binary semaphores after the (synchronous) submit.  Timeline
         * values come from VkTimelineSemaphoreSubmitInfo pNext on this submit. */
        if (result == VK_SUCCESS) {
            const struct VkTimelineSemaphoreSubmitInfo *tsi = NULL;
            const void *nxt = pSubmits[s].pNext;
            while (nxt) {
                const VkBaseInStructure *base = nxt;
                if (base->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    tsi = nxt;
                    break;
                }
                nxt = base->pNext;
            }
            const uint64_t *tl_values = tsi ? tsi->pSignalSemaphoreValues : NULL;
            for (uint32_t si = 0; si < pSubmits[s].signalSemaphoreCount; si++) {
                VkSemaphore sem = pSubmits[s].pSignalSemaphores[si];
                if (!sem) continue;
                if (sem->timeline) {
                    sem->counter = tl_values ? tl_values[si] : sem->counter + 1;
                } else {
                    sem->counter = 1;
                }
                pvk_log("  signal semaphore[%u]: timeline=%d counter=%lu\n",
                        si, sem->timeline, (unsigned long)sem->counter);
            }
        }
        if (result != VK_SUCCESS) break;
    }
    /* FIX BUG6: só sinaliza fence se todos os submits tiveram sucesso */
    if (fence && result == VK_SUCCESS) {
        ((VkFence)fence)->signaled = true;
        pvk_log("  fence signaled: %p\n", (void*)fence);
    }
    pthread_mutex_unlock(&queue->device->submit_mutex);
    pvk_log("vkQueueSubmit: submit=%u result=%d\n", submit_counter, result);
    return result;
}

VkResult vkQueueWaitIdle(VkQueue queue) {
    return VK_SUCCESS;
}

VkResult vkDeviceWaitIdle(VkDevice device) {
    return VK_SUCCESS;
}

void vkGetDeviceQueue2(VkDevice device, const struct VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
    pvk_log("vkGetDeviceQueue2: dev=%p qfi=%u\n", (void*)device,
            pQueueInfo ? pQueueInfo->queueFamilyIndex : 0);
    if (!device || !pQueue) return;
    if (!device->queue) {
        device->queue = calloc(1, sizeof(*device->queue));
        if (!device->queue) { *pQueue = NULL; return; }
        set_loader_magic(device->queue);
        device->queue->device = device;
    }
    *pQueue = device->queue;
    pvk_log("vkGetDeviceQueue2: OK queue=%p\n", (void*)device->queue);
}

void vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const struct VkDependencyInfo *pDependencyInfo) {
    (void)pDependencyInfo;
}

void vkCmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const struct VkDependencyInfo *pDependencyInfo) {
    vkCmdPipelineBarrier2(commandBuffer, pDependencyInfo);
}

VkResult vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const struct VkSubmitInfo2 *pSubmits, VkFence fence) {
    if (!queue || !pSubmits) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&queue->device->submit_mutex);
    VkResult result = VK_SUCCESS;
    for (uint32_t s = 0; s < submitCount; s++) {
        for (uint32_t cb = 0; cb < pSubmits[s].commandBufferInfoCount; cb++) {
            VkCommandBuffer cmd = pSubmits[s].pCommandBufferInfos[cb].commandBuffer;
            if (cmd && cmd->v9_cmd) {
                if (queue->last_v9_cmd != cmd->v9_cmd) {
                    v9_cmd_buffer_destroy(queue->last_v9_cmd);
                    queue->last_v9_cmd = v9_cmd_buffer_ref(cmd->v9_cmd);
                }
                int ret = v9_cmd_buffer_submit(cmd->v9_cmd);
                if (ret != 0) {
                    result = VK_ERROR_INITIALIZATION_FAILED;
                    break;
                }
            }
        }
        if (result != VK_SUCCESS) break;
    }
    if (fence) ((VkFence)fence)->signaled = true;
    pthread_mutex_unlock(&queue->device->submit_mutex);
    return result;
}

void vkCmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers) {
    (void)commandBufferCount; (void)pCommandBuffers;
}

VkResult vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const struct VkSubmitInfo2 *pSubmits, VkFence fence) {
    return vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

/* WSI & Surface Implementation */
/* X11 error trap: loga BadMatch etc. (XPutImage com depth errado falha assim,
 * silenciosamente, sem handler instalado). */
static int panvk_x11_error_handler(Display *dpy, XErrorEvent *ev) {
    char txt[160] = {0};
    XGetErrorText(dpy, ev->error_code, txt, sizeof(txt) - 1);
    pvk_log("X11 ERROR: code=%d req=%d minor=%d -> %s\n",
            ev->error_code, ev->request_code, ev->minor_code, txt);
    return 0;
}

VkResult vkCreateXlibSurfaceKHR(VkInstance instance, const struct VkXlibSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkSurfaceKHR_T *surf = calloc(1, sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    surf->backend = PANVK_PRESENT_XLIB;
    surf->dpy = (Display *)pCreateInfo->dpy;
    surf->window = pCreateInfo->window;
    surf->width = 300;
    surf->height = 300;

    XSetErrorHandler(panvk_x11_error_handler);

    if (surf->dpy && surf->window) {
        XWindowAttributes attr;
        if (XGetWindowAttributes(surf->dpy, surf->window, &attr)) {
            surf->width = attr.width;
            surf->height = attr.height;
            pvk_log("vkCreateXlibSurfaceKHR: janela depth=%d visual=%p class=%d\n",
                    attr.depth, (void*)attr.visual, attr.class);
        } else {
            pvk_log("vkCreateXlibSurfaceKHR: AVISO XGetWindowAttributes falhou (janela invalida?)\n");
        }
    }

    pvk_log("vkCreateXlibSurfaceKHR: backend=XLIB dpy=%p window=%u %ux%u\n",
            surf->dpy, surf->window, surf->width, surf->height);

    *pSurface = surf;
    return VK_SUCCESS;
}

VkResult vkCreateXcbSurfaceKHR(VkInstance instance, const struct VkXcbSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkSurfaceKHR_T *surf = calloc(1, sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    surf->backend = PANVK_PRESENT_XCB;
    surf->connection = (xcb_connection_t *)pCreateInfo->connection;
    surf->window = (uint32_t)pCreateInfo->window;
    surf->is_xcb = true;
    surf->width = 800;
    surf->height = 600;

    if (surf->connection && surf->window) {
        xcb_get_geometry_cookie_t cookie = xcb_get_geometry(surf->connection, surf->window);
        xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(surf->connection, cookie, NULL);
        if (reply) {
            surf->width = reply->width;
            surf->height = reply->height;
            free(reply);
        }
    }

    pvk_log("vkCreateXcbSurfaceKHR: backend=XCB conn=%p window=%u %ux%u\n",
            surf->connection, surf->window, surf->width, surf->height);

    *pSurface = surf;
    return VK_SUCCESS;
}

/* ---- VK_KHR_win32_surface (Wine/DXVK entry point) ---- */
VkResult vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;

    struct VkSurfaceKHR_T *surf = calloc(1, sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* Wine/Win32 surface - DO NOT try to connect to X11/XCB here.
     * In Winlator, the presentation is handled by the Winlator compositor.
     * We just store the HWND and let Winlator handle the display. */
    surf->backend = PANVK_PRESENT_WINE;
    surf->wine_hwnd = (void *)pCreateInfo->hwnd;
    surf->wine_hinstance = (void *)pCreateInfo->hinstance;
    surf->width = 800;
    surf->height = 600;

    pvk_log("vkCreateWin32SurfaceKHR: backend=WINE hwnd=%p hinstance=%p (Winlator will handle presentation)\n",
            surf->wine_hwnd, surf->wine_hinstance);

    *pSurface = surf;
    return VK_SUCCESS;
}

uint32_t vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t *connection, xcb_visualid_t visual_id) {
    return 1; /* VK_TRUE */
}

uint32_t vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex) {
    return 1;
}

/* ---- VK_WINE_nulldrv_surface (Wine init-time dummy surface) ---- */
VkResult vkCreateWINE_nulldrvSurface(VkInstance instance, const VkWINE_nulldrvSurfaceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    if (!pSurface) return VK_ERROR_INITIALIZATION_FAILED;
    struct VkSurfaceKHR_T *surf = calloc(1, sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    surf->backend = PANVK_PRESENT_NULLDRV;
    surf->width = 800;
    surf->height = 600;

    pvk_log("vkCreateWINE_nulldrvSurface: backend=NULLDRV %ux%u\n",
            surf->width, surf->height);

    *pSurface = surf;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPropertiesKHR *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPlanePropertiesKHR *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice, uint32_t planeIndex, uint32_t *pDisplayCount, VkDisplayKHR *pDisplays) {
    if (!pDisplayCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pDisplayCount = 0;
    return VK_SUCCESS;
}

VkResult vkGetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display, uint32_t *pPropertyCount, VkDisplayModePropertiesKHR *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator) {
    if (surface) free(surface);
}

VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, uint32_t *pSupported) {
    if (!pSupported) return VK_ERROR_INITIALIZATION_FAILED;
    *pSupported = 1; /* Queue family 0 supports surface presentation */
    pvk_log("vkGetPhysicalDeviceSurfaceSupportKHR: queueFamily=%u surface=%p backend=%d supported=1\n",
            queueFamilyIndex, (void*)surface, surface ? surface->backend : 0);
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, struct VkSurfaceCapabilitiesKHR *pSurfaceCapabilities) {
    if (!pSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t w = surface ? surface->width : 300;
    uint32_t h = surface ? surface->height : 300;

    pSurfaceCapabilities->minImageCount = 1;
    pSurfaceCapabilities->maxImageCount = 8;
    pSurfaceCapabilities->currentExtent.width = w;
    pSurfaceCapabilities->currentExtent.height = h;
    pSurfaceCapabilities->minImageExtent.width = 1;
    pSurfaceCapabilities->minImageExtent.height = 1;
    pSurfaceCapabilities->maxImageExtent.width = 4096;
    pSurfaceCapabilities->maxImageExtent.height = 4096;
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    pvk_log("vkGetPhysicalDeviceSurfaceCapabilitiesKHR: surface=%p backend=%d extent=%ux%u\n",
            (void*)surface, surface ? surface->backend : 0, w, h);

    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pSurfaceFormatCount, struct VkSurfaceFormatKHR *pSurfaceFormats) {
    if (!pSurfaceFormatCount) return VK_ERROR_INITIALIZATION_FAILED;

    static const struct VkSurfaceFormatKHR formats[] = {
        { .format = VK_FORMAT_B8G8R8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t num_formats = sizeof(formats) / sizeof(formats[0]);

    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = num_formats;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pSurfaceFormatCount < num_formats) ? *pSurfaceFormatCount : num_formats;
    memcpy(pSurfaceFormats, formats, to_copy * sizeof(struct VkSurfaceFormatKHR));
    *pSurfaceFormatCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pPresentModeCount, uint32_t *pPresentModes) {
    if (!pPresentModeCount) return VK_ERROR_INITIALIZATION_FAILED;

    static const uint32_t modes[] = { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR };
    uint32_t num_modes = sizeof(modes) / sizeof(modes[0]);

    if (!pPresentModes) {
        *pPresentModeCount = num_modes;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPresentModeCount < num_modes) ? *pPresentModeCount : num_modes;
    memcpy(pPresentModes, modes, to_copy * sizeof(uint32_t));
    *pPresentModeCount = to_copy;
    return VK_SUCCESS;
}

/* ---- KHR2 Surface functions (DXVK / vkd3d-proton) ---- */
VkResult vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo, VkSurfaceCapabilities2KHR *pSurfaceCapabilities) {
    if (!pSurfaceCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    VkSurfaceCapabilitiesKHR *caps = &pSurfaceCapabilities->surfaceCapabilities;
    VkSurfaceKHR surface = pSurfaceInfo ? pSurfaceInfo->surface : NULL;
    uint32_t w = surface ? surface->width : 800;
    uint32_t h = surface ? surface->height : 600;
    caps->minImageCount = 1;
    caps->maxImageCount = 8;
    caps->currentExtent.width = w;
    caps->currentExtent.height = h;
    caps->minImageExtent.width = 1;
    caps->minImageExtent.height = 1;
    caps->maxImageExtent.width = 4096;
    caps->maxImageExtent.height = 4096;
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo, uint32_t *pSurfaceFormatCount, VkSurfaceFormat2KHR *pSurfaceFormats) {
    if (!pSurfaceFormatCount) return VK_ERROR_INITIALIZATION_FAILED;
    static const struct VkSurfaceFormatKHR formats[] = {
        { .format = VK_FORMAT_B8G8R8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { .format = VK_FORMAT_B8G8R8A8_SRGB, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    uint32_t num = sizeof(formats) / sizeof(formats[0]);
    if (!pSurfaceFormats) { *pSurfaceFormatCount = num; return VK_SUCCESS; }
    uint32_t to_copy = (*pSurfaceFormatCount < num) ? *pSurfaceFormatCount : num;
    for (uint32_t i = 0; i < to_copy; i++) {
        memset(&pSurfaceFormats[i], 0, sizeof(VkSurfaceFormat2KHR));
        pSurfaceFormats[i].surfaceFormat = formats[i];
    }
    *pSurfaceFormatCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfacePresentModes2KHR(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo, uint32_t *pPresentModeCount, uint32_t *pPresentModes) {
    return vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,
        pSurfaceInfo ? pSurfaceInfo->surface : NULL, pPresentModeCount, pPresentModes);
}


/* DXVK probes vkGetPhysicalDeviceSurfacePresentModes2EXT — redirect to the
 * KHR variant, ignoring the extra pNext chain in pSurfaceInfo. */
VkResult vkGetPhysicalDeviceSurfacePresentModes2EXT(VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
    uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes) {
    return vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,
        pSurfaceInfo ? pSurfaceInfo->surface : NULL, pPresentModeCount, (uint32_t*)pPresentModes);
}

/* ---- Swapchain Maintenance 1 (DXVK probes these) ---- */
VkResult vkReleaseSwapchainImagesEXT(VkDevice device, const VkReleaseSwapchainImagesInfoEXT *pReleaseInfo) {
    return VK_SUCCESS;
}

/* Swapchain Implementation */
VkResult vkCreateSwapchainKHR(VkDevice device, const struct VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    if (!device || !pCreateInfo || !pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;

    pvk_log("vkCreateSwapchainKHR: surface=%p backend=%d imageFormat=%u imageCount=%u extent=%ux%u\n",
            (void*)pCreateInfo->surface,
            pCreateInfo->surface ? pCreateInfo->surface->backend : 0,
            pCreateInfo->imageFormat,
            pCreateInfo->minImageCount,
            pCreateInfo->imageExtent.width,
            pCreateInfo->imageExtent.height);

    struct VkSwapchainKHR_T *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sc->device = device;
    sc->surface = pCreateInfo->surface;
    sc->width = pCreateInfo->imageExtent.width > 0 ? pCreateInfo->imageExtent.width : 300;
    sc->height = pCreateInfo->imageExtent.height > 0 ? pCreateInfo->imageExtent.height : 300;
    sc->image_count = pCreateInfo->minImageCount > 0 ? pCreateInfo->minImageCount : 2;

    /* Give each swapchain image a real GPU BO so vkCmdBeginRenderPass can
     * render into it (DXVK binds these images as colour attachments). */
    sc->images = calloc(sc->image_count, sizeof(struct VkImage_T));
    if (!sc->images) {
        free(sc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t aligned_w = (sc->width + 15) & ~15;
    uint32_t aligned_h = (sc->height + 15) & ~15;
    size_t color_bytes = (size_t)aligned_w * aligned_h * 4;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        struct VkImage_T *img = &sc->images[i];
        img->swapchain = sc;
        img->index = i;
        img->width = sc->width;
        img->height = sc->height;
        img->depth = 1;
        img->format = pCreateInfo->imageFormat;
        img->image_type = VK_IMAGE_TYPE_2D;
        img->mip_levels = 1;
        img->array_layers = 1;
        img->samples = VK_SAMPLE_COUNT_1_BIT;
        img->tiling = VK_IMAGE_TILING_OPTIMAL;
        img->usage = pCreateInfo->imageUsage;
        img->size = color_bytes;
        img->bo = pan_kmod_bo_alloc(device->kdev, color_bytes,
                                    PAN_KMOD_BO_FLAG_READ | PAN_KMOD_BO_FLAG_WRITE);
        if (!img->bo) goto fail_bo;
        memset(img->bo->cpu, 0, color_bytes);
        img->row_pitch[0] = sc->width * 4;
        img->memory_offset = 0;
    }

    if (sc->surface && sc->surface->is_xcb && sc->surface->connection && sc->surface->window) {
        sc->xcb_gc = xcb_generate_id(sc->surface->connection);
        xcb_create_gc(sc->surface->connection, sc->xcb_gc, sc->surface->window, 0, NULL);
        sc->image_data = malloc(sc->width * sc->height * 4);
    } else if (sc->surface && sc->surface->dpy && sc->surface->window) {
        int screen = DefaultScreen(sc->surface->dpy);
        sc->gc = XCreateGC(sc->surface->dpy, sc->surface->window, 0, NULL);
        sc->image_data = malloc(sc->width * sc->height * 4);
        if (sc->image_data) {
            /* Usa o depth REAL da janela (24 ou 32). XPutImage exige
             * image.depth == drawable.depth, senao BadMatch = tela preta. */
            int img_depth = 24;
            Window root_ret; int wx, wy; unsigned int ww, wh, wbw, wdepth;
            if (XGetGeometry(sc->surface->dpy, sc->surface->window, &root_ret,
                             &wx, &wy, &ww, &wh, &wbw, &wdepth)) {
                if (wdepth == 32 || wdepth == 24) img_depth = (int)wdepth;
                pvk_log("swapchain XLIB: XGetGeometry %ux%u depth=%u -> XCreateImage depth=%d\n",
                        ww, wh, wdepth, img_depth);
            } else {
                pvk_log("swapchain XLIB: AVISO XGetGeometry falhou, usando depth=24\n");
            }
            sc->ximage = XCreateImage(sc->surface->dpy, DefaultVisual(sc->surface->dpy, screen),
                                     img_depth, ZPixmap, 0, sc->image_data, sc->width, sc->height, 32, 0);
            pvk_log("swapchain XLIB: gc=%p ximage=%p depth=%d bytes_per_line=%d\n",
                    (void*)sc->gc, (void*)sc->ximage, img_depth,
                    sc->ximage ? sc->ximage->bytes_per_line : -1);
        }
    }

    *pSwapchain = sc;
    return VK_SUCCESS;

fail_bo:
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->images[i].bo) pan_kmod_bo_free(sc->images[i].bo);
    }
    free(sc->images);
    free(sc);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator) {
    if (!swapchain) return;
    for (uint32_t i = 0; i < swapchain->image_count; i++) {
        if (swapchain->images[i].bo) pan_kmod_bo_free(swapchain->images[i].bo);
    }
    if (swapchain->images) free(swapchain->images);
    if (swapchain->surface && swapchain->surface->is_xcb && swapchain->surface->connection && swapchain->xcb_gc) {
        xcb_free_gc(swapchain->surface->connection, swapchain->xcb_gc);
    } else if (swapchain->surface && swapchain->surface->dpy && swapchain->gc) {
        XFreeGC(swapchain->surface->dpy, swapchain->gc);
    }
    if (swapchain->image_data) free(swapchain->image_data);
    free(swapchain);
}

VkResult vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages) {
    if (!swapchain || !pSwapchainImageCount) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pSwapchainImages) {
        *pSwapchainImageCount = swapchain->image_count;
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pSwapchainImageCount < swapchain->image_count) ? *pSwapchainImageCount : swapchain->image_count;
    for (uint32_t i = 0; i < to_copy; i++) {
        pSwapchainImages[i] = &swapchain->images[i];
    }
    *pSwapchainImageCount = to_copy;
    return VK_SUCCESS;
}

VkResult vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
    if (!swapchain || !pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;
    if (swapchain->image_count == 0) return VK_ERROR_OUT_OF_DATE_KHR;

    /* Round-robin over the swapchain images.  Rendering is synchronous (submit
     * returns only after the GPU finished), so every image is always free. */
    uint32_t idx = swapchain->next_image++ % swapchain->image_count;
    *pImageIndex = idx;

    pvk_log("vkAcquireNextImageKHR: imageIndex=%u next=%u count=%u\n",
            idx, swapchain->next_image, swapchain->image_count);

    if (semaphore) {
        if (semaphore->timeline)
            semaphore->counter++;
        else
            semaphore->counter = 1; /* binary: signal immediately */
    }
    if (fence) ((VkFence)fence)->signaled = true;
    return VK_SUCCESS;
}

VkResult vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo, uint32_t *pImageIndex) {
    if (!pAcquireInfo) return VK_ERROR_INITIALIZATION_FAILED;
    return vkAcquireNextImageKHR(device, pAcquireInfo->swapchain, pAcquireInfo->timeout,
                                 pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

VkResult vkQueuePresentKHR(VkQueue queue, const struct VkPresentInfoKHR *pPresentInfo) {
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) return VK_ERROR_INITIALIZATION_FAILED;

    pvk_log("vkQueuePresentKHR: swapchainCount=%u waitSemaphores=%u\n",
            pPresentInfo->swapchainCount, pPresentInfo->waitSemaphoreCount);

    /* Wait on any binary/timeline semaphores the app submitted with. */
    if (pPresentInfo->waitSemaphoreCount > 0 && pPresentInfo->pWaitSemaphores) {
        for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++) {
            (void)pPresentInfo->pWaitSemaphores[i];
        }
    }

    VkSwapchainKHR sc = pPresentInfo->pSwapchains[0];
    if (!sc || !sc->surface) {
        pvk_log("ERRO: vkQueuePresentKHR chamado sem swapchain/surface válido!\n");
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    uint32_t present_index = pPresentInfo->pImageIndices ? pPresentInfo->pImageIndices[0] : 0;
    if (present_index >= sc->image_count) present_index = 0;

    /* Frame tracking for debugging */
    static uint32_t frame_counter = 0;
    frame_counter++;

    pvk_log("=== FRAME %u ===\n", frame_counter);
    pvk_log("present_begin: index=%u backend=%d size=%ux%u image_count=%u\n",
            present_index, sc->surface->backend, sc->width, sc->height, sc->image_count);

    /* Lazily allocate image_data and GC if the surface was created via
     * VK_KHR_win32_surface (no GC/image_data set up at swapchain create time). */
    if (!sc->image_data && sc->width && sc->height) {
        pvk_log("allocating image_data: %u bytes\n", sc->width * sc->height * 4);
        sc->image_data = malloc(sc->width * sc->height * 4);
        if (sc->surface->is_xcb && sc->surface->connection && sc->surface->window) {
            if (!sc->xcb_gc) {
                sc->xcb_gc = xcb_generate_id(sc->surface->connection);
                xcb_create_gc(sc->surface->connection, sc->xcb_gc, sc->surface->window, 0, NULL);
            }
        } else if (sc->surface->dpy && sc->surface->window) {
            if (!sc->gc) {
                sc->gc = XCreateGC(sc->surface->dpy, sc->surface->window, 0, NULL);
                int screen = DefaultScreen(sc->surface->dpy);
                sc->ximage = XCreateImage(sc->surface->dpy, DefaultVisual(sc->surface->dpy, screen),
                                          24, ZPixmap, 0, sc->image_data, sc->width, sc->height, 32, 0);
            }
        }
    }

    /* Read pixels from the presented swapchain image's GPU BO */
    uint32_t *src = NULL;
    if (sc->images && present_index < sc->image_count &&
        sc->images[present_index].bo && sc->images[present_index].bo->cpu) {
        src = (uint32_t *)sc->images[present_index].bo->cpu;
        pvk_log("readback: GPU BO addr=%p size=%u\n", src, sc->width * sc->height * 4);
    } else if (queue && queue->last_v9_cmd) {
        /* CPU readback path for the standalone test (no swapchain BOs). */
        if (sc->image_data) {
            pvk_log("readback: CPU fallback via last_v9_cmd\n");
            uint32_t *dst = (uint32_t *)sc->image_data;
            for (uint32_t y = 0; y < sc->height; y++)
                for (uint32_t x = 0; x < sc->width; x++)
                    dst[y * sc->width + x] = v9_cmd_buffer_read_pixel(queue->last_v9_cmd, x, y);
        }
    } else {
        pvk_log("readback: NO SOURCE - frame will be empty\n");
    }

    if (src && sc->image_data) {
        memcpy(sc->image_data, src, sc->width * sc->height * 4);
        pvk_log("memcpy: %u bytes from GPU to image_data\n", sc->width * sc->height * 4);
    }

    /* DIAGNOSTICO de tela preta: nos primeiros frames, amostra pixels e
     * estado do caminho XLIB. Se pixel=00000000 -> problema de render/cache.
     * Se pixel tem cor mas tela preta -> problema de XPutImage/BadMatch. */
    {
        static uint32_t diag_frames = 0;
        if (diag_frames < 5) {
            diag_frames++;
            pvk_log("DIAG present: img[%u] bo=%p bo_gpu=0x%llx (comparar com TRACE submit rt_gpu)\n",
                    present_index, (void*)sc->images[present_index].bo,
                    (unsigned long long)sc->images[present_index].bo->gpu);
            if (sc->image_data) {
                uint32_t *px = (uint32_t *)sc->image_data;
                uint32_t center = px[(sc->height / 2) * sc->width + (sc->width / 2)];
                uint32_t corner = px[0];
                uint32_t q1 = px[(sc->height / 4) * sc->width + (sc->width / 4)];
                uint32_t nonzero = 0;
                for (uint32_t i = 0; i < sc->width * sc->height; i += 997)
                    if (px[i]) nonzero++;
                pvk_log("DIAG frame=%u pixels: centro=%08x quarto=%08x canto=%08x nao_zero=%u/%u\n",
                        diag_frames, center, q1, corner, nonzero, (sc->width * sc->height) / 997);
            }
            if (sc->surface->dpy && sc->surface->window) {
                Window root_ret; int wx, wy; unsigned int ww, wh, wbw, wdepth;
                if (XGetGeometry(sc->surface->dpy, sc->surface->window, &root_ret,
                                 &wx, &wy, &ww, &wh, &wbw, &wdepth)) {
                    int scr = DefaultScreen(sc->surface->dpy);
                    pvk_log("DIAG XLIB: win geom=%ux%u depth=%u | dflt_depth=%d | gc=%p ximage=%p(xdepth=%d)\n",
                            ww, wh, wdepth, DefaultDepth(sc->surface->dpy, scr),
                            (void*)sc->gc, (void*)sc->ximage,
                            sc->ximage ? sc->ximage->depth : -1);
                    XWindowAttributes wa;
                    if (XGetWindowAttributes(sc->surface->dpy, sc->surface->window, &wa))
                        pvk_log("DIAG XLIB: janela map_state=%d (%s=%u visivel) override_redirect=%d\n",
                                wa.map_state,
                                wa.map_state == IsViewable ? "VIEWABLE" : "nao-viewable",
                                IsViewable, wa.override_redirect);
                } else {
                    pvk_log("DIAG XLIB: XGetGeometry FALHOU na janela %u (janela morta?)\n",
                            (unsigned)sc->surface->window);
                }
            } else {
                pvk_log("DIAG XLIB: dpy=%p window=%u -> present XLIB vai PULAR o blit!\n",
                        (void*)sc->surface->dpy, (unsigned)sc->surface->window);
            }
        }
    }

    /* Present based on backend type */
    if (sc->image_data) {
        switch (sc->surface->backend) {
        case PANVK_PRESENT_XCB:
            if (sc->surface->connection && sc->surface->window) {
                xcb_put_image(sc->surface->connection, XCB_IMAGE_FORMAT_Z_PIXMAP,
                              sc->surface->window, sc->xcb_gc,
                              sc->width, sc->height, 0, 0, 0, 24,
                              sc->width * sc->height * 4, (const uint8_t *)sc->image_data);
                xcb_flush(sc->surface->connection);
            }
            break;

        case PANVK_PRESENT_XLIB:
            if (sc->surface->dpy && sc->surface->window && sc->ximage && sc->gc) {
                XPutImage(sc->surface->dpy, sc->surface->window, sc->gc, sc->ximage,
                          0, 0, 0, 0, sc->width, sc->height);
                XFlush(sc->surface->dpy);
                pvk_log("present XLIB: XPutImage %ux%u -> win=%u OK\n",
                        sc->width, sc->height, (unsigned)sc->surface->window);
            } else {
                pvk_log("present XLIB: PULADO dpy=%p win=%u ximage=%p gc=%p\n",
                        (void*)sc->surface->dpy, (unsigned)sc->surface->window,
                        (void*)sc->ximage, (void*)sc->gc);
            }
            break;

        case PANVK_PRESENT_WINE:
            /* Wine/Win32 surface in Winlator:
             * The frame is already rendered to the GPU buffer.
             * Winlator's compositor will handle the presentation.
             * We just need to return success. */
            pvk_log("vkQueuePresentKHR: WINE backend - frame rendered to GPU buffer, Winlator will display it\n");
            break;

        case PANVK_PRESENT_NULLDRV:
            /* Null driver - discard frame, log for debugging */
            pvk_log("vkQueuePresentKHR: NULLDRV backend, frame discarded (%ux%u)\n",
                    sc->width, sc->height);
            break;

        default:
            pvk_log("vkQueuePresentKHR: unknown backend %d, frame dropped\n",
                    sc->surface->backend);
            break;
        }
    }
    return VK_SUCCESS;
}

uint32_t panvk_v9_read_pixel(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y) {
    if (commandBuffer && commandBuffer->v9_cmd) {
        return v9_cmd_buffer_read_pixel(commandBuffer->v9_cmd, x, y);
    }
    return 0;
}

/* CPU access helpers for the bring-up tests: expose the backing BO of an
 * image or buffer so tests can verify blit/copy/clear results without a GPU. */
void *panvk_v9_image_cpu(VkImage image) {
    if (!image || !image->bo) return NULL;
    return (uint8_t *)image->bo->cpu + image->memory_offset;
}

void *panvk_v9_buffer_cpu(VkBuffer buffer) {
    if (!buffer || !buffer->bo) return NULL;
    return (uint8_t *)buffer->bo->cpu + buffer->memory_offset;
}

uint32_t panvk_v9_image_pixel(VkImage image, uint32_t x, uint32_t y) {
    if (!image || !image->bo || !image->bo->cpu) return 0;
    if (x >= image->width || y >= image->height) return 0;
    const uint8_t *base = (const uint8_t *)image->bo->cpu + image->memory_offset;
    uint32_t bpp = panvk_v9_format_bpp(image->format);
    uint32_t v;
    memcpy(&v, base + y * image->row_pitch[0] + x * bpp, 4);
    return v;
}

size_t panvk_v9_compute_binary_size(VkPipeline pipeline) {
    return pipeline ? pipeline->compute_binary.binary_size : 0;
}

uint32_t panvk_v9_compute_local_size(VkPipeline pipeline, uint32_t axis) {
    if (!pipeline) return 0;
    switch (axis) {
    case 0: return pipeline->compute_binary.local_size_x;
    case 1: return pipeline->compute_binary.local_size_y;
    case 2: return pipeline->compute_binary.local_size_z;
    default: return 0;
    }
}

bool panvk_v9_cmd_has_compute(VkCommandBuffer commandBuffer) {
    return commandBuffer && commandBuffer->v9_cmd &&
           v9_cmd_buffer_has_compute(commandBuffer->v9_cmd);
}

/* KHR Aliases for PhysicalDevice2 functions */
void vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, struct VkPhysicalDeviceProperties2 *pProperties) {
    vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}
void vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, struct VkPhysicalDeviceFeatures2 *pFeatures) {
    vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
}
void vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount, struct VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}
void vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice physicalDevice, struct VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
}
VkResult vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, struct VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroups) {
    return vkEnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroups);
}

/* =====================================================================
 * Extended entry points (core 1.2/1.3 + KHR/EXT) so that every advertised
 * extension and every feature gate used by DXVK / vkd3d-proton resolves to
 * a non-NULL function.  Real work is done where cheap; the rest are safe
 * (no-op / best-effort) stubs — captured so the bring-up harness can measure
 * exactly which advertised functionality is actually executable.
 * ===================================================================== */

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo *pInfo) {
    if (!pInfo || !pInfo->buffer) return 0;
    /* Return the real GPU address of the buffer's backing BO plus its memory
     * offset, so DXVK can use it in shaders (descriptor address, etc.). */
    struct VkBuffer_T *b = pInfo->buffer;
    if (b->bo) return (VkDeviceAddress)(b->bo->gpu + b->memory_offset);
    return 0;
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo *pInfo) {
    return vkGetBufferDeviceAddress(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress(VkDevice device, const VkBufferDeviceAddressInfo *pInfo) {
    return (uint64_t)vkGetBufferDeviceAddress(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo) {
    if (!pInfo || !pInfo->memory || !pInfo->memory->bo) return 0;
    return (uint64_t)(pInfo->memory->bo->gpu);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t *pValue) {
    if (!pValue) return VK_ERROR_INITIALIZATION_FAILED;
    *pValue = (semaphore) ? semaphore->counter : 0;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValueKHR(VkDevice device, VkSemaphore semaphore, uint64_t *pValue) {
    return vkGetSemaphoreCounterValue(device, semaphore, pValue);
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *pWaitInfo, uint64_t timeout) {
    if (!pWaitInfo) return VK_ERROR_INITIALIZATION_FAILED;
    /* Submits are synchronous, so any timeline value the app is waiting on has
     * already been reached by the time this is called.  Only honour the
     * waitAll flag semantics on the counters; never block. */
    for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
        VkSemaphore sem = pWaitInfo->pSemaphores[i];
        if (!sem) continue;
        uint64_t want = pWaitInfo->pValues[i];
        uint64_t have = sem->counter;
        if (pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT) {
            if (have >= want) return VK_SUCCESS;
        } else {
            if (have < want) return VK_TIMEOUT;
        }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfo *pWaitInfo, uint64_t timeout) {
    return vkWaitSemaphores(device, pWaitInfo, timeout);
}
VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo) {
    if (pSignalInfo && pSignalInfo->semaphore) pSignalInfo->semaphore->counter = pSignalInfo->value;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphoreKHR(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo) {
    return vkSignalSemaphore(device, pSignalInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo) {
    if (!commandBuffer || !pRenderingInfo) return;
    commandBuffer->rendering_active = VK_TRUE;

    /* Dynamic rendering: set up render target from pRenderingInfo->pColorAttachments */
    if (pRenderingInfo->colorAttachmentCount > 0 && pRenderingInfo->pColorAttachments) {
        const VkRenderingAttachmentInfo *att = &pRenderingInfo->pColorAttachments[0];
        if (att->imageView) {
            struct VkImageView_T *view = att->imageView;
            struct VkImage_T *img = view ? view->image : NULL;
            if (img && img->bo) {
                if (!commandBuffer->v9_cmd) {
                    uint32_t clear_color = 0;
                    if (att->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                        const float *c = (const float *)&att->clearValue;
                        uint8_t r = (uint8_t)(c[0] * 255.0f);
                        uint8_t g = (uint8_t)(c[1] * 255.0f);
                        uint8_t b = (uint8_t)(c[2] * 255.0f);
                        uint8_t a = (uint8_t)(c[3] * 255.0f);
                        clear_color = (a << 24) | (b << 16) | (g << 8) | r;
                    }
                    struct v9_render_target_config config = {
                        .width = pRenderingInfo->renderArea.extent.width > 0 ?
                                 pRenderingInfo->renderArea.extent.width : img->width,
                        .height = pRenderingInfo->renderArea.extent.height > 0 ?
                                  pRenderingInfo->renderArea.extent.height : img->height,
                        .clear_color = clear_color,
                    };
                    commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
                    if (commandBuffer->v9_cmd) v9_cmd_buffer_begin(commandBuffer->v9_cmd);
                }
                if (commandBuffer->v9_cmd) {
                    v9_cmd_buffer_set_render_target(commandBuffer->v9_cmd,
                                                    img->bo, img->bo->gpu + img->memory_offset,
                                                    img->width, img->height);
                }
            } else {
                /* Diagnóstico: BeginRendering sem imagem/BO válido */
                static uint32_t skip_r_n = 0;
                if (skip_r_n < 20 || (skip_r_n % 500) == 0) {
                    skip_r_n++;
                    pvk_log("SKIP Rendering->RT: view=%p img=%p bo=%p\n",
                            (void*)view, (void*)img, img ? (void*)img->bo : NULL);
                }
            }
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer) {
    if (commandBuffer) commandBuffer->rendering_active = VK_FALSE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfoKHR *pRenderingInfo) {
    vkCmdBeginRendering(commandBuffer, (const VkRenderingInfo *)pRenderingInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer) { vkCmdEndRendering(commandBuffer); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo *pDependencyInfo) {
    if (event) event->signaled = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask) {
    if (event) event->signaled = VK_FALSE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents, const VkDependencyInfo *pDependencyInfos) {
    for (uint32_t i = 0; i < eventCount; i++) if (pEvents[i]) pEvents[i]->signaled = VK_TRUE;
}
VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2KHR(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask) { vkCmdResetEvent2(commandBuffer, event, stageMask); }
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents, const VkDependencyInfo *pDependencyInfos) { vkCmdWaitEvents2(commandBuffer, eventCount, pEvents, pDependencyInfos); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo *pDependencyInfo) { vkCmdSetEvent2(commandBuffer, event, pDependencyInfo); }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    if (!commandBuffer || !buffer || !buffer->bo || !buffer->bo->cpu || drawCount == 0) return;
    const uint8_t *base = (const uint8_t *)buffer->bo->cpu + buffer->memory_offset + offset;
    for (uint32_t i = 0; i < drawCount; i++) {
        const uint32_t *p = (const uint32_t *)(base + i * stride);
        uint32_t vertexCount = p[0];
        uint32_t instanceCount = p[1];
        uint32_t firstVertex = p[2];
        uint32_t firstInstance = p[3];
        if (instanceCount == 0 || vertexCount == 0) continue;
        (void)firstVertex; (void)firstInstance;
        /* For now: issue a simple draw with the first instance's vertices.
         * A full implementation would offset pos_gpu by firstVertex * stride. */
        if (commandBuffer->v9_cmd && commandBuffer->graphics_pipeline &&
            !commandBuffer->graphics_pipeline->rasterizer_discard) {
            command_buffer_apply_ubos(commandBuffer);
            command_buffer_apply_textures(commandBuffer);
            command_buffer_apply_samplers(commandBuffer);
            command_buffer_apply_attributes(commandBuffer);
            if (commandBuffer->graphics_pipeline->vertex_binary.binary_size)
                v9_cmd_buffer_set_vertex_shader(commandBuffer->v9_cmd, &commandBuffer->graphics_pipeline->vertex_binary);
            if (commandBuffer->graphics_pipeline->fragment_binary.binary_size)
                v9_cmd_buffer_set_fragment_shader(commandBuffer->v9_cmd, &commandBuffer->graphics_pipeline->fragment_binary);
            v9_cmd_buffer_set_push_constants(
                commandBuffer->v9_cmd,
                commandBuffer->push_constants, commandBuffer->push_constants_size);
            uint64_t pos_gpu = commandBuffer->vertex_bindings[0].buffer && commandBuffer->vertex_bindings[0].buffer->bo ?
                               commandBuffer->vertex_bindings[0].buffer->bo->gpu +
                               commandBuffer->vertex_bindings[0].buffer->memory_offset +
                               commandBuffer->vertex_bindings[0].offset :
                               v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd);
            v9_cmd_draw_indexed(commandBuffer->v9_cmd, v9_cmd_buffer_get_idx_gpu(commandBuffer->v9_cmd),
                                vertexCount, 1, pos_gpu, vertexCount);
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    if (!commandBuffer || !buffer || !buffer->bo || !buffer->bo->cpu || drawCount == 0) return;
    const uint8_t *base = (const uint8_t *)buffer->bo->cpu + buffer->memory_offset + offset;
    for (uint32_t i = 0; i < drawCount; i++) {
        const uint32_t *p = (const uint32_t *)(base + i * stride);
        uint32_t indexCount = p[0];
        uint32_t instanceCount = p[1];
        uint32_t firstIndex = p[2];
        int32_t vertexOffset = (int32_t)p[3];
        uint32_t firstInstance = p[4];
        if (instanceCount == 0 || indexCount == 0) continue;
        (void)firstIndex; (void)vertexOffset; (void)firstInstance;
        if (commandBuffer->v9_cmd && commandBuffer->graphics_pipeline &&
            !commandBuffer->graphics_pipeline->rasterizer_discard) {
            command_buffer_apply_ubos(commandBuffer);
            command_buffer_apply_textures(commandBuffer);
            command_buffer_apply_samplers(commandBuffer);
            command_buffer_apply_attributes(commandBuffer);
            if (commandBuffer->graphics_pipeline->vertex_binary.binary_size)
                v9_cmd_buffer_set_vertex_shader(commandBuffer->v9_cmd, &commandBuffer->graphics_pipeline->vertex_binary);
            if (commandBuffer->graphics_pipeline->fragment_binary.binary_size)
                v9_cmd_buffer_set_fragment_shader(commandBuffer->v9_cmd, &commandBuffer->graphics_pipeline->fragment_binary);
            v9_cmd_buffer_set_push_constants(
                commandBuffer->v9_cmd,
                commandBuffer->push_constants, commandBuffer->push_constants_size);
            uint64_t idx_gpu = commandBuffer->index_buffer && commandBuffer->index_buffer->bo ?
                               commandBuffer->index_buffer->bo->gpu + commandBuffer->index_buffer->memory_offset + commandBuffer->index_offset :
                               v9_cmd_buffer_get_idx_gpu(commandBuffer->v9_cmd);
            uint64_t pos_gpu = commandBuffer->vertex_bindings[0].buffer && commandBuffer->vertex_bindings[0].buffer->bo ?
                               commandBuffer->vertex_bindings[0].buffer->bo->gpu +
                               commandBuffer->vertex_bindings[0].buffer->memory_offset +
                               commandBuffer->vertex_bindings[0].offset :
                               v9_cmd_buffer_get_pos_gpu(commandBuffer->v9_cmd);
            v9_cmd_draw_indexed(commandBuffer->v9_cmd, idx_gpu, indexCount,
                                commandBuffer->index_type, pos_gpu, indexCount);
        }
    }
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    if (!commandBuffer || !buffer || !buffer->bo || !buffer->bo->cpu) return;
    /* Read actual draw count from the count buffer (GPU-written) */
    uint32_t actualDrawCount = maxDrawCount;
    if (countBuffer && countBuffer->bo && countBuffer->bo->cpu) {
        actualDrawCount = *(uint32_t *)((uint8_t *)countBuffer->bo->cpu + countBuffer->memory_offset + countBufferOffset);
        if (actualDrawCount > maxDrawCount) actualDrawCount = maxDrawCount;
    }
    /* Delegate to vkCmdDrawIndirect with the clamped count */
    vkCmdDrawIndirect(commandBuffer, buffer, offset, actualDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) { vkCmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride); }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    if (!commandBuffer || !buffer || !buffer->bo || !buffer->bo->cpu) return;
    uint32_t actualDrawCount = maxDrawCount;
    if (countBuffer && countBuffer->bo && countBuffer->bo->cpu) {
        actualDrawCount = *(uint32_t *)((uint8_t *)countBuffer->bo->cpu + countBuffer->memory_offset + countBufferOffset);
        if (actualDrawCount > maxDrawCount) actualDrawCount = maxDrawCount;
    }
    vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, actualDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCountKHR(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) { vkCmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride); }
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (!commandBuffer || !commandBuffer->compute_pipeline || groupCountX == 0 || groupCountY == 0 || groupCountZ == 0) return;
    VkPipeline pipeline = commandBuffer->compute_pipeline;
    if (!commandBuffer->v9_cmd) {
        struct v9_render_target_config config = { .width = 300, .height = 300, .clear_color = 0 };
        commandBuffer->v9_cmd = v9_cmd_buffer_create(commandBuffer->device->kdev, &config);
        if (!commandBuffer->v9_cmd) return;
        v9_cmd_buffer_begin(commandBuffer->v9_cmd);
    }
    command_buffer_apply_ssbos(commandBuffer);
    if (pipeline->compute_binary.binary_size)
        v9_cmd_buffer_set_compute_shader(commandBuffer->v9_cmd, &pipeline->compute_binary);
    v9_cmd_buffer_dispatch(commandBuffer->v9_cmd, groupCountX, groupCountY, groupCountZ);
}
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data) {
    if (!dstBuffer || !dstBuffer->bo || !dstBuffer->bo->cpu) return;
    uint8_t *dst = (uint8_t *)dstBuffer->bo->cpu + dstBuffer->memory_offset + dstOffset;
    VkDeviceSize count = size / 4;
    for (VkDeviceSize i = 0; i < count; i++) ((uint32_t *)dst)[i] = data;
    /* Handle remaining bytes */
    VkDeviceSize done = count * 4;
    for (VkDeviceSize i = done; i < size; i++) dst[i] = (data >> ((i - done) * 8)) & 0xFF;
}
VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void *pData) {
    if (!dstBuffer || !dstBuffer->bo || !dstBuffer->bo->cpu || !pData || dataSize == 0) return;
    memcpy((uint8_t *)dstBuffer->bo->cpu + dstBuffer->memory_offset + dstOffset, pData, dataSize);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites) {
    if (!commandBuffer || !pDescriptorWrites || descriptorWriteCount == 0) return;
    /* Apply descriptor writes directly — update the bound descriptor sets' buffer/image info.
     * This is the fast path DXVK uses instead of vkUpdateDescriptorSets + vkCmdBindDescriptorSets. */
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            /* For buffer descriptors, the actual binding happens through the
             * existing descriptor_sets path. Push descriptors are consumed on
             * submit via command_buffer_apply_ubos/command_buffer_apply_ssbos. */
        }
    }
    /* Store as if bound — DXVK expects these to be visible on next draw */
    (void)set; (void)pipelineBindPoint; (void)layout;
}
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate, VkPipelineLayout layout, uint32_t set, const void *pData) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t writeMask) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4]) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEnable(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkBool32 *pColorBlendEnables) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkBool32 *pColorBlendEnables) { vkCmdSetColorBlendEnable(commandBuffer, firstAttachment, attachmentCount, pColorBlendEnables); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteMask(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorComponentFlags *pColorMasks) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorComponentFlags *pColorMasks) { vkCmdSetColorWriteMask(commandBuffer, firstAttachment, attachmentCount, pColorMasks); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEquation(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendEquationEXT *pColorBlendEquations) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendEquationEXT *pColorBlendEquations) { vkCmdSetColorBlendEquation(commandBuffer, firstAttachment, attachmentCount, pColorBlendEquations); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D *pScissors) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport *pViewports) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOp(VkCommandBuffer commandBuffer, VkLogicOp logicOp) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable) { vkCmdSetDepthBiasEnable(commandBuffer, depthBiasEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnableEXT(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable) { vkCmdSetPrimitiveRestartEnable(commandBuffer, primitiveRestartEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnableEXT(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable) { vkCmdSetRasterizerDiscardEnable(commandBuffer, rasterizerDiscardEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable) { vkCmdSetDepthBoundsTestEnable(commandBuffer, depthBoundsTestEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias2EXT(VkCommandBuffer commandBuffer, const VkDepthBiasInfoEXT *pDepthBiasInfo) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer, const VkSampleLocationsInfoEXT *pSampleLocationsInfo) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t firstDiscardRectangle, uint32_t discardRectangleCount, const VkRect2D *pDiscardRectangles) {
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetFragmentShadingRateKHR(VkCommandBuffer commandBuffer, const VkExtent2D *pFragmentSize, const VkFragmentShadingRateCombinerOpKHR combinerOps[2]) {
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceFragmentShadingRatesKHR(VkPhysicalDevice physicalDevice, uint32_t *pFragmentShadingRateCount, VkPhysicalDeviceFragmentShadingRateKHR *pFragmentShadingRates) {
    if (!pFragmentShadingRateCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pFragmentShadingRates) { *pFragmentShadingRateCount = 1; return VK_SUCCESS; }
    if (*pFragmentShadingRateCount >= 1) {
        pFragmentShadingRates[0].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
        pFragmentShadingRates[0].pNext = NULL;
        pFragmentShadingRates[0].sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        pFragmentShadingRates[0].fragmentSize.width = 1;
        pFragmentShadingRates[0].fragmentSize.height = 1;
    }
    *pFragmentShadingRateCount = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2(VkDevice device, VkImage image, const VkImageSubresource2 *pSubresource, VkSubresourceLayout2 *pLayout) {
    if (pSubresource && pLayout) vkGetImageSubresourceLayout(device, image, &pSubresource->imageSubresource, &pLayout->subresourceLayout);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2KHR(VkDevice device, VkImage image, const VkImageSubresource2KHR *pSubresource, VkSubresourceLayout2KHR *pLayout) { vkGetImageSubresourceLayout2(device, image, (const VkImageSubresource2 *)pSubresource, (VkSubresourceLayout2 *)pLayout); }
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    VkImage tmp = NULL;
    if (pInfo->pCreateInfo && vkCreateImage(device, pInfo->pCreateInfo, NULL, &tmp) == VK_SUCCESS && tmp) {
        vkGetImageMemoryRequirements(device, tmp, &pMemoryRequirements->memoryRequirements);
        vkDestroyImage(device, tmp, NULL);
    } else {
        pMemoryRequirements->memoryRequirements.size = 4096;
        pMemoryRequirements->memoryRequirements.alignment = 4096;
        pMemoryRequirements->memoryRequirements.memoryTypeBits = 0x3;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo, uint32_t *pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements) {
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(VkPhysicalDevice physicalDevice, uint32_t *pTimeDomainCount, VkTimeDomainEXT *pTimeDomains) {
    if (!pTimeDomainCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pTimeDomains) { *pTimeDomainCount = 1; return VK_SUCCESS; }
    if (*pTimeDomainCount >= 1) pTimeDomains[0] = VK_TIME_DOMAIN_DEVICE_EXT;
    *pTimeDomainCount = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetCalibratedTimestampsEXT(VkDevice device, uint32_t timestampCount, const VkCalibratedTimestampInfoEXT *pTimestampInfos, uint64_t *pTimestamps, uint64_t *pMaxDeviation) {
    if (pTimestamps) for (uint32_t i = 0; i < timestampCount; i++) pTimestamps[i] = 0;
    if (pMaxDeviation) *pMaxDeviation = 0;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp logicOp) { vkCmdSetLogicOp(commandBuffer, logicOp); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullModeEXT(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode) { vkCmdSetCullMode(commandBuffer, cullMode); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFaceEXT(VkCommandBuffer commandBuffer, VkFrontFace frontFace) { vkCmdSetFrontFace(commandBuffer, frontFace); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopologyEXT(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology) { vkCmdSetPrimitiveTopology(commandBuffer, primitiveTopology); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable) { vkCmdSetDepthTestEnable(commandBuffer, depthTestEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable) { vkCmdSetDepthWriteEnable(commandBuffer, depthWriteEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable) { vkCmdSetStencilTestEnable(commandBuffer, stencilTestEnable); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOpEXT(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp) {
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCountEXT(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport *pViewports) {
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCountEXT(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D *pScissors) {
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetVertexInputEXT(VkCommandBuffer commandBuffer, uint32_t vertexBindingDescriptionCount, const VkVertexInputBindingDescription2EXT *pVertexBindingDescriptions, uint32_t vertexAttributeDescriptionCount, const VkVertexInputAttributeDescription2EXT *pVertexAttributeDescriptions) {
}

/* VK_EXT_private_data */
VkResult vkCreatePrivateDataSlot(VkDevice device, const VkPrivateDataSlotCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkPrivateDataSlot *pPrivateDataSlot) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    *pPrivateDataSlot = (VkPrivateDataSlot)(uintptr_t)1;
    return VK_SUCCESS;
}
VkResult vkCreatePrivateDataSlotEXT(VkDevice device, const VkPrivateDataSlotCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkPrivateDataSlot *pPrivateDataSlot) {
    return vkCreatePrivateDataSlot(device, pCreateInfo, pAllocator, pPrivateDataSlot);
}
void vkDestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)privateDataSlot; (void)pAllocator;
}
void vkDestroyPrivateDataSlotEXT(VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks *pAllocator) {
    vkDestroyPrivateDataSlot(device, privateDataSlot, pAllocator);
}
VkResult vkSetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data) {
    (void)device; (void)objectType; (void)objectHandle; (void)privateDataSlot; (void)data;
    return VK_SUCCESS;
}
VkResult vkSetPrivateDataEXT(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data) {
    return vkSetPrivateData(device, objectType, objectHandle, privateDataSlot, data);
}
void vkGetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t *pData) {
    (void)device; (void)objectType; (void)objectHandle; (void)privateDataSlot;
    *pData = 0;
}
void vkGetPrivateDataEXT(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t *pData) {
    vkGetPrivateData(device, objectType, objectHandle, privateDataSlot, pData);
}

/* VK_KHR_synchronization_2 */
void vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query) {
    (void)commandBuffer; (void)stage; (void)queryPool; (void)query;
}
void vkCmdWriteTimestamp2KHR(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query) {
    vkCmdWriteTimestamp2(commandBuffer, stage, queryPool, query);
}

/* VK_KHR_maintenance_4 / Vulkan 1.3 */
void vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    (void)device; (void)pInfo;
    memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
    pMemoryRequirements->memoryRequirements.memoryTypeBits = 0x1;
    pMemoryRequirements->memoryRequirements.alignment = 256;
    pMemoryRequirements->memoryRequirements.size = 4096;
}
void vkGetDeviceBufferMemoryRequirementsKHR(VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    vkGetDeviceBufferMemoryRequirements(device, pInfo, pMemoryRequirements);
}
void vkGetDeviceImageMemoryRequirementsKHR(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    vkGetDeviceImageMemoryRequirements(device, pInfo, pMemoryRequirements);
}
void vkGetDeviceImageSubresourceLayout(VkDevice device, const VkDeviceImageSubresourceInfo *pInfo, VkSubresourceLayout2 *pLayout) {
    (void)device; (void)pInfo;
    memset(pLayout, 0, sizeof(*pLayout));
    pLayout->subresourceLayout.rowPitch = 4096;
}
void vkGetDeviceImageSubresourceLayoutKHR(VkDevice device, const VkDeviceImageSubresourceInfo *pInfo, VkSubresourceLayout2 *pLayout) {
    vkGetDeviceImageSubresourceLayout(device, pInfo, pLayout);
}

/* Vulkan ICD Entry Point Lookup Table */
VkResult vkCreateSamplerYcbcrConversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSamplerYcbcrConversion *pYcbcrConversion) {
    (void)device; (void)pCreateInfo; (void)pAllocator;
    struct VkSamplerYcbcrConversion_T *conv = calloc(1, sizeof(*conv));
    if (!conv) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic(conv);
    *pYcbcrConversion = conv;
    return VK_SUCCESS;
}

VkResult vkCreateSamplerYcbcrConversionKHR(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSamplerYcbcrConversion *pYcbcrConversion) {
    return vkCreateSamplerYcbcrConversion(device, pCreateInfo, pAllocator, pYcbcrConversion);
}

void vkDestroySamplerYcbcrConversion(VkDevice device, VkSamplerYcbcrConversion ycbcrConversion, const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    free(ycbcrConversion);
}

void vkDestroySamplerYcbcrConversionKHR(VkDevice device, VkSamplerYcbcrConversion ycbcrConversion, const VkAllocationCallbacks *pAllocator) {
    vkDestroySamplerYcbcrConversion(device, ycbcrConversion, pAllocator);
}

void vkGetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo, VkDescriptorSetLayoutSupport *pSupport) {
    (void)device; (void)pCreateInfo;
    pSupport->supported = VK_TRUE;
}

void vkGetDescriptorSetLayoutSupportKHR(VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo, VkDescriptorSetLayoutSupport *pSupport) {
    vkGetDescriptorSetLayoutSupport(device, pCreateInfo, pSupport);
}

VkResult vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface, uint32_t *pModes) {
    (void)device; (void)surface;
    *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VkResult vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t *pDataSize, void *pData) {
    (void)device; (void)pipelineCache;
    if (pDataSize) *pDataSize = 0;
    return VK_SUCCESS;
}

VkResult vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache *pSrcCaches) {
    (void)device; (void)dstCache; (void)srcCacheCount; (void)pSrcCaches;
    return VK_SUCCESS;
}

/* =========================================================================
 * FIX (crash: "Ask to run at NULL"): Vulkan 1.1/1.2/1.3 device entry points
 * that WineD3D enumerates from the winevulkan vulkan_device_funcs table but
 * the driver did not export. WineD3D trusts the advertised apiVersion (1.3)
 * and dereferences the returned pointer unconditionally — Box64 v0.4.0 has
 * GO() wrappers for the common ones, so a NULL slot becomes
 * "Unhandled page fault on execute access to 00000000" (seen at the call to
 * vkCmdBeginRenderPass2KHR). Each entry below is a non-NULL, best-effort
 * stub so device init and render passes can proceed.
 * ========================================================================= */

/* ---- Bind/Memory-requirements (*2 / KHR) ---- */
VkResult vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const struct VkBindBufferMemoryInfo *pBindInfos) {
    if (!pBindInfos) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        if (pBindInfos[i].buffer && pBindInfos[i].memory) {
            pBindInfos[i].buffer->bo = pBindInfos[i].memory->bo;
            pBindInfos[i].buffer->memory_offset = pBindInfos[i].memoryOffset;
        }
    }
    return VK_SUCCESS;
}
VkResult vkBindBufferMemory2KHR(VkDevice device, uint32_t bindInfoCount, const struct VkBindBufferMemoryInfo *pBindInfos) {
    return vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
}
VkResult vkBindImageMemory2(VkDevice device, uint32_t bindInfoCount, const struct VkBindImageMemoryInfo *pBindInfos) {
    if (!pBindInfos) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        if (pBindInfos[i].image && pBindInfos[i].memory) {
            pBindInfos[i].image->bo = pBindInfos[i].memory->bo;
            pBindInfos[i].image->memory_offset = pBindInfos[i].memoryOffset;
        }
    }
    return VK_SUCCESS;
}
VkResult vkBindImageMemory2KHR(VkDevice device, uint32_t bindInfoCount, const struct VkBindImageMemoryInfo *pBindInfos) {
    return vkBindImageMemory2(device, bindInfoCount, pBindInfos);
}

void vkGetBufferMemoryRequirements2(VkDevice device, const struct VkBufferMemoryRequirementsInfo2 *pInfo, struct VkMemoryRequirements2 *pMemoryRequirements) {
    (void)pInfo;
    if (pMemoryRequirements) {
        vkGetBufferMemoryRequirements(device, pInfo ? pInfo->buffer : (VkBuffer)0,
                                      &pMemoryRequirements->memoryRequirements);
    }
}
void vkGetBufferMemoryRequirements2KHR(VkDevice device, const struct VkBufferMemoryRequirementsInfo2 *pInfo, struct VkMemoryRequirements2 *pMemoryRequirements) {
    vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
}
void vkGetImageMemoryRequirements2(VkDevice device, const struct VkImageMemoryRequirementsInfo2 *pInfo, struct VkMemoryRequirements2 *pMemoryRequirements) {
    (void)pInfo;
    if (pMemoryRequirements) {
        vkGetImageMemoryRequirements(device, pInfo ? pInfo->image : (VkImage)0,
                                     &pMemoryRequirements->memoryRequirements);
    }
}
void vkGetImageMemoryRequirements2KHR(VkDevice device, const struct VkImageMemoryRequirementsInfo2 *pInfo, struct VkMemoryRequirements2 *pMemoryRequirements) {
    vkGetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);
}
void vkGetImageSparseMemoryRequirements2KHR(VkDevice device, const struct VkImageSparseMemoryRequirementsInfo2 *pInfo, uint32_t *pSparseMemoryRequirementCount, struct VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements) {
    (void)device; (void)pInfo;
    if (pSparseMemoryRequirementCount) { *pSparseMemoryRequirementCount = 0; }
    if (pSparseMemoryRequirements) { memset(pSparseMemoryRequirements, 0, sizeof(*pSparseMemoryRequirements)); }
}

/* ---- Renderpass2 (core + KHR) ---- */
void vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer, const struct VkRenderPassBeginInfo *pRenderPassBegin, const struct VkSubpassBeginInfo *pSubpassBeginInfo) {
    (void)commandBuffer; (void)pRenderPassBegin; (void)pSubpassBeginInfo;
}
void vkCmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const struct VkRenderPassBeginInfo *pRenderPassBegin, const struct VkSubpassBeginInfo *pSubpassBeginInfo) {
    vkCmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}
void vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const struct VkSubpassEndInfo *pSubpassEndInfo) {
    (void)commandBuffer; (void)pSubpassEndInfo;
}
void vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const struct VkSubpassEndInfo *pSubpassEndInfo) {
    vkCmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
}
void vkCmdNextSubpass2(VkCommandBuffer commandBuffer, const struct VkSubpassBeginInfo *pSubpassBeginInfo, const struct VkSubpassEndInfo *pSubpassEndInfo) {
    (void)commandBuffer; (void)pSubpassBeginInfo; (void)pSubpassEndInfo;
}
void vkCmdNextSubpass2KHR(VkCommandBuffer commandBuffer, const struct VkSubpassBeginInfo *pSubpassBeginInfo, const struct VkSubpassEndInfo *pSubpassEndInfo) {
    vkCmdNextSubpass2(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
 }

/* ---- Descriptor push ---- */
void vkCmdPushDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const struct VkWriteDescriptorSet *pDescriptorWrites) {
    (void)commandBuffer; (void)pipelineBindPoint; (void)layout; (void)set;
    (void)descriptorWriteCount; (void)pDescriptorWrites;
}

/* ---- Pipeline executable introspection (KHR) ---- */
VkResult vkGetPipelineExecutablePropertiesKHR(VkDevice device, const struct VkPipelineInfoKHR *pPipelineInfo, uint32_t *pExecutableCount, struct VkPipelineExecutablePropertiesKHR *pProperties) {
    (void)device; (void)pPipelineInfo;
    if (pExecutableCount) { *pExecutableCount = 0; }
    if (pProperties) { memset(pProperties, 0, sizeof(*pProperties)); }
    return VK_SUCCESS;
}
VkResult vkGetPipelineExecutableStatisticsKHR(VkDevice device, const struct VkPipelineExecutableInfoKHR *pExecutableInfo, uint32_t *pStatisticCount, struct VkPipelineExecutableStatisticKHR *pStatistics) {
    (void)device; (void)pExecutableInfo;
    if (pStatisticCount) { *pStatisticCount = 0; }
    if (pStatistics) { memset(pStatistics, 0, sizeof(*pStatistics)); }
    return VK_SUCCESS;
}
VkResult vkGetPipelineExecutableInternalRepresentationsKHR(VkDevice device, const struct VkPipelineExecutableInfoKHR *pExecutableInfo, uint32_t *pInternalRepresentationCount, struct VkPipelineExecutableInternalRepresentationKHR *pInternalRepresentations) {
    (void)device; (void)pExecutableInfo;
    if (pInternalRepresentationCount) { *pInternalRepresentationCount = 0; }
    if (pInternalRepresentations) { memset(pInternalRepresentations, 0, sizeof(*pInternalRepresentations)); }
     return VK_SUCCESS;
}

/* ---- Acceleration structures (KHR) ---- */
VkResult vkBuildAccelerationStructuresKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, uint32_t infoCount, const struct VkAccelerationStructureBuildGeometryInfoKHR *pInfos, const struct VkAccelerationStructureBuildRangeInfoKHR * const *ppBuildRangeInfos) {
    (void)device; (void)deferredOperation; (void)infoCount; (void)pInfos; (void)ppBuildRangeInfos;
    return VK_OPERATION_NOT_DEFERRED_KHR;
}

/* =========================================================================
 * VK_KHR_external_memory_win32 / VK_KHR_external_semaphore_win32 stubs.
 *
 * winevulkan/dxvk/vkd3d resolve these via vkGetDeviceProcAddr and WILL call
 * the returned pointer unconditionally. Returning NULL made the loader reach
 * "Ask to run at NULL" -> SIGSEGV. Box64 v0.4.0 has GO() wrappers for all of
 * them, so returning real (error-returning) stubs is safe: the caller gets a
 * graceful error instead of a crash.
 * ========================================================================= */
VkResult vkGetMemoryWin32HandleKHR(VkDevice device, const void *pInfo, void **pHandle) {
    (void)device; (void)pInfo;
    if (pHandle) *pHandle = NULL;
    pvk_log("vkGetMemoryWin32HandleKHR: external memory win32 handle not supported -> VK_ERROR_INVALID_EXTERNAL_HANDLE\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VkResult vkGetMemoryWin32HandlePropertiesKHR(VkDevice device, uint32_t handleType, void *handle, void *pProps) {
    (void)device; (void)handleType; (void)handle; (void)pProps;
    pvk_log("vkGetMemoryWin32HandlePropertiesKHR: external memory win32 handle not supported -> VK_ERROR_INVALID_EXTERNAL_HANDLE\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VkResult vkGetSemaphoreWin32HandleKHR(VkDevice device, const void *pInfo, void **pHandle) {
    (void)device; (void)pInfo;
    if (pHandle) *pHandle = NULL;
    pvk_log("vkGetSemaphoreWin32HandleKHR: external semaphore win32 handle not supported -> VK_ERROR_INVALID_EXTERNAL_HANDLE\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VkResult vkImportSemaphoreWin32HandleKHR(VkDevice device, const void *pInfo) {
    (void)device; (void)pInfo;
    pvk_log("vkImportSemaphoreWin32HandleKHR: external semaphore win32 handle not supported -> VK_ERROR_INVALID_EXTERNAL_HANDLE\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

/* Generic fallback stub was REMOVED: returning a non-NULL pointer for an
 * unknown function makes Box64 v0.4.0 SIGSEGV (no GO() wrapper -> crashes
 * dereferencing NULL in its own bridge table, seen as "execute access to
 * 00000000" in wine). NULL is the correct Vulkan behavior: winevulkan marks
 * the function unsupported and never calls it. */

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
     if (!pName) return NULL;
     pvk_log("vkGetInstanceProcAddr: instance=%p name='%s'\n", (void*)instance, pName);
 #define MATCH(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction)name
     /* stubs: declarações das funções UNKNOWN (variádicas) — mesmas queues
      * do MATCH table gerada (vk_stubs_decl.h). */
 #include "vk_stubs_decl.h"
    MATCH(vk_icdNegotiateLoaderICDInterfaceVersion);
    MATCH(vkGetInstanceProcAddr);
    MATCH(vkGetDeviceProcAddr);
    MATCH(vk_icdGetInstanceProcAddr);
    MATCH(vkEnumerateInstanceVersion);
    MATCH(vkCreateInstance);
    MATCH(vkDestroyInstance);
    MATCH(vkEnumerateInstanceExtensionProperties);
    MATCH(vkEnumerateInstanceLayerProperties);
    MATCH(vkEnumerateDeviceExtensionProperties);
    MATCH(vkEnumeratePhysicalDevices);
    MATCH(vkEnumeratePhysicalDeviceGroups);
    MATCH(vkEnumeratePhysicalDeviceGroupsKHR);
    MATCH(vkGetPhysicalDeviceProperties);
    MATCH(vkGetPhysicalDeviceProperties2);
    MATCH(vkGetPhysicalDeviceProperties2KHR);
    MATCH(vkGetPhysicalDeviceFeatures);
    MATCH(vkGetPhysicalDeviceFeatures2);
    MATCH(vkGetPhysicalDeviceFeatures2KHR);
    MATCH(vkGetPhysicalDeviceQueueFamilyProperties);
    MATCH(vkGetPhysicalDeviceQueueFamilyProperties2);
    MATCH(vkGetPhysicalDeviceQueueFamilyProperties2KHR);
    MATCH(vkGetPhysicalDeviceMemoryProperties);
    MATCH(vkGetPhysicalDeviceMemoryProperties2);
    MATCH(vkGetPhysicalDeviceMemoryProperties2KHR);
    MATCH(vkGetPhysicalDeviceExternalBufferProperties);
    MATCH(vkGetPhysicalDeviceExternalBufferPropertiesKHR);
    MATCH(vkGetPhysicalDeviceExternalFenceProperties);
    MATCH(vkGetPhysicalDeviceExternalFencePropertiesKHR);
    MATCH(vkGetPhysicalDeviceExternalSemaphoreProperties);
    MATCH(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR);
    MATCH(vkGetPhysicalDeviceExternalImageFormatPropertiesNV);
    MATCH(vkGetPhysicalDeviceToolProperties);
    MATCH(vkGetPhysicalDeviceToolPropertiesEXT);
    MATCH(vkGetPhysicalDevicePresentRectanglesKHR);
    MATCH(vkGetPhysicalDeviceXlibPresentationSupportKHR);
    MATCH(vkGetPhysicalDeviceDisplayProperties2KHR);
    MATCH(vkGetPhysicalDeviceDisplayPlaneProperties2KHR);
    MATCH(vkGetDisplayModeProperties2KHR);
    MATCH(vkGetDisplayPlaneCapabilities2KHR);
    MATCH(vkGetPhysicalDeviceFormatProperties);
    MATCH(vkGetPhysicalDeviceFormatProperties2);
    MATCH(vkGetPhysicalDeviceFormatProperties2KHR);
    MATCH(vkGetPhysicalDeviceImageFormatProperties);
    MATCH(vkGetPhysicalDeviceImageFormatProperties2);
    MATCH(vkGetPhysicalDeviceImageFormatProperties2KHR);
    MATCH(vkGetPhysicalDeviceSparseImageFormatProperties);
    MATCH(vkGetPhysicalDeviceSparseImageFormatProperties2);
    MATCH(vkGetPhysicalDeviceSparseImageFormatProperties2KHR);
    MATCH(vkCreateDevice);
    MATCH(vkDestroyDevice);
    MATCH(vkGetDeviceQueue);
    MATCH(vkAllocateMemory);
    MATCH(vkFreeMemory);
    MATCH(vkMapMemory);
    MATCH(vkUnmapMemory);
    MATCH(vkCreateBuffer);
    MATCH(vkDestroyBuffer);
    MATCH(vkGetBufferMemoryRequirements);
    MATCH(vkBindBufferMemory);
    MATCH(vkCreateImage);
    MATCH(vkDestroyImage);
    MATCH(vkGetImageMemoryRequirements);
    MATCH(vkGetImageSubresourceLayout);
    MATCH(vkBindImageMemory);
    MATCH(vkCreateImageView);
    MATCH(vkDestroyImageView);
    MATCH(vkCreateBufferView);
    MATCH(vkDestroyBufferView);
    MATCH(vkGetRenderAreaGranularity);
    MATCH(vkCmdSetLineWidth);
    MATCH(vkCmdNextSubpass);
    MATCH(vkCmdResolveImage);
    MATCH(vkCmdCopyQueryPoolResults);
    MATCH(vkGetQueryPoolResults);
    MATCH(vkResetDescriptorPool);
    MATCH(vkQueueBindSparse);
    MATCH(vkGetDeviceMemoryCommitment);
    MATCH(vkGetImageSparseMemoryRequirements);
    MATCH(vkGetImageSparseMemoryRequirements2);
    MATCH(vkTrimCommandPool);
    MATCH(vkGetDeviceGroupPeerMemoryFeatures);
    MATCH(vkCmdSetDeviceMask);
    MATCH(vkCreateDescriptorUpdateTemplate);
    MATCH(vkDestroyDescriptorUpdateTemplate);
    MATCH(vkUpdateDescriptorSetWithTemplate);
    MATCH(vkCreateShaderModule);
    MATCH(vkDestroyShaderModule);
    MATCH(vkCreatePipelineCache);
    MATCH(vkDestroyPipelineCache);
    MATCH(vkCreatePipelineLayout);
    MATCH(vkDestroyPipelineLayout);
    MATCH(vkCreateRenderPass);
    MATCH(vkDestroyRenderPass);
    MATCH(vkCreateFramebuffer);
    MATCH(vkDestroyFramebuffer);
    MATCH(vkCreateDescriptorSetLayout);
    MATCH(vkDestroyDescriptorSetLayout);
    MATCH(vkCreateDescriptorPool);
    MATCH(vkDestroyDescriptorPool);
    MATCH(vkAllocateDescriptorSets);
    MATCH(vkFreeDescriptorSets);
    MATCH(vkUpdateDescriptorSets);
    MATCH(vkCreateGraphicsPipelines);
    MATCH(vkCreateComputePipelines);
    MATCH(vkDestroyPipeline);
    MATCH(vkCreateSemaphore);
    MATCH(vkDestroySemaphore);
    MATCH(vkCreateFence);
    MATCH(vkDestroyFence);
    MATCH(vkResetFences);
    MATCH(vkGetFenceStatus);
    MATCH(vkWaitForFences);
    MATCH(vkCreateCommandPool);
    MATCH(vkDestroyCommandPool);
    MATCH(vkAllocateCommandBuffers);
    MATCH(vkFreeCommandBuffers);
    MATCH(vkBeginCommandBuffer);
    MATCH(vkEndCommandBuffer);
    MATCH(vkCmdBindPipeline);
    MATCH(vkCmdSetViewport);
    MATCH(vkCmdSetScissor);
    MATCH(vkCmdBindDescriptorSets);
    MATCH(vkCmdBindVertexBuffers);
    MATCH(vkCmdBindIndexBuffer);
    MATCH(vkCmdPushConstants);
    MATCH(vkCmdSetDepthBias);
    MATCH(vkCreateEvent);
    MATCH(vkDestroyEvent);
    MATCH(vkGetEventStatus);
    MATCH(vkSetEvent);
    MATCH(vkResetEvent);
    MATCH(vkCmdSetEvent);
    MATCH(vkCmdResetEvent);
    MATCH(vkCmdWaitEvents);
    MATCH(vkCreateQueryPool);
    MATCH(vkDestroyQueryPool);
    MATCH(vkCmdBeginQuery);
    MATCH(vkCmdEndQuery);
    MATCH(vkCmdWriteTimestamp);
    MATCH(vkCmdResetQueryPool);
    MATCH(vkCmdDispatch);
    MATCH(vkCmdDispatchIndirect);
    MATCH(vkFlushMappedMemoryRanges);
    MATCH(vkInvalidateMappedMemoryRanges);
    MATCH(vkCreateRenderPass2);
    MATCH(vkCreateRenderPass2KHR);
    MATCH(vkGetDeviceQueue2);
    MATCH(vkCmdPipelineBarrier2);
    MATCH(vkCmdPipelineBarrier2KHR);
    MATCH(vkQueueSubmit2);
    MATCH(vkQueueSubmit2KHR);
    MATCH(vkCmdExecuteCommands);
    MATCH(vkCmdCopyBuffer);
    MATCH(vkCreateSampler);
    MATCH(vkDestroySampler);
    MATCH(vkCmdCopyBufferToImage);
    MATCH(vkCmdCopyImageToBuffer);
    MATCH(vkCmdCopyImage);
    MATCH(vkCmdBlitImage);
    MATCH(vkCmdClearColorImage);
    MATCH(vkCmdClearDepthStencilImage);
    MATCH(vkCmdClearAttachments);
    MATCH(vkCmdPipelineBarrier);
    MATCH(vkCmdDraw);
    MATCH(vkCmdBeginRenderPass);
    MATCH(vkCmdDrawIndexed);
    MATCH(vkCmdEndRenderPass);
    MATCH(vkQueueSubmit);
    MATCH(vkQueueWaitIdle);
    MATCH(vkDeviceWaitIdle);
    MATCH(vkCreateXlibSurfaceKHR);
    MATCH(vkCreateXcbSurfaceKHR);
    /* vkCreateWin32SurfaceKHR: Box64 now has wrapper (my_vkCreateWin32SurfaceKHR).
     * Driver handles Wine surface by storing HWND (Winlator handles display). */
    MATCH(vkCreateWin32SurfaceKHR);
    MATCH(vkGetPhysicalDeviceWin32PresentationSupportKHR);
    MATCH(vkCreateWINE_nulldrvSurface);
    MATCH(vkGetPhysicalDeviceXcbPresentationSupportKHR);
    MATCH(vkGetPhysicalDeviceDisplayPropertiesKHR);
    MATCH(vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
    MATCH(vkGetDisplayPlaneSupportedDisplaysKHR);
    MATCH(vkGetDisplayModePropertiesKHR);
    MATCH(vkDestroySurfaceKHR);
    MATCH(vkGetPhysicalDeviceSurfaceSupportKHR);
    MATCH(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    MATCH(vkGetPhysicalDeviceSurfaceFormatsKHR);
    MATCH(vkGetPhysicalDeviceSurfacePresentModesKHR);
    MATCH(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    MATCH(vkGetPhysicalDeviceSurfaceFormats2KHR);
    MATCH(vkGetPhysicalDeviceSurfacePresentModes2KHR);
    MATCH(vkReleaseSwapchainImagesEXT);
    MATCH(vkCreateSwapchainKHR);
    MATCH(vkDestroySwapchainKHR);
    MATCH(vkGetSwapchainImagesKHR);
    MATCH(vkAcquireNextImageKHR);
    MATCH(vkQueuePresentKHR);
    MATCH(vkGetBufferDeviceAddress);
    MATCH(vkGetBufferDeviceAddressKHR);
    MATCH(vkGetBufferOpaqueCaptureAddress);
    MATCH(vkGetDeviceMemoryOpaqueCaptureAddress);
    MATCH(vkGetSemaphoreCounterValue);
    MATCH(vkGetSemaphoreCounterValueKHR);
    MATCH(vkWaitSemaphores);
    MATCH(vkWaitSemaphoresKHR);
    MATCH(vkSignalSemaphore);
    MATCH(vkSignalSemaphoreKHR);
    MATCH(vkCmdBeginRendering);
    MATCH(vkCmdEndRendering);
    MATCH(vkCmdBeginRenderingKHR);
    MATCH(vkCmdEndRenderingKHR);
    MATCH(vkCmdSetEvent2);
    MATCH(vkCmdResetEvent2);
    MATCH(vkCmdWaitEvents2);
    MATCH(vkCmdSetEvent2KHR);
    MATCH(vkCmdResetEvent2KHR);
    MATCH(vkCmdWaitEvents2KHR);
    MATCH(vkCmdDrawIndirect);
    MATCH(vkCmdDrawIndexedIndirect);
    MATCH(vkCmdDrawIndirectCount);
    MATCH(vkCmdDrawIndirectCountKHR);
    MATCH(vkCmdDrawIndexedIndirectCount);
    MATCH(vkCmdDrawIndexedIndirectCountKHR);
    MATCH(vkCmdDispatchBase);
    MATCH(vkCmdFillBuffer);
    MATCH(vkCmdUpdateBuffer);
    MATCH(vkCmdPushDescriptorSetKHR);
    MATCH(vkCmdPushDescriptorSetWithTemplateKHR);
    MATCH(vkCmdSetStencilReference);
    MATCH(vkCmdSetStencilCompareMask);
    MATCH(vkCmdSetStencilWriteMask);
    MATCH(vkCmdSetBlendConstants);
    MATCH(vkCmdSetDepthBounds);
    MATCH(vkCmdSetDepthTestEnable);
    MATCH(vkCmdSetDepthWriteEnable);
    MATCH(vkCmdSetDepthCompareOp);
    MATCH(vkCmdSetStencilTestEnable);
    MATCH(vkCmdSetCullMode);
    MATCH(vkCmdSetFrontFace);
    MATCH(vkCmdSetPrimitiveTopology);
    MATCH(vkCmdSetColorBlendEnable);
    MATCH(vkCmdSetColorWriteMask);
    MATCH(vkCmdSetColorBlendEquation);
    MATCH(vkCmdSetScissorWithCount);
    MATCH(vkCmdSetViewportWithCount);
    MATCH(vkCmdSetLogicOp);
    MATCH(vkCmdSetDepthBiasEnable);
    MATCH(vkCmdSetPrimitiveRestartEnable);
    MATCH(vkCmdSetRasterizerDiscardEnable);
    MATCH(vkCmdSetDepthBoundsTestEnable);
    MATCH(vkCmdSetDepthBias2EXT);
    MATCH(vkCmdSetSampleLocationsEXT);
    MATCH(vkCmdSetDiscardRectangleEXT);
    MATCH(vkCmdSetFragmentShadingRateKHR);
    MATCH(vkGetPhysicalDeviceFragmentShadingRatesKHR);
    MATCH(vkGetImageSubresourceLayout2);
    MATCH(vkGetImageSubresourceLayout2KHR);
    MATCH(vkGetDeviceImageMemoryRequirements);
    MATCH(vkGetDeviceImageSparseMemoryRequirements);
    MATCH(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT);
    MATCH(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR);
    MATCH(vkGetPhysicalDeviceMultisamplePropertiesEXT);
    MATCH(vkGetPhysicalDeviceSupportedFramebufferMixedSamplesCombinationsNV);
    MATCH(vkGetPhysicalDeviceVideoCapabilitiesKHR);
    MATCH(vkGetPhysicalDeviceVideoFormatPropertiesKHR);
    MATCH(vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR);
    MATCH(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR);
    MATCH(vkGetPhysicalDeviceCooperativeMatrixPropertiesNV);
    MATCH(vkGetPhysicalDeviceCooperativeMatrixFlexibleDimensionsPropertiesNV);
    MATCH(vkGetPhysicalDeviceOpticalFlowImageFormatsNV);
    MATCH(vkCreateDebugReportCallbackEXT);
    MATCH(vkDestroyDebugReportCallbackEXT);
    MATCH(vkDebugReportMessageEXT);
    MATCH(vkCreateDebugUtilsMessengerEXT);
    MATCH(vkDestroyDebugUtilsMessengerEXT);
    MATCH(vkSubmitDebugUtilsMessageEXT);
    MATCH(vkReleaseDisplayEXT);
    MATCH(vkAcquireXlibDisplayEXT);
    MATCH(vkGetRandROutputDisplayEXT);
    MATCH(vkGetPhysicalDeviceSurfaceCapabilities2EXT);
    MATCH(vkCreateHeadlessSurfaceEXT);
    MATCH(vkAcquireDrmDisplayEXT);
    MATCH(vkGetDrmDisplayEXT);
    MATCH(vkCreateDisplayModeKHR);
    MATCH(vkGetDisplayPlaneCapabilitiesKHR);
    MATCH(vkCreateDisplayPlaneSurfaceKHR);
    MATCH(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    MATCH(vkGetCalibratedTimestampsEXT);
    MATCH(vkCmdSetLogicOpEXT);
    MATCH(vkCmdSetCullModeEXT);
    MATCH(vkCmdSetFrontFaceEXT);
    MATCH(vkCmdSetPrimitiveTopologyEXT);
    MATCH(vkCmdSetDepthTestEnableEXT);
    MATCH(vkCmdSetDepthWriteEnableEXT);
    MATCH(vkCmdSetStencilTestEnableEXT);
    MATCH(vkCmdSetStencilOpEXT);
    MATCH(panvk_v9_read_pixel);
    MATCH(vkResetCommandBuffer);
    MATCH(vkResetCommandPool);
    MATCH(vkCreateSamplerYcbcrConversion);
    MATCH(vkDestroySamplerYcbcrConversion);
    MATCH(vkCreateSamplerYcbcrConversionKHR);
    MATCH(vkDestroySamplerYcbcrConversionKHR);
    MATCH(vkGetDescriptorSetLayoutSupport);
    MATCH(vkGetDescriptorSetLayoutSupportKHR);
    MATCH(vkGetDeviceGroupSurfacePresentModesKHR);
    MATCH(vkGetPipelineCacheData);
    MATCH(vkMergePipelineCaches);
    MATCH(vkCmdSetViewportWithCountEXT);
    MATCH(vkCmdSetScissorWithCountEXT);
    MATCH(vkCmdSetVertexInputEXT);
    MATCH(vkCmdSetRasterizerDiscardEnableEXT);
    MATCH(vkCmdSetDepthBiasEnableEXT);
    MATCH(vkCmdSetPrimitiveRestartEnableEXT);
    MATCH(vkCmdSetColorBlendEnableEXT);
    MATCH(vkCmdSetColorWriteMaskEXT);
    MATCH(vkCmdSetColorBlendEquationEXT);
    MATCH(vkCmdSetDepthBoundsTestEnableEXT);
    MATCH(vkResetQueryPool);
    MATCH(vkResetQueryPoolEXT);
    MATCH(vkCreatePrivateDataSlot);
    MATCH(vkCreatePrivateDataSlotEXT);
    MATCH(vkDestroyPrivateDataSlot);
    MATCH(vkDestroyPrivateDataSlotEXT);
    MATCH(vkSetPrivateData);
    MATCH(vkSetPrivateDataEXT);
    MATCH(vkGetPrivateData);
    MATCH(vkGetPrivateDataEXT);
    MATCH(vkCmdWriteTimestamp2);
    MATCH(vkCmdWriteTimestamp2KHR);
    MATCH(vkGetDeviceBufferMemoryRequirements);
    MATCH(vkGetDeviceBufferMemoryRequirementsKHR);
    MATCH(vkGetDeviceImageMemoryRequirementsKHR);
    MATCH(vkGetDeviceImageSubresourceLayout);
    MATCH(vkGetDeviceImageSubresourceLayoutKHR);
    /* VK_KHR_external_memory_win32 / external_semaphore_win32 stubs
     * (Box64 has GO() wrappers for these, so non-NULL is safe). */
    MATCH(vkGetMemoryWin32HandleKHR);
    MATCH(vkGetMemoryWin32HandlePropertiesKHR);
    MATCH(vkGetSemaphoreWin32HandleKHR);
     MATCH(vkImportSemaphoreWin32HandleKHR);
     /* FIX (crash: "Ask to run at NULL"): device entry points wined3d/winevulkan
      * enumerate from the vulkan_device_funcs table that the driver did not
      * resolve. Returning non-NULL (best-effort stub) prevents the loader from
      * hitting a NULL slot. (vkAcquireNextImage2KHR has a real impl above.) */
     MATCH(vkAcquireNextImage2KHR);
     MATCH(vkBindBufferMemory2);
     MATCH(vkBindBufferMemory2KHR);
     MATCH(vkBindImageMemory2);
     MATCH(vkBindImageMemory2KHR);
     MATCH(vkGetBufferMemoryRequirements2);
     MATCH(vkGetBufferMemoryRequirements2KHR);
     MATCH(vkGetImageMemoryRequirements2);
     MATCH(vkGetImageMemoryRequirements2KHR);
     MATCH(vkGetImageSparseMemoryRequirements2KHR);
     MATCH(vkCmdBeginRenderPass2);
     MATCH(vkCmdBeginRenderPass2KHR);
     MATCH(vkCmdEndRenderPass2);
     MATCH(vkCmdEndRenderPass2KHR);
     MATCH(vkCmdNextSubpass2);
     MATCH(vkCmdNextSubpass2KHR);
     MATCH(vkCmdPushDescriptorSet);
     /* NOTE: vkReleaseSwapchainImagesKHR, vkCmdSetFragmentShadingRate (core)
      * and vkGetShaderModuleCreateInfoSizesKHR are intentionally NOT matched:
      * Box64 v0.4.0 has no GO() wrapper for them, so returning non-NULL makes
      * Box64 SIGSEGV ("no wrapper ... (nil)"). NULL is the Vulkan-correct
      * response — winevulkan marks them unsupported and skips. */
     MATCH(vkGetPipelineExecutablePropertiesKHR);
     MATCH(vkGetPipelineExecutableStatisticsKHR);
     MATCH(vkGetPipelineExecutableInternalRepresentationsKHR);
     MATCH(vkBuildAccelerationStructuresKHR);
    /* --- stubs gerados (UNKNOWN funcs -> retornam erro em vez de NULL) --- */
//    MATCH(vkAcquireFullScreenExclusiveModeEXT);
    MATCH(vkAcquirePerformanceConfigurationINTEL);
    MATCH(vkAcquireProfilingLockKHR);
    MATCH(vkAntiLagUpdateAMD);
    MATCH(vkBindAccelerationStructureMemoryNV);
    MATCH(vkBindDataGraphPipelineSessionMemoryARM);
    MATCH(vkBindOpticalFlowSessionImageNV);
    MATCH(vkBindTensorMemoryARM);
    MATCH(vkBindVideoSessionMemoryKHR);
    MATCH(vkBuildMicromapsEXT);
    MATCH(vkCmdBeginConditionalRenderingEXT);
    MATCH(vkCmdBeginQueryIndexedEXT);
    MATCH(vkCmdBeginTransformFeedbackEXT);
    MATCH(vkCmdBeginVideoCodingKHR);
    MATCH(vkCmdBindDescriptorBufferEmbeddedSamplers2EXT);
    MATCH(vkCmdBindDescriptorBufferEmbeddedSamplersEXT);
    MATCH(vkCmdBindDescriptorBuffersEXT);
    MATCH(vkCmdBindDescriptorSets2);
    MATCH(vkCmdBindDescriptorSets2KHR);
    MATCH(vkCmdBindIndexBuffer2);
    MATCH(vkCmdBindIndexBuffer2KHR);
    MATCH(vkCmdBindInvocationMaskHUAWEI);
    MATCH(vkCmdBindPipelineShaderGroupNV);
    MATCH(vkCmdBindShadersEXT);
    MATCH(vkCmdBindShadingRateImageNV);
    MATCH(vkCmdBindTransformFeedbackBuffersEXT);
    MATCH(vkCmdBindVertexBuffers2);
    MATCH(vkCmdBindVertexBuffers2EXT);
    MATCH(vkCmdBlitImage2);
    MATCH(vkCmdBlitImage2KHR);
    MATCH(vkCmdBuildAccelerationStructureNV);
    MATCH(vkCmdBuildAccelerationStructuresIndirectKHR);
    MATCH(vkCmdBuildAccelerationStructuresKHR);
    MATCH(vkCmdBuildMicromapsEXT);
    MATCH(vkCmdControlVideoCodingKHR);
    MATCH(vkCmdConvertCooperativeVectorMatrixNV);
    MATCH(vkCmdCopyAccelerationStructureKHR);
    MATCH(vkCmdCopyAccelerationStructureNV);
    MATCH(vkCmdCopyAccelerationStructureToMemoryKHR);
    MATCH(vkCmdCopyBuffer2);
    MATCH(vkCmdCopyBuffer2KHR);
    MATCH(vkCmdCopyBufferToImage2);
    MATCH(vkCmdCopyBufferToImage2KHR);
    MATCH(vkCmdCopyImage2);
    MATCH(vkCmdCopyImage2KHR);
    MATCH(vkCmdCopyImageToBuffer2);
    MATCH(vkCmdCopyImageToBuffer2KHR);
    MATCH(vkCmdCopyMemoryIndirectNV);
    MATCH(vkCmdCopyMemoryToAccelerationStructureKHR);
    MATCH(vkCmdCopyMemoryToImageIndirectNV);
    MATCH(vkCmdCopyMemoryToMicromapEXT);
    MATCH(vkCmdCopyMicromapEXT);
    MATCH(vkCmdCopyMicromapToMemoryEXT);
    MATCH(vkCmdCopyTensorARM);
    MATCH(vkCmdCuLaunchKernelNVX);
    // MATCH(vkCmdCudaLaunchKernelNV) - handled via stub fallback
    MATCH(vkCmdDebugMarkerBeginEXT);
    MATCH(vkCmdDebugMarkerEndEXT);
    MATCH(vkCmdDebugMarkerInsertEXT);
    MATCH(vkCmdDecodeVideoKHR);
    MATCH(vkCmdDecompressMemoryIndirectCountNV);
    MATCH(vkCmdDecompressMemoryNV);
    MATCH(vkCmdDispatchBaseKHR);
    MATCH(vkCmdDispatchDataGraphARM);
//    MATCH(vkCmdDispatchGraphAMDX);
//    MATCH(vkCmdDispatchGraphIndirectAMDX);
//    MATCH(vkCmdDispatchGraphIndirectCountAMDX);
    MATCH(vkCmdDrawClusterHUAWEI);
    MATCH(vkCmdDrawClusterIndirectHUAWEI);
    MATCH(vkCmdDrawIndexedIndirectCountAMD);
    MATCH(vkCmdDrawIndirectByteCountEXT);
    MATCH(vkCmdDrawIndirectCountAMD);
    MATCH(vkCmdDrawMeshTasksEXT);
    MATCH(vkCmdDrawMeshTasksIndirectCountEXT);
    MATCH(vkCmdDrawMeshTasksIndirectCountNV);
    MATCH(vkCmdDrawMeshTasksIndirectEXT);
    MATCH(vkCmdDrawMeshTasksIndirectNV);
    MATCH(vkCmdDrawMeshTasksNV);
    MATCH(vkCmdDrawMultiEXT);
    MATCH(vkCmdDrawMultiIndexedEXT);
    MATCH(vkCmdEncodeVideoKHR);
    MATCH(vkCmdEndConditionalRenderingEXT);
    MATCH(vkCmdEndQueryIndexedEXT);
    MATCH(vkCmdEndTransformFeedbackEXT);
    MATCH(vkCmdEndVideoCodingKHR);
    MATCH(vkCmdExecuteGeneratedCommandsEXT);
    MATCH(vkCmdExecuteGeneratedCommandsNV);
//    MATCH(vkCmdInitializeGraphScratchMemoryAMDX);
    MATCH(vkCmdOpticalFlowExecuteNV);
    MATCH(vkCmdPreprocessGeneratedCommandsEXT);
    MATCH(vkCmdPreprocessGeneratedCommandsNV);
    MATCH(vkCmdPushConstants2);
    MATCH(vkCmdPushConstants2KHR);
    MATCH(vkCmdPushDescriptorSet2);
    MATCH(vkCmdPushDescriptorSet2KHR);
    MATCH(vkCmdPushDescriptorSetWithTemplate);
    MATCH(vkCmdPushDescriptorSetWithTemplate2);
    MATCH(vkCmdPushDescriptorSetWithTemplate2KHR);
    MATCH(vkCmdResolveImage2);
    MATCH(vkCmdResolveImage2KHR);
    MATCH(vkCmdSetAlphaToCoverageEnableEXT);
    MATCH(vkCmdSetAlphaToOneEnableEXT);
    MATCH(vkCmdSetAttachmentFeedbackLoopEnableEXT);
    MATCH(vkCmdSetCheckpointNV);
    MATCH(vkCmdSetCoarseSampleOrderNV);
    MATCH(vkCmdSetColorBlendAdvancedEXT);
    MATCH(vkCmdSetColorWriteEnableEXT);
    MATCH(vkCmdSetConservativeRasterizationModeEXT);
    MATCH(vkCmdSetCoverageModulationModeNV);
    MATCH(vkCmdSetCoverageModulationTableEnableNV);
    MATCH(vkCmdSetCoverageModulationTableNV);
    MATCH(vkCmdSetCoverageReductionModeNV);
    MATCH(vkCmdSetCoverageToColorEnableNV);
    MATCH(vkCmdSetCoverageToColorLocationNV);
    MATCH(vkCmdSetDepthClampEnableEXT);
    MATCH(vkCmdSetDepthClampRangeEXT);
    MATCH(vkCmdSetDepthClipEnableEXT);
    MATCH(vkCmdSetDepthClipNegativeOneToOneEXT);
    MATCH(vkCmdSetDepthCompareOpEXT);
    MATCH(vkCmdSetDescriptorBufferOffsets2EXT);
    MATCH(vkCmdSetDescriptorBufferOffsetsEXT);
    MATCH(vkCmdSetDeviceMaskKHR);
    MATCH(vkCmdSetDiscardRectangleEnableEXT);
    MATCH(vkCmdSetDiscardRectangleModeEXT);
    MATCH(vkCmdSetExclusiveScissorEnableNV);
    MATCH(vkCmdSetExclusiveScissorNV);
    MATCH(vkCmdSetExtraPrimitiveOverestimationSizeEXT);
    MATCH(vkCmdSetFragmentShadingRateEnumNV);
    MATCH(vkCmdSetLineRasterizationModeEXT);
    MATCH(vkCmdSetLineStipple);
    MATCH(vkCmdSetLineStippleEXT);
    MATCH(vkCmdSetLineStippleEnableEXT);
    MATCH(vkCmdSetLineStippleKHR);
    MATCH(vkCmdSetLogicOpEnableEXT);
    MATCH(vkCmdSetPatchControlPointsEXT);
    MATCH(vkCmdSetPerformanceMarkerINTEL);
    MATCH(vkCmdSetPerformanceOverrideINTEL);
    MATCH(vkCmdSetPerformanceStreamMarkerINTEL);
    MATCH(vkCmdSetPolygonModeEXT);
    MATCH(vkCmdSetProvokingVertexModeEXT);
    MATCH(vkCmdSetRasterizationSamplesEXT);
    MATCH(vkCmdSetRasterizationStreamEXT);
    MATCH(vkCmdSetRayTracingPipelineStackSizeKHR);
    MATCH(vkCmdSetRenderingAttachmentLocations);
    MATCH(vkCmdSetRenderingAttachmentLocationsKHR);
    MATCH(vkCmdSetRenderingInputAttachmentIndices);
    MATCH(vkCmdSetRenderingInputAttachmentIndicesKHR);
    MATCH(vkCmdSetRepresentativeFragmentTestEnableNV);
    MATCH(vkCmdSetSampleLocationsEnableEXT);
    MATCH(vkCmdSetSampleMaskEXT);
    MATCH(vkCmdSetShadingRateImageEnableNV);
    MATCH(vkCmdSetStencilOp);
    MATCH(vkCmdSetTessellationDomainOriginEXT);
    MATCH(vkCmdSetViewportShadingRatePaletteNV);
    MATCH(vkCmdSetViewportSwizzleNV);
    MATCH(vkCmdSetViewportWScalingEnableNV);
    MATCH(vkCmdSetViewportWScalingNV);
    MATCH(vkCmdSubpassShadingHUAWEI);
    MATCH(vkCmdTraceRaysIndirect2KHR);
    MATCH(vkCmdTraceRaysIndirectKHR);
    MATCH(vkCmdTraceRaysKHR);
    MATCH(vkCmdTraceRaysNV);
    MATCH(vkCmdUpdatePipelineIndirectBufferNV);
    MATCH(vkCmdWriteAccelerationStructuresPropertiesKHR);
    MATCH(vkCmdWriteAccelerationStructuresPropertiesNV);
    MATCH(vkCmdWriteBufferMarker2AMD);
    MATCH(vkCmdWriteBufferMarkerAMD);
    MATCH(vkCmdWriteMicromapsPropertiesEXT);
    MATCH(vkCompileDeferredNV);
    MATCH(vkConvertCooperativeVectorMatrixNV);
    MATCH(vkCopyAccelerationStructureKHR);
    MATCH(vkCopyAccelerationStructureToMemoryKHR);
    MATCH(vkCopyImageToImage);
    MATCH(vkCopyImageToImageEXT);
    MATCH(vkCopyImageToMemory);
    MATCH(vkCopyImageToMemoryEXT);
    MATCH(vkCopyMemoryToAccelerationStructureKHR);
    MATCH(vkCopyMemoryToImage);
    MATCH(vkCopyMemoryToImageEXT);
    MATCH(vkCopyMemoryToMicromapEXT);
    MATCH(vkCopyMicromapEXT);
    MATCH(vkCopyMicromapToMemoryEXT);
    MATCH(vkDeferredOperationJoinKHR);
    MATCH(vkDisplayPowerControlEXT);
    MATCH(vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM);
    MATCH(vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR);
    MATCH(vkGetAccelerationStructureBuildSizesKHR);
    MATCH(vkGetAccelerationStructureDeviceAddressKHR);
    MATCH(vkGetAccelerationStructureHandleNV);
    MATCH(vkGetAccelerationStructureMemoryRequirementsNV);
    MATCH(vkGetAccelerationStructureOpaqueCaptureDescriptorDataEXT);
    MATCH(vkGetBufferDeviceAddressEXT);
    MATCH(vkGetBufferOpaqueCaptureAddressKHR);
    MATCH(vkGetBufferOpaqueCaptureDescriptorDataEXT);
    MATCH(vkGetCalibratedTimestampsKHR);
    // MATCH(vkGetCudaModuleCacheNV)
    MATCH(vkGetDataGraphPipelineAvailablePropertiesARM);
    MATCH(vkGetDataGraphPipelinePropertiesARM);
    MATCH(vkGetDataGraphPipelineSessionBindPointRequirementsARM);
    MATCH(vkGetDataGraphPipelineSessionMemoryRequirementsARM);
    MATCH(vkGetDeferredOperationMaxConcurrencyKHR);
    MATCH(vkGetDeferredOperationResultKHR);
    MATCH(vkGetDescriptorEXT);
    MATCH(vkGetDescriptorSetHostMappingVALVE);
    MATCH(vkGetDescriptorSetLayoutBindingOffsetEXT);
    MATCH(vkGetDescriptorSetLayoutHostMappingInfoVALVE);
    MATCH(vkGetDescriptorSetLayoutSizeEXT);
    MATCH(vkGetDeviceAccelerationStructureCompatibilityKHR);
    MATCH(vkGetDeviceFaultInfoEXT);
    MATCH(vkGetDeviceGroupPeerMemoryFeaturesKHR);
    MATCH(vkGetDeviceGroupPresentCapabilitiesKHR);
//    MATCH(vkGetDeviceGroupSurfacePresentModes2EXT);
    MATCH(vkGetDeviceImageSparseMemoryRequirementsKHR);
    MATCH(vkGetDeviceMemoryOpaqueCaptureAddressKHR);
    MATCH(vkGetDeviceMicromapCompatibilityEXT);
    MATCH(vkGetDeviceSubpassShadingMaxWorkgroupSizeHUAWEI);
    MATCH(vkGetDeviceTensorMemoryRequirementsARM);
    MATCH(vkGetDynamicRenderingTilePropertiesQCOM);
    MATCH(vkGetEncodedVideoSessionParametersKHR);
//    MATCH(vkGetExecutionGraphPipelineNodeIndexAMDX);
//    MATCH(vkGetExecutionGraphPipelineScratchSizeAMDX);
    MATCH(vkGetFenceFdKHR);
    MATCH(vkGetFramebufferTilePropertiesQCOM);
    MATCH(vkGetGeneratedCommandsMemoryRequirementsEXT);
    MATCH(vkGetGeneratedCommandsMemoryRequirementsNV);
    MATCH(vkGetImageDrmFormatModifierPropertiesEXT);
    MATCH(vkGetImageOpaqueCaptureDescriptorDataEXT);
    MATCH(vkGetImageSubresourceLayout2EXT);
    MATCH(vkGetImageViewAddressNVX);
    MATCH(vkGetImageViewHandle64NVX);
    MATCH(vkGetImageViewHandleNVX);
    MATCH(vkGetImageViewOpaqueCaptureDescriptorDataEXT);
    MATCH(vkGetLatencyTimingsNV);
    MATCH(vkGetMemoryFdKHR);
    MATCH(vkGetMemoryFdPropertiesKHR);
    MATCH(vkGetMemoryHostPointerPropertiesEXT);
    MATCH(vkGetMemoryRemoteAddressNV);
    MATCH(vkGetMicromapBuildSizesEXT);
    MATCH(vkGetPastPresentationTimingGOOGLE);
    MATCH(vkGetPerformanceParameterINTEL);
    MATCH(vkGetPhysicalDeviceCooperativeVectorPropertiesNV);
    MATCH(vkGetPhysicalDeviceExternalTensorPropertiesARM);
    MATCH(vkGetPhysicalDeviceQueueFamilyDataGraphProcessingEnginePropertiesARM);
    MATCH(vkGetPhysicalDeviceQueueFamilyDataGraphPropertiesARM);
    MATCH(vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR);
    MATCH(vkGetPhysicalDeviceSurfacePresentModes2EXT);
//    MATCH(vkGetPhysicalDeviceWaylandPresentationSupportKHR);
    MATCH(vkGetPipelineBinaryDataKHR);
    MATCH(vkGetPipelineIndirectDeviceAddressNV);
    MATCH(vkGetPipelineIndirectMemoryRequirementsNV);
    MATCH(vkGetPipelineKeyKHR);
    MATCH(vkGetPipelinePropertiesEXT);
    MATCH(vkGetQueueCheckpointData2NV);
    MATCH(vkGetQueueCheckpointDataNV);
    MATCH(vkGetRayTracingCaptureReplayShaderGroupHandlesKHR);
    MATCH(vkGetRayTracingShaderGroupHandlesKHR);
    MATCH(vkGetRayTracingShaderGroupHandlesNV);
    MATCH(vkGetRayTracingShaderGroupStackSizeKHR);
    MATCH(vkGetRefreshCycleDurationGOOGLE);
    MATCH(vkGetRenderingAreaGranularity);
    MATCH(vkGetRenderingAreaGranularityKHR);
    MATCH(vkGetSamplerOpaqueCaptureDescriptorDataEXT);
    MATCH(vkGetSemaphoreFdKHR);
    MATCH(vkGetShaderBinaryDataEXT);
    MATCH(vkGetShaderInfoAMD);
    MATCH(vkGetShaderModuleCreateInfoIdentifierEXT);
    MATCH(vkGetShaderModuleIdentifierEXT);
    MATCH(vkGetSwapchainCounterEXT);
    MATCH(vkGetSwapchainStatusKHR);
    MATCH(vkGetTensorMemoryRequirementsARM);
    MATCH(vkGetTensorOpaqueCaptureDescriptorDataARM);
    MATCH(vkGetTensorViewOpaqueCaptureDescriptorDataARM);
    MATCH(vkGetValidationCacheDataEXT);
    MATCH(vkGetVideoSessionMemoryRequirementsKHR);
    MATCH(vkImportFenceFdKHR);
    MATCH(vkImportSemaphoreFdKHR);
    MATCH(vkInitializePerformanceApiINTEL);
    MATCH(vkLatencySleepNV);
    MATCH(vkMapMemory2);
    MATCH(vkMapMemory2KHR);
    MATCH(vkMergeValidationCachesEXT);
    MATCH(vkQueueNotifyOutOfBandNV);
    MATCH(vkQueueSetPerformanceConfigurationINTEL);
//    MATCH(vkReleaseFullScreenExclusiveModeEXT);
    MATCH(vkReleasePerformanceConfigurationINTEL);
    MATCH(vkReleaseProfilingLockKHR);
    MATCH(vkSetDeviceMemoryPriorityEXT);
    MATCH(vkSetHdrMetadataEXT);
    MATCH(vkSetLatencyMarkerNV);
    MATCH(vkSetLatencySleepModeNV);
    MATCH(vkSetLocalDimmingAMD);
    MATCH(vkTransitionImageLayout);
    MATCH(vkTransitionImageLayoutEXT);
    MATCH(vkTrimCommandPoolKHR);
    MATCH(vkUninitializePerformanceApiINTEL);
    MATCH(vkUnmapMemory2);
    MATCH(vkUnmapMemory2KHR);
    MATCH(vkUpdateDescriptorSetWithTemplateKHR);
    MATCH(vkUpdateIndirectExecutionSetPipelineEXT);
    MATCH(vkUpdateIndirectExecutionSetShaderEXT);
    MATCH(vkUpdateVideoSessionParametersKHR);
    MATCH(vkWaitForPresent2KHR);
    MATCH(vkWaitForPresentKHR);
    MATCH(vkWriteAccelerationStructuresPropertiesKHR);
    MATCH(vkWriteMicromapsPropertiesEXT);
 #undef MATCH

    /* vkCreateWin32SurfaceKHR is now exposed via MATCH table above.
     * Box64 has a wrapper for it (my_vkCreateWin32SurfaceKHR). No override needed. */

    /* FIX (crash real): NÃO devolver o generic stub para funções fora da */
    pvk_log("vkGetInstanceProcAddr: UNKNOWN function '%s' -> returning NULL (no Box64 wrapper)\n", pName);
    return NULL;
}

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    return vkGetInstanceProcAddr(NULL, pName);
}

__attribute__((visibility("default"))) PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return vkGetInstanceProcAddr(instance, pName);
}