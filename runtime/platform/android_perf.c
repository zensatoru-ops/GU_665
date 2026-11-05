// Android performance hints (ADPF: Android Dynamic Performance Framework; runtime/platform.h perf_hint_*). The threads
// the frame rate rests on share one hint session whose target is the frame period at the settings' FPS, and each
// presented frame's work is reported to it: the system then picks cores and clocks for that rate (it raises the
// threads' clock floor when frames run long and lets it down when they are short), rather than its load tracking
// winding the clocks down during every frame's waits and up again too late.
//   MHGU_ADPF=0             no session
//   MHGU_ADPF_LOG=1         every 2 s: the work reported per frame (average, max), each thread's CPU time per frame
//                           (average / max), the target, the thermal headroom and status (also without a session)
//   MHGU_ADPF_TARGET=<pct>  the target as a percentage of the frame period (default 100; lower: boosted sooner, warmer)
//
// The threads: the game's update thread (runtime/hle/vi.c vi_pause_game, once a frame), the presenting one
// (runtime/nvn/vulkan/device.c backend_present) and the one translating command lists (exec.c backend_execute) when it
// is another. The session is made with the first two once both are known (those still running: a thread whose CPU
// clock no longer answers has ended). A translating thread joins it with APerformanceHint_setThreads (API 34) only once
// it has submitted in SUBMIT_FRAMES frames in a row - a thread that submits a few times while loading and ends doesn't
// - as does a game or render thread that turns up later; one that ended leaves it then. The functions are looked up in
// libandroid.so at run time (API 33; the app's minimum is 28: below 33 there is no session), and the session is made
// on a thread of its own (binder calls: the frame's path only reports, which the system batches).
//
// A frame's work: the busiest session thread's CPU time since the previous present (CLOCK_THREAD_CPUTIME_ID: what it
// computed, with the time it spent blocked left out wherever that was - the guest's waits on its events (vsync, the
// other thread's work), the frame limiter's sleep, the GPU waits, vk_lock). The render thread's time from frame start to
// present less its GPU waits (vk_wait_seq) would still count the guest's own waits as work: at the FPS cap that is the
// whole frame period whatever the load, and the system would boost all the time. The busiest thread rather than the sum:
// the update thread goes on with the next frame while the render thread translates this one, so the slower of the two
// sets the rate (MHGU_ADPF_LOG shows both: their sum above the frame period at the cap says they overlap).
// Power efficiency (APerformanceHint_setPreferPowerEfficiency, API 35): on, the system may keep the threads on its
// efficient cores, which a tight frame cannot afford, but which is most of a phone's heat when frames leave time over.
// So it goes by what the frames need: on after EASY_RUNS seconds or so of frames using under half the target, off at once
// when they near it (MHGU_ADPF_EFFICIENT=0: never).
#define _GNU_SOURCE
#include <android/api-level.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "../config.h"
#include "../platform.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

// <android/performance_hint.h> (API 33-35) and <android/thermal.h> (API 30-31), called through pointers: linked
// directly, they would keep the app from loading below API 33
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AThermalManager AThermalManager;
static struct {
    APerformanceHintManager *(*get_manager)(void);
    APerformanceHintSession *(*create_session)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int64_t (*preferred_rate)(APerformanceHintManager *);
    int (*update_target)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    int (*set_threads)(APerformanceHintSession *, const pid_t *, size_t);  // (API 34)
    int (*prefer_efficiency)(APerformanceHintSession *, bool);             // (API 35)
    AThermalManager *(*thermal_manager)(void);                             // (API 30)
    int (*thermal_status)(AThermalManager *);                              // (API 30)
    float (*thermal_headroom)(AThermalManager *, int);                     // (API 31)
} fn;
static pthread_once_t load_once = PTHREAD_ONCE_INIT;

static void load(void) {
    void *lib = dlopen("libandroid.so", RTLD_NOW);  // (already loaded: the runtime links it)
    if (!lib) return;
#define SYM(f, name) fn.f = (__typeof__(fn.f))dlsym(lib, name)
    SYM(get_manager, "APerformanceHint_getManager");
    SYM(create_session, "APerformanceHint_createSession");
    SYM(preferred_rate, "APerformanceHint_getPreferredUpdateRateNanos");
    SYM(update_target, "APerformanceHint_updateTargetWorkDuration");
    SYM(report, "APerformanceHint_reportActualWorkDuration");
    SYM(set_threads, "APerformanceHint_setThreads");
    SYM(prefer_efficiency, "APerformanceHint_setPreferPowerEfficiency");
    SYM(thermal_manager, "AThermal_acquireManager");
    SYM(thermal_status, "AThermal_getCurrentThermalStatus");
    SYM(thermal_headroom, "AThermal_getThermalHeadroom");
#undef SYM
}

