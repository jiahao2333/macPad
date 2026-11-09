// MACWS_MEMDIAG purgeable-memory census (WindowServer only).
//
// Purpose: attribute the task ledger `purgeable_nonvolatile` growth seen in
// docs/windowserver-memory-investigation.md (288 MB -> 1.4 GB + 1.6 GB
// compressed within ~15 s of opening Weather) to concrete kernel objects.
//
// Observational only.  Nothing is retained, released, made volatile or
// otherwise changed; the only extra kernel traffic is one
// IOSurface SetPurgeable(KeepCurrent) query per live surface per tick, which
// by definition does not change state, and a read-only VM region walk.
//
// Sources:
//   * IOConnectCallMethod_new / the IOSurface release adapter in
//     mac_hooks.m report every IOSurface user-client and IOGPU purgeable
//     call (selector, kr, scalar args) here.
//       IOSurface sel 0    = create (surface ID offset in the reply is
//                            discovered at runtime from IOSurfaceCreate)
//       IOSurface sel 1    = release(surfaceID)
//       IOSurface sel 0x14 = SetPurgeable(surfaceID, state) -> old state
//                            (identical ABI in macOS 13.4 and iOS 16.5,
//                            static disasm 2026-11-09)
//       IOGPU (iOS) 0x9 / 0xa / 0xc = resource create / finalize /
//                            SetPurgeable(resourceID, state) -> old
//   * IOSurfaceCreate_safe reports API-created surfaces (size/format/site).
//   * IOSurfaceSetPurgeable / IOSurfaceLookup* are interposed here to record
//     who asks for which purgeable state.
@import Foundation;
@import Darwin;
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurfaceRef.h>
// <mach/mach_vm.h> is marked unsupported in the iOS SDK; the MIG routines
// exist in libsystem_kernel on iOS 16.
extern kern_return_t mach_vm_region_recurse(vm_map_read_t, mach_vm_address_t *,
    mach_vm_size_t *, natural_t *, vm_region_recurse_info_t,
    mach_msg_type_number_t *);
extern kern_return_t mach_vm_purgable_control(vm_map_t, mach_vm_address_t,
    vm_purgable_t, int *);
#import <execinfo.h>
#import <dlfcn.h>
#import <pthread.h>
#import <stdatomic.h>

extern BOOL macws_memdiag_enabled(void);

#define PD_SURF_CAP 32768u          // power of two
#define PD_SITE_CAP 512u
#define PD_CONN_CAP 64u

enum { PD_ORIGIN_KCREATE = 0, PD_ORIGIN_API, PD_ORIGIN_LOOKUP, PD_ORIGIN_N };

typedef struct {
    uint32_t id;          // 0 = empty, UINT32_MAX = tombstone
    uint16_t refs;
    uint8_t origin;
    uint8_t lastSetState; // last successful explicit SetPurgeable, 0xff none
    uint64_t bytes;
    uint32_t pf, w, h;
    uint16_t site;
    uint16_t apiSite;     // last IOSurfaceSetPurgeable caller site
    uint32_t setCalls;
} pd_surface;

typedef struct {
    char name[200];
    uint64_t created, released;
    uint64_t setVolatile, setNonvolatile, setEmpty, setKeep;
} pd_site;

static pthread_mutex_t g_pd_lock = PTHREAD_MUTEX_INITIALIZER;
static pd_surface *g_pd_surf;
static pd_site g_pd_site[PD_SITE_CAP];
static unsigned g_pd_site_n;
static uint64_t g_pd_surf_overflow, g_pd_unknown_release;
static io_connect_t g_pd_surface_conn;
static int g_pd_id_offset = -1;     // byte offset of surfaceID in sel-0 reply
static __thread int t_pd_internal;  // our own census calls
static __thread uint8_t t_pd_create_reply[0x100];
static __thread size_t t_pd_create_reply_len;
static __thread int t_pd_create_reply_valid;

// Counters (protected by g_pd_lock).
static uint64_t g_pd_surf_sel_calls[64], g_pd_surf_sel_fail[64];
static uint64_t g_pd_surf_set[4][2];      // [state][ok?0:1]
static uint64_t g_pd_surf_old[4];
static IOReturn g_pd_surf_set_last_err;
static uint64_t g_pd_gpu_set[4][2], g_pd_gpu_old[4];
static IOReturn g_pd_gpu_set_last_err;
static uint64_t g_pd_gpu_create_ok, g_pd_gpu_create_fail, g_pd_gpu_final;
static uint64_t g_pd_api_set[4];

static struct { io_connect_t c; int kind; } g_pd_conn[PD_CONN_CAP];
enum { PD_KIND_OTHER = 1, PD_KIND_SURFACE, PD_KIND_IOGPU };

