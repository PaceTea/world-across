// The builder (albw_builder.h).
//
// Where things are. The package's kit -- the translating tools, the addresses the game is known to
// branch to (entries/), the header the translated code is compiled against, the module's glue and,
// where the package brings them, Python and a compiler -- is looked for beside the program (kit/),
// where an AppImage keeps it (../share/world-across/kit) and, for a developer's build, in the
// checkout the program was built in. What is made goes into <cache>/build: the card's code, the
// translation, the objects, the module and build.log, the log of everything that ran.
#include "albw_builder.h"
#include "albw_platform.h"
#include "albw_savedata.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#define MODULE_NAME "game.dll"
#define EXE ".exe"
#else
#define MODULE_NAME "libgame.so"
#define EXE ""
#endif

#define STAGES 5
#define SPLIT 16                                         // C files the translation is split into

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static struct albw_build_progress now;                  // under mutex
static char build_dir[600], module_path[700], code_path[700], log_path[700];
static char kit[600], tools[700], include_dir[700], glue[700], python[700], cc[700];
static int printing;                                     // albw_build_run: progress to stdout

// The releases whose addresses the kit may carry (entries/<product>.txt): all of them are handed to
// the tools, which pick the card's.
static const char *const releases[] = { "CTR-P-BZLE", "CTR-P-BZLP", "CTR-P-BZLJ", "CTR-P-BZLK", NULL };

// The flags the translation is checked with: no fused multiply-adds, no assumptions about aliasing,
// sqrtf as an instruction, nothing that would call a C library; -pipe keeps the compiler's
// intermediate files out of the temporary folder.
static const char *const compile_flags[] = {
    "-O1", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-pipe",
#ifndef _WIN32
    "-fPIC", "-fvisibility=hidden",
#endif
};
#define COMPILE_FLAGS (sizeof compile_flags / sizeof compile_flags[0])

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void paths(void) {
    if (build_dir[0]) return;
    char root[500];
    albw_cache_root(root, sizeof root);
    snprintf(build_dir, sizeof build_dir, "%s/build", root);
    snprintf(module_path, sizeof module_path, "%s/%s", build_dir, MODULE_NAME);
    snprintf(code_path, sizeof code_path, "%s/rom-extract/code.bin", build_dir);
    snprintf(log_path, sizeof log_path, "%s/build.log", build_dir);
}

const char *albw_build_dir(void) { paths(); return build_dir; }
const char *albw_build_module_path(void) { paths(); return module_path; }
const char *albw_build_code_path(void) { paths(); return code_path; }

static void make_dirs(const char *path) {
    char buf[800];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue;
        char sep = *p;
        *p = '\0';
        albw_mkdir(buf);
        *p = sep;
    }
    albw_mkdir(buf);
}

// A line into the log and, for a run with no window and where `show`, onto the terminal.
static void log_vline(int show, const char *fmt, va_list ap) {
    va_list again;
    va_copy(again, ap);
    FILE *f = fopen(log_path, "a");
    if (f) {
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    if (show && printing) {
        vprintf(fmt, again);
        putchar('\n');
        fflush(stdout);
    }
    va_end(again);
}

static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vline(1, fmt, ap);
    va_end(ap);
}

static void log_only(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vline(0, fmt, ap);
    va_end(ap);
}

static void set_stage(int stage, const char *what) {
    pthread_mutex_lock(&mutex);
    now.stage = stage;
    now.stages = STAGES;
    now.fraction = -1.0f;
    snprintf(now.what, sizeof now.what, "%s", what);
    pthread_mutex_unlock(&mutex);
    log_line("== %d/%d %s", stage, STAGES, what);
}