// ---- the threads (each names itself: perf_hint_thread)
#define MAX_THREADS 8     // (room for translating threads that come and go: entries are never reused)
#define SUBMIT_FRAMES 8   // a translating thread joins the session once it has submitted in this many frames in a row
#define CORE (1u << PERF_GAME | 1u << PERF_RENDER)  // (the roles the session is made with)
typedef struct {
    pid_t tid;
    clockid_t clock;              // its CPU time
    _Atomic unsigned roles;       // PERF_* bits (changed under lock)
    _Atomic uint32_t submitted;   // the frame (presents) it last submitted in
    // (the presenting thread's)
    bool gone;                    // its clock stopped answering: the thread ended
    bool in;                      // in the session (or the one being made)
    int streak;                   // not in it: the frames in a row it submitted in (up to SUBMIT_FRAMES)
    uint64_t last, sum, max;      // its CPU time at the last present; this log period's total and largest frame
} Thr;
static Thr thr[MAX_THREADS];
static _Atomic int nthr;  // entries complete below it
static _Atomic unsigned roles_known;
static _Atomic uint32_t presents;  // frames presented so far: the number of the one in progress
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned t_roles;  // the roles this thread has given already
static __thread int t_slot = -1;   // its entry in thr (-1: none, the table was full)
static void hot_thread_to_big_cores(void);  // (Snapdragon 665 class, below)

void perf_hint_thread(int role) {
    unsigned bit = 1u << role;
    if (t_roles & bit) {  // (every frame after the first: a thread-local load; a translating thread notes the frame)
        if (role == PERF_SUBMIT && t_slot >= 0)
            atomic_store_explicit(&thr[t_slot].submitted, atomic_load_explicit(&presents, memory_order_relaxed),
                                  memory_order_relaxed);
        return;
    }
    if (!t_roles) hot_thread_to_big_cores();  // (this thread's first role: once per thread)
    t_roles |= bit;
    pid_t tid = gettid();
    uint32_t f = atomic_load_explicit(&presents, memory_order_relaxed);
    pthread_mutex_lock(&lock);
    int n = atomic_load_explicit(&nthr, memory_order_relaxed), i = 0;
    while (i < n && thr[i].tid != tid) i++;
    if (i < n) {
        atomic_fetch_or(&thr[i].roles, bit);
    } else if (n < MAX_THREADS && pthread_getcpuclockid(pthread_self(), &thr[n].clock) == 0) {
        thr[n].tid = tid;
        atomic_store(&thr[n].roles, bit);
        atomic_store_explicit(&thr[n].submitted, f - 1, memory_order_relaxed);  // (none yet)
        atomic_store_explicit(&nthr, n + 1, memory_order_release);
    } else {
        i = -1;
    }
    if (i >= 0 && role == PERF_SUBMIT) atomic_store_explicit(&thr[i].submitted, f, memory_order_relaxed);
    t_slot = i;
    atomic_fetch_or(&roles_known, bit);
    pthread_mutex_unlock(&lock);
}

static void role_name(unsigned roles, char *buf, size_t size) {
    static const char *const names[] = {"game", "render", "submit"};
    size_t o = 0;
    buf[0] = 0;
    for (int r = 0; r < 3; r++)
        if ((roles & (1u << r)) && o < size) o += (size_t)snprintf(buf + o, size - o, "%s%s", o ? "+" : "", names[r]);
}

// "game 1234, render+submit 1235" (the entries in mask)
static void thread_list(unsigned mask, char *buf, size_t size) {
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < MAX_THREADS && o < size; i++) {
        if (!(mask >> i & 1)) continue;
        char r[32];
        role_name(atomic_load(&thr[i].roles), r, sizeof r);
        o += (size_t)snprintf(buf + o, size - o, "%s%s %d", o ? ", " : "", r, (int)thr[i].tid);
    }
}