static int pd_enabled(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_acquire);
    if (v < 0) {
        const char *p = getprogname();
        v = macws_memdiag_enabled() && p && strstr(p, "WindowServer") ? 1 : 0;
        if (v) {
            g_pd_surf = calloc(PD_SURF_CAP, sizeof(pd_surface));
            if (!g_pd_surf) v = 0;
        }
        atomic_store_explicit(&cached, v, memory_order_release);
    }
    return v;
}

static pthread_mutex_t g_pd_conn_lock = PTHREAD_MUTEX_INITIALIZER;
static int pd_conn_kind_locked(io_connect_t c, int hint, void *ra);
static int pd_conn_kind(io_connect_t c, int hint, void *ra) {
    pthread_mutex_lock(&g_pd_conn_lock);
    int k = pd_conn_kind_locked(c, hint, ra);
    pthread_mutex_unlock(&g_pd_conn_lock);
    return k;
}
static const char *pd_base(const char *path);
// IOObjectGetClass() returns an empty name for user-client connect ports on
// this kernel (runtime 2026-11-09), so classify by the caller image instead:
// a connection used from the IOSurface framework is the IOSurface root user
// client; IOGPU connections are identified by mac_hooks.m.
static int pd_conn_kind_locked(io_connect_t c, int hint, void *ra) {
    for (unsigned i = 0; i < PD_CONN_CAP; i++) {
        if (g_pd_conn[i].c == c) {
            if (hint && g_pd_conn[i].kind != hint) g_pd_conn[i].kind = hint;
            return g_pd_conn[i].kind;
        }
        if (!g_pd_conn[i].c) break;
    }
    io_name_t cls = {0};
    int kind = hint;
    if (!kind) {
        Dl_info di;
        kind = PD_KIND_OTHER;
        if (ra && dladdr(ra, &di) && di.dli_fname &&
            !strcmp(pd_base(di.dli_fname), "IOSurface"))
            kind = PD_KIND_SURFACE;
    }
    if (kind == PD_KIND_OTHER) {
        // Do not cache "other": the first call on a connection may come from
        // an unrelated helper; re-evaluate (cheap dladdr) until classified.
        return kind;
    }
    IOObjectGetClass(c, cls);
    for (unsigned i = 0; i < PD_CONN_CAP; i++) {
        if (!g_pd_conn[i].c) {
            g_pd_conn[i].c = c; g_pd_conn[i].kind = kind;
            dprintf(STDERR_FILENO,
                "#### MACWS-PURGEDIAG conn=%u class=%s kind=%d\n", c, cls, kind);
            break;
        }
    }
    return kind;
}