static int fail(const char *fmt, ...) {
    char why[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    log_line("failed: %s", why);
    pthread_mutex_lock(&mutex);
    snprintf(now.error, sizeof now.error, "%s", why);
    pthread_mutex_unlock(&mutex);
    return 0;
}

// The kit, and in it -- or on the system -- Python and a compiler.
static int find_kit(void) {
    char exe[600];
    albw_exe_dir(exe, sizeof exe);
    const char *const layouts[] = { "%s/kit", "%s/../share/world-across/kit", "%s/.." };
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        char dir[600], probe[700];
        snprintf(dir, sizeof dir, layouts[i], exe);
        snprintf(probe, sizeof probe, "%s/tools/recompile.py", dir);
        if (!exists(probe)) continue;
        snprintf(kit, sizeof kit, "%s", dir);
        snprintf(tools, sizeof tools, "%s/tools", dir);
        // A package's kit has include/ and glue/; a checkout has them in src/runtime.
        snprintf(include_dir, sizeof include_dir, "%s/include", dir);
        snprintf(glue, sizeof glue, "%s/glue/albw_game_glue.c", dir);
        if (!exists(glue)) {
            snprintf(include_dir, sizeof include_dir, "%s/src/runtime", dir);
            snprintf(glue, sizeof glue, "%s/src/runtime/albw_game_glue.c", dir);
        }
        // Python and the compiler: the kit's own where it has them, else the system's.
#ifdef _WIN32
        snprintf(python, sizeof python, "%s/python/python.exe", dir);
        if (!exists(python)) snprintf(python, sizeof python, "python");
        snprintf(cc, sizeof cc, "%s/gcc/bin/gcc.exe", dir);
        if (!exists(cc)) snprintf(cc, sizeof cc, "gcc");
#else
        snprintf(python, sizeof python, "%s/python/bin/python3", dir);
        if (!exists(python)) snprintf(python, sizeof python, "python3");
        snprintf(cc, sizeof cc, "%s/gcc/bin/gcc", dir);
        if (!exists(cc)) snprintf(cc, sizeof cc, "cc");
#endif
        return exists(glue);
    }
    return 0;
}

static int run(const char *const *argv) {
    char line[2048];
    size_t at = 0;
    for (int i = 0; argv[i] && at < sizeof line - 1; i++) {
        at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? " " : "", argv[i]);
    }
    // Into the log only: what runs is for when something goes wrong. (Compiling runs this on
    // several threads at once, so nothing here may change what the others see.)
    log_only("$ %s", line);
    return albw_run(argv, build_dir, log_path);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    return ok;
}

// Compiling, several files at once: as many as there are processors, and as the memory free now
// allows -- a compiler takes up to a gigabyte on the largest of the translated files.
static const char *jobs_src[SPLIT + 8];
static int jobs_count, jobs_next, jobs_done, jobs_failed;

static void *compile_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mutex);
        int i = jobs_next < jobs_count && !jobs_failed ? jobs_next++ : -1;
        pthread_mutex_unlock(&mutex);
        if (i < 0) return NULL;
        char obj[400], base[300];
        const char *name = strrchr(jobs_src[i], '/');
        snprintf(base, sizeof base, "%s", name ? name + 1 : jobs_src[i]);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(obj, sizeof obj, "obj/%s.o", base);
        // Every path is relative to the build folder, so that where the package was unpacked never
        // matters to the compiler.
        const char *argv[COMPILE_FLAGS + 10];
        size_t n = 0;
        argv[n++] = cc;
        for (size_t k = 0; k < COMPILE_FLAGS; k++) argv[n++] = compile_flags[k];
        argv[n++] = "-Iglue";
        argv[n++] = "-Isrc/generated";
        argv[n++] = "-c";
        argv[n++] = jobs_src[i];
        argv[n++] = "-o";
        argv[n++] = obj;
        argv[n] = NULL;
        int status = run(argv);
        pthread_mutex_lock(&mutex);
        if (status != 0) jobs_failed = 1;
        jobs_done++;
        now.fraction = (float)jobs_done / (float)jobs_count;
        pthread_mutex_unlock(&mutex);
        if (printing) printf("   compiled %s (%d of %d)\n", base, jobs_done, jobs_count);
    }
}