static bool alive(Thr *t) {  // (the presenting thread) its clock still answers: the thread has not ended
    struct timespec ts;
    if (!t->gone && clock_gettime(t->clock, &ts) != 0) t->gone = true;
    return !t->gone;
}

// ---- the session
static _Atomic(APerformanceHintSession *) session;  // (published by the maker once made)
static _Atomic int unavailable;                      // there will be none: MHGU_ADPF=0, below API 33, refused
// (the presenting thread's)
static bool making;      // the maker has been started
static int64_t target_ns;
static int pct = 100, log_on = -1;
static struct { int n; int32_t tids[MAX_THREADS]; char list[200]; } mk;  // the maker's threads (set before it starts)

static int64_t frame_target(void) {
    float fps = config_live_fps();
    if (!(fps >= 1.0f)) fps = 60.0f;  // (the settings not read yet)
    return (int64_t)(1e9 / fps * pct / 100.0);
}

static void *make_session(void *arg) {
    (void)arg;
    pthread_once(&load_once, load);
    int n = mk.n;
    APerformanceHintManager *m = NULL;
    if (!fn.get_manager || !fn.create_session || !fn.update_target || !fn.report) {
        LOG("[perf] ADPF unavailable: Android API %d (performance hint sessions are API 33)\n", android_get_device_api_level());
    } else if (!(m = fn.get_manager())) {
        LOG("[perf] ADPF unavailable: no performance hint manager\n");
    } else {
        APerformanceHintSession *s = n ? fn.create_session(m, mk.tids, (size_t)n, target_ns) : NULL;
        if (s) {
            if (fn.prefer_efficiency) fn.prefer_efficiency(s, false);
            LOG("[perf] ADPF session: %d thread%s (%s), target %.2f ms\n", n, n == 1 ? "" : "s", mk.list, target_ns / 1e6);
            atomic_store_explicit(&session, s, memory_order_release);
            return NULL;
        }
        // (-1: this device's power HAL has no hint sessions)
        LOG("[perf] ADPF unavailable: no session given for %s (Android API %d, preferred update rate %lld ns)\n", mk.list,
            android_get_device_api_level(), fn.preferred_rate ? (long long)fn.preferred_rate(m) : 0ll);
    }
    atomic_store_explicit(&unavailable, 1, memory_order_relaxed);
    return NULL;
}

// The session's threads set again (APerformanceHint_setThreads, API 34): those it has, a game or render thread new
// since, the translating threads that submitted in SUBMIT_FRAMES frames in a row; none that ended. Below API 34, or
// refused: it keeps the ones it has, and this isn't tried again.
static void regroup(APerformanceHintSession *s, int n) {
    static bool stuck;
    if (stuck) return;
    pid_t tids[MAX_THREADS];
    int k = 0;
    unsigned mask = 0;
    for (int i = 0; i < n; i++) {
        Thr *t = &thr[i];
        if ((t->in || (atomic_load(&t->roles) & CORE) || t->streak >= SUBMIT_FRAMES) && alive(t))
            tids[k++] = t->tid, mask |= 1u << i;
    }
    char list[200];
    thread_list(mask, list, sizeof list);
    int r = fn.set_threads && k ? fn.set_threads(s, tids, (size_t)k) : -1;
    if (!fn.set_threads)
        LOG("[perf] ADPF session keeps its threads: no APerformanceHint_setThreads below API 34 (for %s)\n", list);
    else if (r)
        LOG("[perf] ADPF session keeps its threads: %s not taken (%d)\n", list, r);
    else
        LOG("[perf] ADPF session: %d thread%s (%s)\n", k, k == 1 ? "" : "s", list);
    if (r) { stuck = true; return; }
    for (int i = 0; i < n; i++) thr[i].in = mask >> i & 1;
}

