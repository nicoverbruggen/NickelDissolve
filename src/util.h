#ifndef NDS_UTIL_H
#define NDS_UTIL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <NickelHook.h>

// The mod version, baked in by NickelHook.mk (git describe). Logged on every line and in the
// startup block, so a user-attached log always says exactly which build produced it.
#ifndef NH_VERSION
#define NH_VERSION "dev"
#endif

// Cap the on-device log so it can't grow without bound across many boots. On the first write of
// a boot, if the log is larger than this it's rotated to a single ".old" generation. A healthy
// supported device is quiet (only the startup block), so this is reached only by a long-lived or
// deliberately-verbose device.
#ifndef NDS_LOG_MAX_BYTES
#define NDS_LOG_MAX_BYTES (256 * 1024)
#endif

// Verbose (per-ioctl / per-turn) tracing switch. Published once by nds_init after the config is
// parsed: true in "observe" mode, when nds_log:1 is set, or when the config had a problem —
// false on a healthy device in "sweep"/"off". Gates all hot-path logging so supported devices
// stay quiet. Defined in nickeldissolve.cc.
extern bool nds_verbose_enabled;

__attribute__((unused)) static inline char *strtrim(char *s) {
    if (!s)
        return NULL;

    char *a = s;
    char *b = s + strlen(s);
    for (; a < b && isspace((unsigned char)(*a)); a++);
    for (; b > a && isspace((unsigned char)(*(b - 1))); b--);
    *b = '\0';
    return a;
}

__attribute__((unused)) static inline void nds_log_file_line(const char *file, int line, const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';

    nh_log("%s (%s:%d)", msg, file, line);

    mkdir(NDS_CONFIG_DIR, 0755);

    // Rotate once per process, on the first file write of the boot. A benign race if two threads
    // hit this first (at most a redundant rename); the flag keeps it to one check per process.
    static bool nds_log_rotate_checked = false;
    if (!nds_log_rotate_checked) {
        nds_log_rotate_checked = true;
        struct stat st;
        if (stat(NDS_CONFIG_DIR "/nickel-dissolve.log", &st) == 0 && st.st_size > NDS_LOG_MAX_BYTES)
            rename(NDS_CONFIG_DIR "/nickel-dissolve.log", NDS_CONFIG_DIR "/nickel-dissolve.log.old");
    }

    FILE *f = fopen(NDS_CONFIG_DIR "/nickel-dissolve.log", "a");
    if (!f)
        return;

    // localtime_r, not localtime: logging happens on whatever thread hit a problem (the ioctl
    // hook runs on Nickel's render/UI threads), and localtime's shared static buffer is a data
    // race between concurrent callers.
    time_t now = time(NULL);
    struct tm tmbuf;
    struct tm *tm = localtime_r(&now, &tmbuf);
    if (tm) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    fprintf(f, "NickelDissolve " NH_VERSION ": %s (%s:%d)\n", msg, file, line);
    fclose(f);
}

// Always written (problems, safety, startup block). A healthy boot writes only the startup block.
#define NDS_LOG(fmt, ...) nds_log_file_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

// Verbose tracing — written only when nds_verbose_enabled (observe mode / nds_log:1 / config
// problem). The hot-path sweep/ioctl traces gate on nds_verbose_enabled directly (they also
// rate-limit); this macro is for one-off verbose lines.
#define NDS_DBG(fmt, ...) do { if (nds_verbose_enabled) NDS_LOG(fmt, ##__VA_ARGS__); } while (0)

// Log the running firmware version once at startup, next to the mod version and resolved hooks,
// so a future-firmware breakage report shows which firmware ran. The serial number (field 0 of
// /mnt/onboard/.kobo/version) is deliberately dropped. Diagnostics only — failures are silent.
__attribute__((unused)) static inline void nds_log_firmware(void) {
    FILE *f = fopen("/mnt/onboard/.kobo/version", "r");
    if (!f) {
        NDS_LOG("startup: firmware version unavailable");
        return;
    }
    char vline[512];
    char *got = fgets(vline, sizeof(vline), f);
    fclose(f);
    if (!got)
        return;
    vline[strcspn(vline, "\r\n")] = '\0';
    const char *comma = strchr(vline, ',');   // <serial>,<...>,<firmware>,<model>,...
    NDS_LOG("startup: firmware %s", comma ? comma + 1 : vline);
}

#ifdef __cplusplus
}
#endif
#endif