// What play has taught the build. When the game reaches code the module lacks, the runtime notes the
// address in missing-<product>.txt beside the saves; the next build folds it into the release's
// addresses, and keeps it -- with every one before it -- in found-<product>.txt there, so that it
// is never needed twice. (That file can only be made by playing, so it lives with the saves, not
// in the cache.) Version 0.1.1 wrote missing_entries.txt and learned-<product>.txt, without the
// release in the one and with Thumb addresses' low bit lost: both are left alone.
static void missing_path(char *out, size_t n, const char *product) {
    snprintf(out, n, "%s/missing-%s.txt", albw_save_root(), product);
}

static void learned_path(char *out, size_t n, const char *product) {
    snprintf(out, n, "%s/found-%s.txt", albw_save_root(), product);
}

// The release of the game there now, from the card it was made from.
static int built_product(char *out, size_t n) {
    char path[800];
    paths();
    snprintf(path, sizeof path, "%s/rom-extract/product_code.txt", build_dir);
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, " \r\n")] = '\0';
        fclose(f);
    }
    return out[0] && !strpbrk(out, "/\\.");
}

int albw_build_has_missing(void) {
    char prod[32], path[700];
    if (!built_product(prod, sizeof prod)) return 0;
    missing_path(path, sizeof path, prod);
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

// Addresses -- eight hex digits a line -- read into a set kept sorted.
struct addrs { uint32_t *v; size_t n, cap; };

static int addr_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void addrs_read(struct addrs *a, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        char *end;
        unsigned long v = strtoul(line, &end, 16);
        if (end == line) continue;
        if (a->n == a->cap) {
            size_t cap = a->cap ? a->cap * 2 : 1024;
            uint32_t *grown = realloc(a->v, cap * sizeof *grown);
            if (!grown) break;
            a->v = grown;
            a->cap = cap;
        }
        a->v[a->n++] = (uint32_t)v;
    }
    fclose(f);
}

static int addrs_write(struct addrs *a, const char *path) {
    qsort(a->v, a->n, sizeof *a->v, addr_cmp);
    char tmp[800];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    for (size_t i = 0; i < a->n; i++) {
        if (i && a->v[i] == a->v[i - 1]) continue;
        fprintf(f, "%08X\n", a->v[i]);
    }
    if (fclose(f) != 0) return 0;
    return albw_rename(tmp, path) == 0;
}

static char product[32];                                 // the card's, once it has been read

// Which kit a game was made with: a hash of everything the build reads besides the card -- the
// tools, the addresses, the header and the glue -- and of how it compiles. A launcher of another
// version brings another kit, and makes the game again rather than play a translation its runtime
// may no longer fit.
static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n--) h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

static uint64_t fnv_file(uint64_t h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return fnv(h, "-", 1);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = fnv(h, buf, n);
    fclose(f);
    return fnv(h, "+", 1);
}

static uint64_t kit_identity(void) {
    uint64_t h = 14695981039346656037ULL;
    char path[900];
    const char *const tool_files[] = { "extract_rom.py", "find_functions.py", "recompile.py" };
    for (size_t i = 0; i < sizeof tool_files / sizeof tool_files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", tools, tool_files[i]);
        h = fnv_file(h, path);
    }
    snprintf(path, sizeof path, "%s/albw_cpu.h", include_dir);
    h = fnv_file(h, path);
    snprintf(path, sizeof path, "%s/albw_game.h", include_dir);
    h = fnv_file(h, path);
    h = fnv_file(h, glue);
    for (int i = 0; releases[i]; i++) {
        snprintf(path, sizeof path, "%s/entries/%s.txt", kit, releases[i]);
        h = fnv_file(h, path);
    }
    for (size_t k = 0; k < COMPILE_FLAGS; k++) h = fnv(h, compile_flags[k], strlen(compile_flags[k]) + 1);
    return fnv(h, MODULE_NAME, sizeof MODULE_NAME);
}