// MHGU_ADPF_LOG: the period's frames, reported work, each thread's CPU time per frame, thermal state
static void log_frame(uint64_t work, bool have_session) {
    static uint64_t t0, frames, work_sum, work_max;
    static AThermalManager *tm;
    static bool tm_tried;
    frames++, work_sum += work;
    if (work > work_max) work_max = work;
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    if (now - t0 < 2000000000ull) return;
    char line[600];
    int o = snprintf(line, sizeof line, "[perf] 2s: %llu frames, work %.2f ms avg %.2f max, target %.2f ms%s |",
                     (unsigned long long)frames, work_sum / 1e6 / frames, work_max / 1e6,
                     (have_session ? target_ns : frame_target()) / 1e6, have_session ? "" : " (no session)");
    int n = atomic_load_explicit(&nthr, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        char r[32];
        role_name(atomic_load(&thr[i].roles), r, sizeof r);
        if (o < (int)sizeof line - 80)
            o += snprintf(line + o, sizeof line - o, " %s %d%s %.2f/%.2f", r, (int)thr[i].tid,
                          thr[i].gone ? " (gone)" : have_session && !thr[i].in ? " (not in session)" : "",
                          thr[i].sum / 1e6 / frames, thr[i].max / 1e6);
        thr[i].sum = thr[i].max = 0;
    }
    if (!tm_tried) {  // (thermal service: a binder call, only while this log is on)
        tm_tried = true;
        pthread_once(&load_once, load);
        if (fn.thermal_manager) tm = fn.thermal_manager();
    }
    if (tm && o < (int)sizeof line - 80) {
        float h = fn.thermal_headroom ? fn.thermal_headroom(tm, 0) : NAN;  // (1.0: throttled hard, severe status)
        o += snprintf(line + o, sizeof line - o, " | thermal headroom %.2f, status %d", h,
                      fn.thermal_status ? fn.thermal_status(tm) : -1);
    }
    LOG("%s\n", line);
    t0 = now, frames = work_sum = work_max = 0;
}

#define EASY_FRAMES 60  // (frames looked at together)
#define EASY_RUNS 5     // (of them in a row, all easy, before the efficient cores)
#define EASY_RUNS_MAX 80
// A phone whose frames are easy on the fast cores but not on the efficient ones went back and forth all the time
// (a Unisoc T606: about 30 times in one session, each a few slow frames): each time they did not last a minute, the
// next try waits twice as long
static void efficiency_follow(APerformanceHintSession *s, int64_t work) {
    static int frames, easy_runs, off = -1, need = EASY_RUNS;
    static int64_t sum, longest;
    static bool efficient;
    static uint64_t on_since;
    if (off < 0) {
        const char *e = getenv("MHGU_ADPF_EFFICIENT");
        off = !fn.prefer_efficiency || (e && !strcmp(e, "0"));
    }
    if (off) return;
    sum += work, longest = work > longest ? work : longest;
    if (++frames < EASY_FRAMES) return;
    int64_t avg = sum / frames, target = target_ns;
    bool tight = avg > target * 7 / 10 || longest > target;
    easy_runs = avg < target / 2 && longest < target * 8 / 10 ? easy_runs + 1 : 0;
    frames = 0, sum = longest = 0;
    bool want = tight ? false : efficient || easy_runs >= need;
    if (want == efficient) return;
    efficient = want;
    fn.prefer_efficiency(s, want);
    uint64_t now = plat_now_ns();
    if (want) on_since = now, easy_runs = 0;
    else if (now - on_since < 60000000000ull) need = need * 2 > EASY_RUNS_MAX ? EASY_RUNS_MAX : need * 2;
    else need = EASY_RUNS;
    LOG("[perf] efficient cores %s (work %.1f ms of %.1f)%s\n", want ? "on" : "off", avg / 1e6, target / 1e6,
        want ? "" : need > EASY_RUNS ? ": the next try waits longer" : "");
}

// Once a minute (and at the first present): the process's memory map entries against vm.max_map_count, its resident
// memory and the system's available memory. Games that close after 5 to 15 minutes (PowerVR, a 3 GB phone) ran out of
// map entries (an mprotect said "out of memory" on a phone with 11 GB) or of memory: this shows whether either grows.
static long count_lines(const char *path, long *bytes) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[16384];
    long n = 0, total = 0;
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        total += r;
        for (ssize_t i = 0; i < r; i++) n += buf[i] == '\n';
    }
    close(fd);
    if (bytes) *bytes = total;
    return n;
}

static long kb_field(const char *path, const char *name) {
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    char line[256];
    long v = -1;
    size_t len = strlen(name);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, name, len)) { v = atol(line + len); break; }
    fclose(f);
    return v;
}