static const char *pd_base(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static int pd_boring_image(const char *b) {
    return !strncmp(b, "libmachook", 10) || !strcmp(b, "IOSurface") ||
        !strcmp(b, "IOKit") || !strcmp(b, "libdyld.dylib") ||
        !strcmp(b, "libsystem_c.dylib");
}

// Called with g_pd_lock held.
static uint16_t pd_site_intern(const char *name) {
    for (unsigned i = 1; i < g_pd_site_n; i++)
        if (!strcmp(g_pd_site[i].name, name)) return (uint16_t)i;
    if (g_pd_site_n == 0) {
        snprintf(g_pd_site[0].name, sizeof(g_pd_site[0].name), "(other)");
        g_pd_site_n = 1;
    }
    if (g_pd_site_n >= PD_SITE_CAP) return 0;
    snprintf(g_pd_site[g_pd_site_n].name, sizeof(g_pd_site[0].name), "%s", name);
    return (uint16_t)g_pd_site_n++;
}

// Two interesting frames: first frame outside IOSurface/IOKit/libmachook,
// and the first frame outside the graphics stack as well.
// Caller chain: the first five frames outside IOSurface/IOKit/libmachook,
// as image+offset (symbolicate offline against the macOS dyld cache).
static void pd_site_name(char *out, size_t len, int skipFrames) {
    void *frames[16];
    int n = backtrace(frames, 16);
    size_t used = 0;
    int emitted = 0;
    out[0] = 0;
    for (int i = skipFrames; i < n && emitted < 5; i++) {
        Dl_info di;
        if (!dladdr(frames[i], &di) || !di.dli_fname) continue;
        const char *base = pd_base(di.dli_fname);
        if (pd_boring_image(base)) continue;
        uintptr_t off = (uintptr_t)frames[i] - (uintptr_t)di.dli_fbase;
        int w = snprintf(out + used, len - used, "%s%s+%#lx",
                         emitted ? "<" : "", base, (unsigned long)off);
        if (w < 0 || (size_t)w >= len - used) break;
        used += (size_t)w;
        emitted++;
    }
    if (!emitted) snprintf(out, len, "?");
}

static pd_surface *pd_find(uint32_t id, int create) {
    if (!g_pd_surf || !id || id == UINT32_MAX) return NULL;
    uint32_t h = (id * 2654435761u) & (PD_SURF_CAP - 1);
    pd_surface *tomb = NULL;
    for (unsigned probe = 0; probe < PD_SURF_CAP; probe++) {
        pd_surface *e = &g_pd_surf[(h + probe) & (PD_SURF_CAP - 1)];
        if (e->id == id) return e;
        if (e->id == UINT32_MAX) { if (!tomb) tomb = e; continue; }
        if (e->id == 0) {
            if (!create) return NULL;
            e = tomb ? tomb : e;
            memset(e, 0, sizeof(*e));
            e->id = id; e->lastSetState = 0xff;
            return e;
        }
    }
    if (create && tomb) {
        memset(tomb, 0, sizeof(*tomb));
        tomb->id = id; tomb->lastSetState = 0xff;
        return tomb;
    }
    if (create) g_pd_surf_overflow++;
    return NULL;
}

static void pd_fill_from_ref(pd_surface *e, IOSurfaceRef s) {
    e->bytes = IOSurfaceGetAllocSize(s);
    e->pf = IOSurfaceGetPixelFormat(s);
    e->w = (uint32_t)IOSurfaceGetWidth(s);
    e->h = (uint32_t)IOSurfaceGetHeight(s);
}

// ── Hooks from mac_hooks.m ──────────────────────────────────────────────────
void macws_purgediag_ioconnect(io_connect_t c, uint32_t selector,
                               const uint64_t *in, uint32_t inCnt,
                               const uint64_t *out, const uint32_t *outCnt,
                               const void *outStruct, const size_t *outStructCnt,
                               IOReturn r, int isIOGPU, void *ra) {
    if (!pd_enabled() || t_pd_internal) return;
    int kind = pd_conn_kind(c, isIOGPU == 1 ? PD_KIND_IOGPU :
                               isIOGPU == 2 ? PD_KIND_SURFACE : 0, ra);
    if (kind == PD_KIND_SURFACE) {
        char site[200] = "";
        int needSite = (selector == 0 && r == KERN_SUCCESS);
        if (needSite) pd_site_name(site, sizeof(site), 2);
        pthread_mutex_lock(&g_pd_lock);
        g_pd_surface_conn = c;
        if (selector < 64) {
            g_pd_surf_sel_calls[selector]++;
            if (r != KERN_SUCCESS) g_pd_surf_sel_fail[selector]++;
        }
        if (selector == 0 && r == KERN_SUCCESS && outStruct && outStructCnt) {
            size_t n = *outStructCnt < sizeof(t_pd_create_reply)
                ? *outStructCnt : sizeof(t_pd_create_reply);
            memcpy(t_pd_create_reply, outStruct, n);
            t_pd_create_reply_len = n;
            t_pd_create_reply_valid = 1;
            if (g_pd_id_offset >= 0 && (size_t)g_pd_id_offset + 4 <= n) {
                uint32_t id;
                memcpy(&id, (const uint8_t *)outStruct + g_pd_id_offset, 4);
                pd_surface *e = pd_find(id, 1);
                if (e) {
                    if (!e->refs) {
                        e->origin = PD_ORIGIN_KCREATE;
                        e->site = pd_site_intern(site);
                        g_pd_site[e->site].created++;
                    }
                    e->refs++;
                }
            }
        } else if (selector == 1 && inCnt >= 1 && in && r == KERN_SUCCESS) {
            pd_surface *e = pd_find((uint32_t)in[0], 0);
            if (e) {
                if (e->refs) e->refs--;
                if (!e->refs) {
                    if (e->site < PD_SITE_CAP) g_pd_site[e->site].released++;
                    e->id = UINT32_MAX;
                }
            } else g_pd_unknown_release++;
        } else if (selector == 0x14 && inCnt >= 2 && in) {
            uint32_t st = (uint32_t)in[1] & 3;
            g_pd_surf_set[st][r == KERN_SUCCESS ? 0 : 1]++;
            if (r != KERN_SUCCESS) g_pd_surf_set_last_err = r;
            if (r == KERN_SUCCESS && out && outCnt && *outCnt >= 1)
                g_pd_surf_old[out[0] & 3]++;
            pd_surface *e = pd_find((uint32_t)in[0], 0);
            if (e) {
                e->setCalls++;
                if (r == KERN_SUCCESS && st != 3) e->lastSetState = (uint8_t)st;
            }
        }
        pthread_mutex_unlock(&g_pd_lock);
    } else if (kind == PD_KIND_IOGPU) {
        if (selector != 0x9 && selector != 0xa && selector != 0xc) return;
        pthread_mutex_lock(&g_pd_lock);
        if (selector == 0x9) {
            if (r == KERN_SUCCESS) g_pd_gpu_create_ok++; else g_pd_gpu_create_fail++;
        } else if (selector == 0xa) {
            g_pd_gpu_final++;
        } else if (inCnt >= 2 && in) {
            uint32_t st = (uint32_t)in[1] & 3;
            g_pd_gpu_set[st][r == KERN_SUCCESS ? 0 : 1]++;
            if (r != KERN_SUCCESS) g_pd_gpu_set_last_err = r;
            if (r == KERN_SUCCESS && out && outCnt && *outCnt >= 1)
                g_pd_gpu_old[out[0] & 3]++;
        }
        pthread_mutex_unlock(&g_pd_lock);
    }
}

// Release delivered by the trap-pair adapter (calls the kernel directly, so
// IOConnectCallMethod_new never sees it).
void macws_purgediag_release(io_connect_t c, uint32_t surfaceID, IOReturn r) {
    uint64_t in = surfaceID;
    uint32_t zero = 0;
    macws_purgediag_ioconnect(c, 1, &in, 1, NULL, &zero, NULL, NULL, r, 2, NULL);
}

static void pd_track_ref(IOSurfaceRef s, int origin, int skipFrames) {
    if (!s) return;
    uint32_t id = IOSurfaceGetID(s);
    char site[200];
    pd_site_name(site, sizeof(site), skipFrames);
    pthread_mutex_lock(&g_pd_lock);
    if (origin == PD_ORIGIN_API && g_pd_id_offset < 0 &&
        t_pd_create_reply_valid) {
        for (size_t off = 0; off + 4 <= t_pd_create_reply_len; off += 4) {
            uint32_t v;
            memcpy(&v, t_pd_create_reply + off, 4);
            if (v == id) {
                g_pd_id_offset = (int)off;
                dprintf(STDERR_FILENO,
                    "#### MACWS-PURGEDIAG create-reply id_offset=%#zx "
                    "reply_len=%#zx id=%u\n", off, t_pd_create_reply_len, id);
                break;
            }
        }
    }
    t_pd_create_reply_valid = 0;
    pd_surface *e = pd_find(id, 1);
    if (e) {
        if (!e->refs) {
            // Not seen at kernel level (offset unknown yet / lookup).
            e->refs = 1;
            e->origin = (uint8_t)origin;
            e->site = pd_site_intern(site);
            g_pd_site[e->site].created++;
        } else if (e->origin == PD_ORIGIN_KCREATE && origin == PD_ORIGIN_API) {
            // Same object seen at both levels; keep one ref, better site.
            e->origin = PD_ORIGIN_API;
            uint16_t ns = pd_site_intern(site);
            if (ns != e->site) {
                if (g_pd_site[e->site].created) g_pd_site[e->site].created--;
                e->site = ns;
                g_pd_site[ns].created++;
            }
        } else if (origin == PD_ORIGIN_LOOKUP) {
            e->refs++;
        }
        pd_fill_from_ref(e, s);
    }
    pthread_mutex_unlock(&g_pd_lock);
}

IOSurfaceRef macws_purgediag_api_surface(IOSurfaceRef s) {
    if (s && pd_enabled() && !t_pd_internal)
        pd_track_ref(s, PD_ORIGIN_API, 2);
    return s;
}

// ── Interposes ──────────────────────────────────────────────────────────────
#define PD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { const void *r; const void *e; } \
    _pd_interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = \
    { (const void *)(unsigned long)&_replacement, \
      (const void *)(unsigned long)&_replacee };

static int pd_caller_is_us(void *ra) {
    Dl_info di;
    return dladdr(ra, &di) && di.dli_fname &&
        !strncmp(pd_base(di.dli_fname), "libmachook", 10);
}

static kern_return_t pd_IOSurfaceSetPurgeable(IOSurfaceRef s, uint32_t st,
                                              uint32_t *old) {
    kern_return_t r = IOSurfaceSetPurgeable(s, st, old);
    if (!pd_enabled() || t_pd_internal ||
        pd_caller_is_us(__builtin_return_address(0)) || !s)
        return r;
    char site[200];
    pd_site_name(site, sizeof(site), 1);
    uint32_t id = IOSurfaceGetID(s);
    pthread_mutex_lock(&g_pd_lock);
    g_pd_api_set[st & 3]++;
    uint16_t si = pd_site_intern(site);
    switch (st & 3) {
        case 0: g_pd_site[si].setNonvolatile++; break;
        case 1: g_pd_site[si].setVolatile++; break;
        case 2: g_pd_site[si].setEmpty++; break;
        default: g_pd_site[si].setKeep++; break;
    }
    pd_surface *e = pd_find(id, 0);
    if (e && (st & 3) != 3) e->apiSite = si;
    pthread_mutex_unlock(&g_pd_lock);
    return r;
}
PD_INTERPOSE(pd_IOSurfaceSetPurgeable, IOSurfaceSetPurgeable)

static IOSurfaceRef pd_IOSurfaceLookup(IOSurfaceID id) {
    IOSurfaceRef s = IOSurfaceLookup(id);
    if (s && pd_enabled() && !t_pd_internal &&
        !pd_caller_is_us(__builtin_return_address(0)))
        pd_track_ref(s, PD_ORIGIN_LOOKUP, 1);
    return s;
}
PD_INTERPOSE(pd_IOSurfaceLookup, IOSurfaceLookup)

static IOSurfaceRef pd_IOSurfaceLookupFromMachPort(mach_port_t port) {
    IOSurfaceRef s = IOSurfaceLookupFromMachPort(port);
    if (s && pd_enabled() && !t_pd_internal &&
        !pd_caller_is_us(__builtin_return_address(0)))
        pd_track_ref(s, PD_ORIGIN_LOOKUP, 1);
    return s;
}
PD_INTERPOSE(pd_IOSurfaceLookupFromMachPort, IOSurfaceLookupFromMachPort)

// ── Per-tick census ─────────────────────────────────────────────────────────
static double pd_mb(uint64_t b) { return (double)b / (1024.0 * 1024.0); }

static void pd_fourcc(uint32_t pf, char out[8]) {
    char c[4] = {(char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf};
    int printable = 1;
    for (int i = 0; i < 4; i++) if (c[i] < 32 || c[i] > 126) printable = 0;
    if (printable) snprintf(out, 8, "%.4s", c);
    else snprintf(out, 8, "%x", pf);
}

typedef struct { uint64_t key; uint64_t count, bytes, nvBytes; } pd_agg;

static void pd_agg_add(pd_agg *a, unsigned cap, uint64_t key, uint64_t bytes,
                       int nonvolatile) {
    for (unsigned i = 0; i < cap; i++) {
        if (a[i].count && a[i].key != key) continue;
        a[i].key = key; a[i].count++; a[i].bytes += bytes;
        if (nonvolatile) a[i].nvBytes += bytes;
        return;
    }
}

static int pd_agg_take_max(pd_agg *a, unsigned cap) {
    int best = -1;
    for (unsigned i = 0; i < cap; i++)
        if (a[i].count && (best < 0 || a[i].nvBytes > a[best].nvBytes ||
            (a[i].nvBytes == a[best].nvBytes && a[i].bytes > a[best].bytes)))
            best = (int)i;
    return best;
}

static void pd_vm_walk(unsigned long long tick) {
    mach_vm_address_t addr = 0;
    natural_t depth = 0;
    uint64_t vsz[4] = {0}, res[4] = {0}, regions[4] = {0};
    pd_agg tags[48] = {{0}};
    for (unsigned guard = 0; guard < 200000; guard++) {
        mach_vm_size_t size = 0;
        struct vm_region_submap_info_64 info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        int state = 0;
        if (mach_vm_purgable_control(mach_task_self(), addr,
                VM_PURGABLE_GET_STATE, &state) == KERN_SUCCESS) {
            int s = state & VM_PURGABLE_STATE_MASK;
            if (s > 3) s = 3;
            uint64_t r = (uint64_t)info.pages_resident * vm_page_size;
            vsz[s] += size; res[s] += r; regions[s]++;
            pd_agg_add(tags, 48, ((uint64_t)info.user_tag << 2) | (uint64_t)s,
                       r, s == VM_PURGABLE_NONVOLATILE);
        }
        addr += size;
    }
    dprintf(STDERR_FILENO,
        "#### MACWS-PURGEDIAG vmwalk tick=%llu nonvolatile=%lu/%.1fMB(res %.1fMB) "
        "volatile=%lu/%.1fMB(res %.1fMB) empty=%lu/%.1fMB(res %.1fMB)\n",
        tick, (unsigned long)regions[0], pd_mb(vsz[0]), pd_mb(res[0]),
        (unsigned long)regions[1], pd_mb(vsz[1]), pd_mb(res[1]),
        (unsigned long)regions[2], pd_mb(vsz[2]), pd_mb(res[2]));
    char line[768]; size_t len = 0;
    len += snprintf(line + len, sizeof(line) - len,
        "#### MACWS-PURGEDIAG vmwalk-tags tick=%llu (tag:state resMB)", tick);
    for (int k = 0; k < 10; k++) {
        int b = pd_agg_take_max(tags, 48);
        if (b < 0) break;
        // sort by resident bytes regardless of state
        len += snprintf(line + len, sizeof(line) - len, " %llu:%llu=%.1f",
            (unsigned long long)(tags[b].key >> 2),
            (unsigned long long)(tags[b].key & 3), pd_mb(tags[b].bytes));
        tags[b].count = 0;
        if (len >= sizeof(line) - 32) break;
    }
    dprintf(STDERR_FILENO, "%s\n", line);
}

void macws_purgediag_log(unsigned long long tick) {
    if (!pd_enabled()) return;
    // Snapshot live surfaces.
    typedef struct { uint32_t id; uint8_t origin, lastSet; uint16_t site, apiSite;
                     uint64_t bytes; uint32_t pf, w, h; } snap;
    unsigned cap = 8192, n = 0;
    snap *s = calloc(cap, sizeof(snap));
    if (!s) return;
    io_connect_t conn;
    int idOffset;
    uint64_t setc[4][2], oldc[4], gset[4][2], gold[4], apis[4];
    uint64_t selc[64], self[64];
    uint64_t gcreate, gcreatef, gfinal, overflow, unkrel;
    IOReturn lastErr, gLastErr;
    pthread_mutex_lock(&g_pd_lock);
    conn = g_pd_surface_conn; idOffset = g_pd_id_offset;
    for (unsigned i = 0; i < PD_SURF_CAP && n < cap; i++) {
        pd_surface *e = &g_pd_surf[i];
        if (!e->id || e->id == UINT32_MAX || !e->refs) continue;
        s[n++] = (snap){e->id, e->origin, e->lastSetState, e->site, e->apiSite,
                        e->bytes, e->pf, e->w, e->h};
    }
    memcpy(setc, g_pd_surf_set, sizeof(setc)); memcpy(oldc, g_pd_surf_old, sizeof(oldc));
    memcpy(gset, g_pd_gpu_set, sizeof(gset)); memcpy(gold, g_pd_gpu_old, sizeof(gold));
    memcpy(apis, g_pd_api_set, sizeof(apis));
    memcpy(selc, g_pd_surf_sel_calls, sizeof(selc));
    memcpy(self, g_pd_surf_sel_fail, sizeof(self));
    gcreate = g_pd_gpu_create_ok; gcreatef = g_pd_gpu_create_fail;
    gfinal = g_pd_gpu_final; overflow = g_pd_surf_overflow;
    unkrel = g_pd_unknown_release;
    lastErr = g_pd_surf_set_last_err; gLastErr = g_pd_gpu_set_last_err;
    pthread_mutex_unlock(&g_pd_lock);

    // Fill unknown sizes (kernel-created) via a temporary lookup, and query
    // the kernel's current purgeable state with KeepCurrent (no change).
    uint64_t stCount[5] = {0}, stBytes[5] = {0};   // 0..2 states, 3 keep?, 4 query failed
    uint64_t byOrigin[PD_ORIGIN_N][2] = {{0}};     // [origin][0=count,1=nvBytes]
    uint64_t mismatch = 0, sizeUnknown = 0;
    IOReturn qErr = 0;
    pd_agg *sites = calloc(PD_SITE_CAP, sizeof(pd_agg));
    pd_agg *fmts = calloc(256, sizeof(pd_agg));
    pd_agg *apiSites = calloc(PD_SITE_CAP, sizeof(pd_agg));
    t_pd_internal = 1;
    for (unsigned i = 0; i < n; i++) {
        if (!s[i].bytes) {
            IOSurfaceRef ref = IOSurfaceLookup(s[i].id);
            if (ref) {
                s[i].bytes = IOSurfaceGetAllocSize(ref);
                s[i].pf = IOSurfaceGetPixelFormat(ref);
                s[i].w = (uint32_t)IOSurfaceGetWidth(ref);
                s[i].h = (uint32_t)IOSurfaceGetHeight(ref);
                CFRelease(ref);
                pthread_mutex_lock(&g_pd_lock);
                pd_surface *e = pd_find(s[i].id, 0);
                if (e) { e->bytes = s[i].bytes; e->pf = s[i].pf;
                         e->w = s[i].w; e->h = s[i].h; }
                pthread_mutex_unlock(&g_pd_lock);
            } else sizeUnknown++;
        }
        int st = 4;
        {
            // KeepCurrent query through a temporary in-task lookup; never
            // changes the state.  (Direct selector calls need the framework's
            // private connection, which is not always known yet.)
            IOSurfaceRef ref = IOSurfaceLookup(s[i].id);
            if (ref) {
                uint32_t old = 0;
                IOReturn r = IOSurfaceSetPurgeable(ref,
                    kIOSurfacePurgeableKeepCurrent, &old);
                if (r == KERN_SUCCESS) st = (int)(old & 3); else qErr = r;
                CFRelease(ref);
            } else qErr = kIOReturnNotFound;
        }
        stCount[st]++; stBytes[st] += s[i].bytes;
        int nv = (st == 0);
        if (s[i].lastSet != 0xff && st <= 2 && s[i].lastSet != st) mismatch++;
        byOrigin[s[i].origin][0]++;
        if (nv) byOrigin[s[i].origin][1] += s[i].bytes;
        if (sites) pd_agg_add(sites, PD_SITE_CAP, s[i].site, s[i].bytes, nv);
        if (apiSites && s[i].apiSite)
            pd_agg_add(apiSites, PD_SITE_CAP, s[i].apiSite, s[i].bytes, nv);
        if (fmts) {
            uint64_t wb = s[i].w >= 2048 ? 2048 : s[i].w >= 1024 ? 1024 :
                          s[i].w >= 512 ? 512 : s[i].w >= 128 ? 128 : 0;
            uint64_t hb = s[i].h >= 2048 ? 2048 : s[i].h >= 1024 ? 1024 :
                          s[i].h >= 512 ? 512 : s[i].h >= 128 ? 128 : 0;
            pd_agg_add(fmts, 256, ((uint64_t)s[i].pf << 32) | (wb << 16) | hb,
                       s[i].bytes, nv);
        }
    }
    t_pd_internal = 0;

    task_vm_info_data_t vmi;
    mach_msg_type_number_t vmc = TASK_VM_INFO_COUNT;
    uint64_t ledgerNV = 0, ledgerNVc = 0, ledgerV = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &vmc) ==
        KERN_SUCCESS) {
        ledgerNV = vmi.ledger_purgeable_nonvolatile;
        ledgerNVc = vmi.ledger_purgeable_novolatile_compressed;
        ledgerV = vmi.ledger_purgeable_volatile;
    }
    uint64_t ownedNV = byOrigin[PD_ORIGIN_KCREATE][1] + byOrigin[PD_ORIGIN_API][1];
    dprintf(STDERR_FILENO,
        "#### MACWS-PURGEDIAG census tick=%llu conn=%u id_off=%d live=%u "
        "nonvolatile=%llu/%.1fMB volatile=%llu/%.1fMB empty=%llu/%.1fMB "
        "query_failed=%llu/%.1fMB(last %#x) size_unknown=%llu "
        "own_nv=%.1fMB import_nv=%.1fMB ledger_nv=%.1fMB+%.1fMBc ledger_vol=%.1fMB "
        "untracked_nv=%.1fMB state_mismatch=%llu overflow=%llu unknown_release=%llu\n",
        tick, conn, idOffset, n,
        (unsigned long long)stCount[0], pd_mb(stBytes[0]),
        (unsigned long long)stCount[1], pd_mb(stBytes[1]),
        (unsigned long long)stCount[2], pd_mb(stBytes[2]),
        (unsigned long long)stCount[4], pd_mb(stBytes[4]), qErr,
        (unsigned long long)sizeUnknown,
        pd_mb(ownedNV), pd_mb(byOrigin[PD_ORIGIN_LOOKUP][1]),
        pd_mb(ledgerNV), pd_mb(ledgerNVc), pd_mb(ledgerV),
        pd_mb(ledgerNV + ledgerNVc > ownedNV ? ledgerNV + ledgerNVc - ownedNV : 0),
        (unsigned long long)mismatch, (unsigned long long)overflow,
        (unsigned long long)unkrel);
    dprintf(STDERR_FILENO,
        "#### MACWS-PURGEDIAG origins tick=%llu kcreate=%llu/%.1fMBnv api=%llu/%.1fMBnv "
        "lookup=%llu/%.1fMBnv\n", tick,
        (unsigned long long)byOrigin[0][0], pd_mb(byOrigin[0][1]),
        (unsigned long long)byOrigin[1][0], pd_mb(byOrigin[1][1]),
        (unsigned long long)byOrigin[2][0], pd_mb(byOrigin[2][1]));
    dprintf(STDERR_FILENO,
        "#### MACWS-PURGEDIAG setpurgeable tick=%llu surf_kernel "
        "nv=%llu/%llu vol=%llu/%llu empty=%llu/%llu keep=%llu/%llu (ok/fail, last_err %#x) "
        "old[nv=%llu vol=%llu empty=%llu] api[nv=%llu vol=%llu empty=%llu keep=%llu] "
        "gpu nv=%llu/%llu vol=%llu/%llu empty=%llu/%llu keep=%llu/%llu (last_err %#x) "
        "gpu_old[nv=%llu vol=%llu empty=%llu] gpu_res create=%llu fail=%llu final=%llu\n",
        tick,
        (unsigned long long)setc[0][0], (unsigned long long)setc[0][1],
        (unsigned long long)setc[1][0], (unsigned long long)setc[1][1],
        (unsigned long long)setc[2][0], (unsigned long long)setc[2][1],
        (unsigned long long)setc[3][0], (unsigned long long)setc[3][1], lastErr,
        (unsigned long long)oldc[0], (unsigned long long)oldc[1], (unsigned long long)oldc[2],
        (unsigned long long)apis[0], (unsigned long long)apis[1],
        (unsigned long long)apis[2], (unsigned long long)apis[3],
        (unsigned long long)gset[0][0], (unsigned long long)gset[0][1],
        (unsigned long long)gset[1][0], (unsigned long long)gset[1][1],
        (unsigned long long)gset[2][0], (unsigned long long)gset[2][1],
        (unsigned long long)gset[3][0], (unsigned long long)gset[3][1], gLastErr,
        (unsigned long long)gold[0], (unsigned long long)gold[1], (unsigned long long)gold[2],
        (unsigned long long)gcreate, (unsigned long long)gcreatef,
        (unsigned long long)gfinal);
    {
        char line[1024]; size_t len = 0;
        len += snprintf(line + len, sizeof(line) - len,
            "#### MACWS-PURGEDIAG surf-sel tick=%llu", tick);
        for (int i = 0; i < 64; i++)
            if (selc[i] && len < sizeof(line) - 40)
                len += snprintf(line + len, sizeof(line) - len, " %#x=%llu/%llu",
                    i, (unsigned long long)selc[i], (unsigned long long)self[i]);
        dprintf(STDERR_FILENO, "%s\n", line);
    }
    if (sites) {
        for (int k = 0; k < 10; k++) {
            int b = pd_agg_take_max(sites, PD_SITE_CAP);
            if (b < 0) break;
            pthread_mutex_lock(&g_pd_lock);
            pd_site si = g_pd_site[sites[b].key < PD_SITE_CAP ? sites[b].key : 0];
            pthread_mutex_unlock(&g_pd_lock);
            dprintf(STDERR_FILENO,
                "#### MACWS-PURGEDIAG site tick=%llu live=%llu bytes=%.1fMB nv=%.1fMB "
                "created=%llu released=%llu set[nv=%llu vol=%llu empty=%llu] %s\n", tick,
                (unsigned long long)sites[b].count, pd_mb(sites[b].bytes),
                pd_mb(sites[b].nvBytes), (unsigned long long)si.created,
                (unsigned long long)si.released,
                (unsigned long long)si.setNonvolatile,
                (unsigned long long)si.setVolatile, (unsigned long long)si.setEmpty,
                si.name);
            sites[b].count = 0;
        }
    }
    if (apiSites) {
        for (int k = 0; k < 6; k++) {
            int b = pd_agg_take_max(apiSites, PD_SITE_CAP);
            if (b < 0) break;
            pthread_mutex_lock(&g_pd_lock);
            pd_site si = g_pd_site[apiSites[b].key < PD_SITE_CAP ? apiSites[b].key : 0];
            pthread_mutex_unlock(&g_pd_lock);
            dprintf(STDERR_FILENO,
                "#### MACWS-PURGEDIAG setter tick=%llu live=%llu bytes=%.1fMB nv=%.1fMB "
                "calls[nv=%llu vol=%llu empty=%llu] %s\n", tick,
                (unsigned long long)apiSites[b].count, pd_mb(apiSites[b].bytes),
                pd_mb(apiSites[b].nvBytes), (unsigned long long)si.setNonvolatile,
                (unsigned long long)si.setVolatile, (unsigned long long)si.setEmpty,
                si.name);
            apiSites[b].count = 0;
        }
    }
    if (fmts) {
        for (int k = 0; k < 10; k++) {
            int b = pd_agg_take_max(fmts, 256);
            if (b < 0) break;
            char cc[8];
            pd_fourcc((uint32_t)(fmts[b].key >> 32), cc);
            dprintf(STDERR_FILENO,
                "#### MACWS-PURGEDIAG fmt tick=%llu pf=%s w>=%llu h>=%llu live=%llu "
                "bytes=%.1fMB nv=%.1fMB\n", tick, cc,
                (unsigned long long)((fmts[b].key >> 16) & 0xffff),
                (unsigned long long)(fmts[b].key & 0xffff),
                (unsigned long long)fmts[b].count, pd_mb(fmts[b].bytes),
                pd_mb(fmts[b].nvBytes));
            fmts[b].count = 0;
        }
    }
    free(sites); free(fmts); free(apiSites); free(s);
    pd_vm_walk(tick);
}