static void kit_id_path(char *out, size_t n) { snprintf(out, n, "%s/kit.id", build_dir); }

int albw_build_current(void) {
    paths();
    // Without its kit the game cannot be made again, so the one there is is played.
    if (!find_kit()) return 1;
    if (albw_build_has_missing()) return 0;               // the last run met code it lacks
    char path[800], line[64] = "";
    kit_id_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = '\0';
    fclose(f);
    return strtoull(line, NULL, 16) == kit_identity();
}

static int build(const char *card) {
    paths();
    time_t started = time(NULL);
    make_dirs(build_dir);
    FILE *f = fopen(log_path, "w");
    if (f) fclose(f);
    log_line("World Across: building the game from %s", card);
    if (!find_kit()) {
        return fail("The package's kit is missing: its tools, the translation's header or the "
                    "module's glue. Unpack the whole package and start the program from the "
                    "unpacked folder.");
    }
    log_line("kit %s, python %s, compiler %s", kit, python, cc);

    // The card's code.
    set_stage(1, "Reading your card");
    char dir[1400], from[1400], to[1400];
    snprintf(dir, sizeof dir, "%s/entries", build_dir);
    make_dirs(dir);
    snprintf(dir, sizeof dir, "%s/obj", build_dir);
    make_dirs(dir);
    snprintf(dir, sizeof dir, "%s/src/generated", build_dir);
    make_dirs(dir);
    // The glue and the two headers it and the translation include, beside the translation.
    snprintf(dir, sizeof dir, "%s/glue", build_dir);
    make_dirs(dir);
    const char *const copies[][2] = {
        { glue, "glue/albw_game_glue.c" },
        { "albw_game.h", "glue/albw_game.h" },
        { "albw_cpu.h", "glue/albw_cpu.h" },
    };
    for (size_t i = 0; i < sizeof copies / sizeof copies[0]; i++) {
        if (copies[i][0] == glue) snprintf(from, sizeof from, "%s", glue);
        else snprintf(from, sizeof from, "%s/%s", include_dir, copies[i][0]);
        snprintf(to, sizeof to, "%s/%s", build_dir, copies[i][1]);
        if (!copy_file(from, to)) return fail("The package's %s could not be copied.", copies[i][0]);
    }
    // The addresses the game is known to branch to, per release: all of them, the tools pick.
    for (int i = 0; releases[i]; i++) {
        snprintf(from, sizeof from, "%s/entries/%s.txt", kit, releases[i]);
        snprintf(to, sizeof to, "%s/entries/%s.txt", build_dir, releases[i]);
        if (exists(from)) copy_file(from, to);
    }
    // Python in its UTF-8 mode, writing the log as UTF-8 whatever the system's code page is, and
    // deaf to PYTHONHOME and PYTHONPATH, which could point the kit's Python at another's library.
    snprintf(from, sizeof from, "%s/extract_rom.py", tools);
    const char *extract[] = { python, "-E", "-X", "utf8", from, card, "rom-extract", NULL };
    int status = run(extract);
    if (status < 0) {
#ifdef _WIN32
        return fail("Python could not be started (%s). Unpack the whole package again.", python);
#else
        return fail("Python 3 could not be started. Install it (it comes with nearly every "
                    "distribution), then press Play again.");
#endif
    }
    if (status != 0 || !exists(code_path)) {
        return fail("The card could not be read. It has to be a decrypted dump of the cartridge "
                    "(.3ds or .cci). The log says more.");
    }
    // The release's addresses, with what play has taught: the ones learned before and the ones
    // the last run met.
    product[0] = '\0';
    snprintf(from, sizeof from, "%s/rom-extract/product_code.txt", build_dir);
    FILE *pf = fopen(from, "r");
    if (pf) {
        if (fgets(product, sizeof product, pf)) product[strcspn(product, " \r\n")] = '\0';
        fclose(pf);
    }
    if (product[0] && !strpbrk(product, "/\\.")) {
        struct addrs known = { 0 };
        snprintf(to, sizeof to, "%s/entries/%s.txt", build_dir, product);
        addrs_read(&known, to);
        size_t from_kit = known.n;
        learned_path(from, sizeof from, product);
        addrs_read(&known, from);
        missing_path(from, sizeof from, product);
        addrs_read(&known, from);
        if (known.n > from_kit) {
            log_line("%zu addresses play has found, besides the package's %zu", known.n - from_kit, from_kit);
            if (!addrs_write(&known, to)) return fail("The addresses could not be written.");
        }
        free(known.v);
    }

    set_stage(2, "Finding the game's functions");
    snprintf(from, sizeof from, "%s/find_functions.py", tools);
    const char *find[] = { python, "-E", "-X", "utf8", from, NULL };
    status = run(find);
    if (status != 0) {
        return fail("Finding the functions failed%s. The log says more.",
#ifdef _WIN32
                    ""
#else
                    " -- is Python's capstone module installed? (python-capstone, or pip install "
                    "capstone)"
#endif
                    );
    }

    set_stage(3, "Translating them to C");
    snprintf(from, sizeof from, "%s/recompile.py", tools);
    char split[16];
    snprintf(split, sizeof split, "%d", SPLIT);
    const char *recompile[] = { python, "-E", "-X", "utf8", from, "--all", "--split", split, NULL };
    status = run(recompile);
    if (status != 0) return fail("Translating failed. The log says more.");

    // Compiling: the translation and the glue, in parallel.
    set_stage(4, "Compiling the translation");
    const char *ver[] = { cc, "--version", NULL };
    if (run(ver) != 0) {
#ifdef _WIN32
        return fail("The compiler could not be started (%s). Unpack the whole package again.", cc);
#else
        return fail("A C compiler is needed, once, to build the game: install gcc (Arch: "
                    "pacman -S gcc; Debian, Ubuntu: apt install gcc; Fedora: dnf install gcc), "
                    "then press Play again.");
#endif
    }
    static char sources[SPLIT + 8][64];
    jobs_count = 0;
    for (int i = 0; i < SPLIT + 4 && jobs_count < SPLIT + 4; i++) {
        snprintf(sources[jobs_count], sizeof sources[0], "src/generated/albw_generated_%d.c", i);
        snprintf(to, sizeof to, "%s/%s", build_dir, sources[jobs_count]);
        if (exists(to)) {
            jobs_src[jobs_count] = sources[jobs_count];
            jobs_count++;
        }
    }
    const char *extra[] = { "src/generated/albw_tables.c", "src/generated/albw_code_id.c" };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
        snprintf(to, sizeof to, "%s/%s", build_dir, extra[i]);
        if (exists(to)) jobs_src[jobs_count++] = extra[i];
    }
    jobs_src[jobs_count++] = "glue/albw_game_glue.c";
    jobs_next = jobs_done = jobs_failed = 0;
    int workers = albw_cpu_count();
    uint64_t memory = albw_memory_available();
    uint64_t per_compile = (uint64_t)5 << 28;            // 1.25 GB: the largest file takes 1
    if (memory && (uint64_t)workers > memory / per_compile) workers = (int)(memory / per_compile);
    if (workers > jobs_count) workers = jobs_count;
    if (workers < 1) workers = 1;
    log_line("%d files, %d at a time", jobs_count, workers);
    pthread_t threads[64];
    if (workers > 64) workers = 64;
    for (int i = 0; i < workers; i++) pthread_create(&threads[i], NULL, compile_worker, NULL);
    for (int i = 0; i < workers; i++) pthread_join(threads[i], NULL);
    if (jobs_failed) return fail("Compiling failed. The log says more.");

    set_stage(5, "Linking the game");
    const char *link[48];
    int n = 0;
    char out[64];
    snprintf(out, sizeof out, "%s.new", MODULE_NAME);
    link[n++] = cc;
    link[n++] = "-shared";
    link[n++] = "-nostdlib";
    link[n++] = "-o";
    link[n++] = out;
    static char objs[SPLIT + 8][320];
    for (int i = 0; i < jobs_count; i++) {
        const char *name = strrchr(jobs_src[i], '/');
        snprintf(objs[i], sizeof objs[i], "obj/%s", name ? name + 1 : jobs_src[i]);
        char *dot = strrchr(objs[i], '.');
        if (dot) strcpy(dot, ".o");
        link[n++] = objs[i];
    }
    link[n++] = "-lgcc";