// (the error screen's log: what the minute's line says, now; open / read only, nothing that allocates)
void perf_mem_report(void) {
    long maps = count_lines("/proc/self/maps", NULL), rss = -1;
    char buf[4096];
    int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r > 0) {
            buf[r] = 0;
            const char *p = strstr(buf, "VmRSS:");
            if (p) rss = atol(p + 6);
        }
    }
    fprintf(stderr, "[mem] at the error: %ld map entries, resident %ld MB\n", maps, rss / 1024);
}

static void mem_watch(void) {
    static uint64_t next;
    static long max_maps = -1, first_maps = -1, first_rss = -1;
    uint64_t now = plat_now_ns();
    if (now < next) return;
    next = now + 60000000000ull;
    if (max_maps < 0) {
        FILE *f = fopen("/proc/sys/vm/max_map_count", "re");
        if (f) { if (fscanf(f, "%ld", &max_maps) != 1) max_maps = 0; fclose(f); }
        if (max_maps < 0) max_maps = 0;
    }
    long maps = count_lines("/proc/self/maps", NULL);
    long rss = kb_field("/proc/self/status", "VmRSS:"), avail = kb_field("/proc/meminfo", "MemAvailable:");
    if (first_maps < 0) first_maps = maps, first_rss = rss;
    void vk_mem_summary(char *out, size_t size);  // (nvn/vulkan/device.c)
    char gpu[96];
    vk_mem_summary(gpu, sizeof gpu);
    char of[32] = "";  // (apps may not read vm.max_map_count: 65530 is the kernel's default)
    if (max_maps > 0) snprintf(of, sizeof of, " of %ld", max_maps);
    LOG("[mem] %ld map entries%s (%+ld since the start), resident %ld MB (%+ld), the system's available %ld MB%s\n",
        maps, of, maps - first_maps, rss / 1024, (rss - first_rss) / 1024, avail / 1024, gpu);
}

void perf_hint_frame(void) {
    perf_hint_thread(PERF_RENDER);
    mem_watch();
    if (log_on < 0) {  // (the first present: the environment)
        const char *e = getenv("MHGU_ADPF_LOG");
        log_on = e && *e && strcmp(e, "0") != 0;
        e = getenv("MHGU_ADPF_TARGET");
        if (e && atoi(e) > 0) pct = atoi(e) < 25 ? 25 : atoi(e) > 400 ? 400 : atoi(e);
        e = getenv("MHGU_ADPF");
        if (e && !strcmp(e, "0")) {
            LOG("[perf] ADPF off (MHGU_ADPF=0)\n");
            atomic_store_explicit(&unavailable, 1, memory_order_relaxed);
        }
    }
    APerformanceHintSession *s = atomic_load_explicit(&session, memory_order_acquire);
    int n = atomic_load_explicit(&nthr, memory_order_acquire);
    // (the frame that ends here: the number the translating threads noted while it ran)
    uint32_t f = atomic_fetch_add_explicit(&presents, 1, memory_order_relaxed);
    if (!s && !making && !atomic_load_explicit(&unavailable, memory_order_relaxed) &&
        (atomic_load_explicit(&roles_known, memory_order_acquire) & CORE) == CORE) {
        // (the update thread's first frame came: the session is made with the game and render threads, those still
        // running; a translating thread joins it later, once it has shown it works in every frame)
        making = true;
        target_ns = frame_target();
        unsigned mask = 0;
        mk.n = 0;
        for (int i = 0; i < n; i++)
            if ((atomic_load(&thr[i].roles) & CORE) && alive(&thr[i]))
                thr[i].in = true, mask |= 1u << i, mk.tids[mk.n++] = thr[i].tid;
        thread_list(mask, mk.list, sizeof mk.list);
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &a, make_session, NULL) != 0)
            atomic_store_explicit(&unavailable, 1, memory_order_relaxed);
        pthread_attr_destroy(&a);
    }
    if (!s && !log_on) return;
    // the frame's work: the busiest session thread's CPU time since the last present (without a session yet: any's)
    uint64_t work = 0;
    bool again = false;  // the session's threads to be set again (regroup)
    for (int i = 0; i < n; i++) {
        Thr *t = &thr[i];
        struct timespec ts;
        if (t->gone) continue;
        if (clock_gettime(t->clock, &ts) != 0) { t->gone = true; again |= t->in; continue; }
        uint64_t c = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, d = t->last ? c - t->last : 0;
        t->last = c;
        t->sum += d;
        if (d > t->max) t->max = d;
        if (d > work && (!s || t->in)) work = d;
        if (!s || t->in) continue;
        if (atomic_load(&t->roles) & CORE) again = true;  // (a game or render thread new since the session was made)
        else if (atomic_load_explicit(&t->submitted, memory_order_relaxed) != f) t->streak = 0;
        else if (t->streak < SUBMIT_FRAMES && ++t->streak == SUBMIT_FRAMES) again = true;  // (one of every frame's now)
    }
    if (s && work) efficiency_follow(s, (int64_t)work);
    if (s) {
        int64_t tgt = frame_target();  // (the settings' FPS changed: the target follows)
        if (tgt != target_ns) {
            int r = fn.update_target(s, tgt);
            if (r) LOG("[perf] ADPF target %.2f ms not taken (%d)\n", tgt / 1e6, r);
            else if (log_on) LOG("[perf] ADPF target %.2f ms\n", tgt / 1e6);
            target_ns = tgt;
        }
        if (again) regroup(s, n);
        if (work) fn.report(s, (int64_t)work);
    }
    if (log_on) log_frame(work, s != NULL);
}