#ifdef _WIN32
    link[n++] = "-Wl,--entry,DllMainCRTStartup";
#else
    // Everything the translation calls has to be in it, or Play would find out instead.
    link[n++] = "-Wl,--no-undefined";
#endif
    link[n] = NULL;
    if (run(link) != 0) return fail("Linking failed. The log says more.");
    snprintf(from, sizeof from, "%s/%s", build_dir, out);
    if (albw_rename(from, module_path) != 0) return fail("The game could not be put in place.");
    kit_id_path(to, sizeof to);
    FILE *id = fopen(to, "w");
    if (id) {
        fprintf(id, "%016llx\n", (unsigned long long)kit_identity());
        fclose(id);
    }
    // What the last run met is part of the game now: kept, and no longer missing.
    if (product[0] && albw_build_has_missing()) {
        struct addrs learned = { 0 };
        char missing[700];
        learned_path(to, sizeof to, product);
        missing_path(missing, sizeof missing, product);
        addrs_read(&learned, to);
        addrs_read(&learned, missing);
        if (addrs_write(&learned, to)) remove(missing);
        free(learned.v);
    }
    log_line("built %s in %ld seconds", module_path, (long)(time(NULL) - started));
    return 1;
}

static void (*done_cb)(int ok);
static char card_copy[1024];