// ================================================================== Snapdragon 665 class devices (Adreno 610)
// No ADPF on Android 10 (it starts at API 33), so the system gets no hint about which threads matter: the shader
// prewarm's workers (3 compile + 1 translate threads, exec.c) then compete with the game and render threads for the
// four big cores (Cortex-A73 class) while the four little ones (Cortex-A53 class) idle. Work nobody waits for goes to
// the little cores at a lower priority; the jobs a draw waits for keep every core.
// Not measured on a device yet: MHGU_BG_THREADS=0 turns it off, MHGU_ADPF_LOG / MHGU_VK_STATS show the effect.

// SoCs with an Adreno 610: Snapdragon 665 (SM6125, board "trinket"), 662 (SM6115), 460 (SM4250), 680 (SM6225)
// (board "bengal"). The GPU's own name is only known once Vulkan is up; the settings are read before that.
bool plat_weak_soc(void) {
    static int v = -1;
    if (v >= 0) return v;
    const char *e = getenv("MHGU_SOC_PROFILE");
    if (e && *e) {
        v = !strcasecmp(e, "adreno610") || !strcmp(e, "1");
        LOG("[perf] MHGU_SOC_PROFILE=%s: Adreno 610 profile %s\n", e, v ? "on" : "off");
        return v;
    }
    char plat[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "";
    __system_property_get("ro.board.platform", plat);
    __system_property_get("ro.soc.model", model);
    static const char *const platforms[] = {"trinket", "bengal"};
    static const char *const models[] = {"SM6125", "SM6115", "SM4250", "SM6225"};
    int on = 0;
    for (size_t i = 0; i < sizeof platforms / sizeof *platforms; i++)
        if (!strcasecmp(plat, platforms[i])) on = 1;
    for (size_t i = 0; i < sizeof models / sizeof *models; i++)
        if (!strcasecmp(model, models[i])) on = 1;
    v = on;
    if (v) LOG("[perf] Adreno 610 class SoC (board %s, model %s): background work on the little cores, game threads on the big ones\n", plat, model[0] ? model : "?");
    return v;
}

static bool cpu_value(int cpu, const char *file, long *out) {
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/%s", cpu, file);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    long x;
    bool ok = fscanf(f, "%ld", &x) == 1;
    fclose(f);
    if (ok) *out = x;
    return ok;
}

// CPU sets are used through the raw system calls and a mask of our own: <sched.h>'s cpu_set_t needs _GNU_SOURCE before
// any system header, and the build force-includes volk.h (which pulls those in) ahead of this file's own define.
#define MASK_WORDS 16  // up to 1024 CPUs
typedef struct { unsigned long w[MASK_WORDS]; } CpuMask;
#define MASK_BITS (8 * (int)sizeof(unsigned long))
static void mask_set(CpuMask *m, int cpu) { m->w[cpu / MASK_BITS] |= 1ul << (cpu % MASK_BITS); }
static int mask_count(const CpuMask *m) {
    int n = 0;
    for (int i = 0; i < MASK_WORDS; i++) n += __builtin_popcountl(m->w[i]);
    return n;
}
static bool mask_get(CpuMask *m) {  // this thread's own
    memset(m, 0, sizeof *m);
    return syscall(SYS_sched_getaffinity, gettid(), sizeof *m, m->w) > 0;
}
static bool mask_apply(const CpuMask *m) { return syscall(SYS_sched_setaffinity, gettid(), sizeof *m, m->w) == 0; }

// The little cores: those with a clearly lower capacity (cpu_capacity, or the top frequency where the kernel gives no
// capacity: the Snapdragon 665's 1.8 against 2.0 GHz is 90%). False when they cannot be told, or fewer than two of
// either kind exist: then nothing is pinned.
static CpuMask found_big;  // (the others, set with the little ones: big_cores)
static bool little_cores(CpuMask *set) {
    static int known = -1;
    static CpuMask found;
    if (known < 0) {
        int n = (int)sysconf(_SC_NPROCESSORS_CONF);
        long v[MASK_WORDS * 64], top = 0;
        bool ok = n >= 4 && n <= MASK_WORDS * MASK_BITS;
        for (int i = 0; ok && i < n; i++) {
            ok = cpu_value(i, "cpu_capacity", &v[i]) || cpu_value(i, "cpufreq/cpuinfo_max_freq", &v[i]);
            if (ok && v[i] > top) top = v[i];
        }
        int little = 0, big = 0;
        memset(&found, 0, sizeof found);
        memset(&found_big, 0, sizeof found_big);
        for (int i = 0; ok && i < n; i++) {
            if (v[i] * 100 < top * 95) mask_set(&found, i), little++;
            else mask_set(&found_big, i), big++;
        }
        known = ok && little >= 2 && big >= 2;
        LOG("[perf] cores: %d little, %d big (%s)\n", little, big, known ? "background work goes to the little ones" : "no split: nothing pinned");
    }
    if (known) *set = found;
    return known;
}

// Raising the priority back needs RLIMIT_NICE 20 or more (Android's init sets 40); without it the thread would stay low.
static bool nice_restorable(void) {
    struct rlimit rl;
    return getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur >= 20;
}

void plat_background_work(bool on) {
    static __thread int cur;  // 0 as the system had it, 1 in the background
    static __thread CpuMask original;
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("MHGU_BG_THREADS");
        enabled = e && *e ? atoi(e) != 0 : plat_weak_soc();
    }
    if (!enabled || (on ? 1 : 0) == cur) return;
    pid_t tid = gettid();
    if (on) {
        CpuMask little;
        bool low_prio = nice_restorable(), pin = little_cores(&little);
        if (!low_prio && !pin) return;
        if (!mask_get(&original)) memset(&original, 0, sizeof original);
        if (pin && mask_count(&original)) mask_apply(&little);
        if (low_prio) setpriority(PRIO_PROCESS, (id_t)tid, 10);
        cur = 1;
    } else {
        if (mask_count(&original)) mask_apply(&original);
        if (nice_restorable()) setpriority(PRIO_PROCESS, (id_t)tid, 0);
        cur = 0;
    }
}

// The threads the frame rate rests on (the game's update thread, the presenting one, the one translating command lists:
// perf_hint_thread's roles) stay on the big cores on a Snapdragon 665 class phone. Android 10 gives them no hint (ADPF is
// API 33), and a thread that wakes every frame for a few milliseconds is what a scheduler leaves on a little core
// (Cortex-A53 class, about a third of a big one's speed here) after a quiet moment, until its load tracking catches up.
// Only the affinity changes (no priority: a game thread above the system's own would only starve the driver's threads).
// MHGU_HOT_PIN=0 turns it off. Not measured on a device yet: MHGU_ADPF_LOG=1 shows each thread's CPU time per frame.
static void hot_thread_to_big_cores(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("MHGU_HOT_PIN");
        enabled = e && *e ? atoi(e) != 0 : plat_weak_soc();
    }
    if (!enabled) return;
    CpuMask little, now, big;
    if (!little_cores(&little) || !mask_get(&now)) return;
    big = found_big;
    for (int i = 0; i < MASK_WORDS; i++) big.w[i] &= now.w[i];  // (only what this thread may use already)
    if (mask_count(&big) < 2 || !mask_apply(&big)) return;
    LOG("[perf] thread %d: %d big cores\n", (int)gettid(), mask_count(&big));
}