static void *build_thread(void *arg) {
    (void)arg;
    int ok = build(card_copy);
    pthread_mutex_lock(&mutex);
    now.state = ok ? ALBW_BUILD_DONE : ALBW_BUILD_FAILED;
    pthread_mutex_unlock(&mutex);
    if (done_cb) done_cb(ok);
    return NULL;
}

int albw_build_start(const char *card, void (*done)(int ok)) {
    paths();
    pthread_mutex_lock(&mutex);
    if (now.state == ALBW_BUILD_RUNNING) {
        pthread_mutex_unlock(&mutex);
        return 0;
    }
    memset(&now, 0, sizeof now);
    now.state = ALBW_BUILD_RUNNING;
    now.stages = STAGES;
    now.fraction = -1.0f;
    snprintf(now.log, sizeof now.log, "%s", log_path);
    pthread_mutex_unlock(&mutex);
    snprintf(card_copy, sizeof card_copy, "%s", card);
    done_cb = done;
    pthread_t t;
    if (pthread_create(&t, NULL, build_thread, NULL) != 0) {
        pthread_mutex_lock(&mutex);
        now.state = ALBW_BUILD_FAILED;
        snprintf(now.error, sizeof now.error, "The build could not be started.");
        pthread_mutex_unlock(&mutex);
        return 0;
    }
    pthread_detach(t);
    return 1;
}

void albw_build_progress(struct albw_build_progress *out) {
    pthread_mutex_lock(&mutex);
    *out = now;
    pthread_mutex_unlock(&mutex);
}

int albw_build_run(const char *card) {
    paths();
    printing = 1;
    memset(&now, 0, sizeof now);
    now.state = ALBW_BUILD_RUNNING;
    snprintf(now.log, sizeof now.log, "%s", log_path);
    int ok = build(card);
    now.state = ok ? ALBW_BUILD_DONE : ALBW_BUILD_FAILED;
    if (!ok) printf("[build] %s\n[build] the log: %s\n", now.error, log_path);
    return ok;
}
