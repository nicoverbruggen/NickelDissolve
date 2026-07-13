#ifndef NDS_UTIL_H
#define NDS_UTIL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <ctype.h>
#include <dirent.h>
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

// NTX hwconfig board fingerprint. Every NTX-built Kobo (both the i.MX/mxcfb and the MTK/hwtcon
// families) carries a "HW CONFIG " blob at offset 0x80000 on the internal MMC. After the header
// (magic[10] + version[5] + size[1]) the NTXHWCFG_VAL struct starts at byte 16, one byte per field
// (offsets per NTX's arch/arm/mach-mx6/ntx_hwconfig.h). We decode the fields that identify the board
// and its revision into a labelled table, so a page-turn-quality report says exactly which board
// variant it came from (the Kobo Libra 2 alone ships in several revisions with different EPD panels,
// PMICs and waveforms). Notably bDisplayCtrl (25) encodes the EPD controller + EPD PMIC combo (e.g. a
// "...+SY7636" part vs the JD9930 on the F revision), and bPCB_REV (57) is the literal revision
// counter u-boot / the NTX updater key off. A raw window is logged too, so anything can be re-decoded
// from a log alone. Read-only, diagnostics only, silent on any failure.
__attribute__((unused)) static inline void nds_log_hwconfig(void) {
    FILE *f = fopen("/dev/mmcblk0", "rb");
    if (!f)
        return;
    unsigned char b[80];
    size_t n = 0;
    if (fseek(f, 0x80000L, SEEK_SET) == 0)
        n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n < 63 || memcmp(b, "HW CONFIG ", 10) != 0)   // no (usable) NTX config block at this offset
        return;

    char ver[6];
    memcpy(ver, b + 10, 5);
    ver[5] = '\0';
    for (int i = 0; i < 5; i++)                       // version is a printable "vX.Y" string
        if (ver[i] < 32 || ver[i] > 126) { ver[i] = '\0'; break; }

    // VCOM is a signed value in 10 mV units, high byte first (bVCOM_10mV_HiByte/LoByte at 55/56).
    int vcom_raw = (b[55] << 8) | b[56];
    int vcom_mv = (vcom_raw >= 0x8000 ? vcom_raw - 0x10000 : vcom_raw) * 10;

    NDS_LOG("startup: ---- NTX hardware revision (HW CONFIG %s) ----", ver);
    NDS_LOG("startup:   PCB / model         [16] = %u", (unsigned)b[16]);
    NDS_LOG("startup:   PCB revision        [57] = %u", (unsigned)b[57]);
    NDS_LOG("startup:   PCB level           [58] = %u", (unsigned)b[58]);
    NDS_LOG("startup:   EPD controller+PMIC [25] = %u", (unsigned)b[25]);
    NDS_LOG("startup:   EPD panel           [26] = %u", (unsigned)b[26]);
    NDS_LOG("startup:   EPD VCOM         [55/56] = %d mV (raw 0x%04x)", vcom_mv, (unsigned)vcom_raw);
    NDS_LOG("startup:   display resolution  [47] = %u", (unsigned)b[47]);
    NDS_LOG("startup:   frontlight          [48] = %u", (unsigned)b[48]);
    NDS_LOG("startup:   frontlight LED drv  [54] = %u", (unsigned)b[54]);
    NDS_LOG("startup:   system PMIC         [60] = %u", (unsigned)b[60]);

    char hex[3 * 53 + 1];
    int p = 0;
    for (int i = 10; i < 63; i++)
        p += snprintf(hex + p, sizeof(hex) - (size_t)p, "%02x ", b[i]);
    if (p > 0)
        hex[p - 1] = '\0';
    NDS_LOG("startup:   raw[10..62]              = %s", hex);
}

// One-shot raw-flash region dump, for offline analysis when the on-flash waveform blob's exact MMC
// offset is unknown (it varies by board, and the u-boot update offset isn't where the running system
// keeps it). Copies [offset, offset+len) from /dev/mmcblk0 to `path` — meant to be a file on the
// onboard partition so it can be pulled over USB and parsed on a real machine. Gated by the caller
// behind a debug config key so it never runs on a normal device. Read-only w.r.t. flash; silent on
// failure apart from one summary log line.
__attribute__((unused)) static inline void nds_dump_flash(long offset, long len, const char *path) {
    FILE *in = fopen("/dev/mmcblk0", "rb");
    if (!in)
        return;
    FILE *out = fopen(path, "wb");
    if (!out) {
        fclose(in);
        return;
    }
    long written = 0;
    if (fseek(in, offset, SEEK_SET) == 0) {
        char buf[65536];
        long remaining = len;
        while (remaining > 0) {
            size_t want = remaining > (long)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
            size_t got = fread(buf, 1, want, in);
            if (got == 0)
                break;
            written += (long)fwrite(buf, 1, got, out);
            remaining -= (long)got;
        }
    }
    fclose(out);
    fclose(in);
    NDS_LOG("startup: dumped flash 0x%lx +0x%lx (%ld bytes) -> %s", offset, len, written, path);
}

// EPD waveform fingerprint. Which waveform "modes" a panel supports — e.g. whether a plain
// non-inverting GL16 exists alongside the Regal/GLKW16 the Libra 2 uses for reading turns — is
// panel-specific and is NOT in the device tree; it lives in the panel's waveform blob on flash and
// in the eink driver. SSH to some of these devices is unreliable, so the mod logs it directly. Three
// sources, so a page-turn-quality report says exactly what the panel can do:
//   1. the driver's reported waveform version string;
//   2. everything the driver exposes under /proc/eink/waveform (names + small text values — this is
//      where a mode count / temperature table typically surfaces, and the file names vary, so we
//      enumerate rather than guess);
//   3. the raw header of the on-flash waveform blob. On the Libra 2 (Netronix E70K10) u-boot flashes
//      the waveform header at sector 16415 (offset 0x803E00). The .wbf header carries fpl_platform,
//      waveform version/type and the mode/temperature counts; we log the raw bytes plus a decode of
//      the stable low fields (layout per fread-ink/inkwave). filesize sanity-checks the offset.
// Read-only, diagnostics only, silent on any failure.
__attribute__((unused)) static inline void nds_log_waveform(void) {
    FILE *f = fopen("/proc/eink/waveform/waveform_version", "r");
    if (f) {
        char v[256];
        if (fgets(v, sizeof(v), f)) {
            v[strcspn(v, "\r\n")] = '\0';
            NDS_LOG("startup: eink waveform_version=%s", v);
        }
        fclose(f);
    }

    DIR *d = opendir("/proc/eink/waveform");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            if (strcmp(de->d_name, "waveform_version") == 0)   // already logged above
                continue;
            char path[512];
            snprintf(path, sizeof(path), "/proc/eink/waveform/%s", de->d_name);
            FILE *pf = fopen(path, "r");
            if (!pf) {
                NDS_LOG("startup: eink wf/%s (unreadable)", de->d_name);
                continue;
            }
            char buf[256];
            size_t n = fread(buf, 1, sizeof(buf) - 1, pf);
            fclose(pf);
            buf[n] = '\0';
            for (size_t i = 0; i < n; i++)                     // flatten to one printable line
                if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')
                    buf[i] = ' ';
            NDS_LOG("startup: eink wf/%s=%s", de->d_name, buf);
        }
        closedir(d);
    }

    // Locate and decode the panel's waveform blob (an E-Ink ".wbf") on raw flash, and log the panel's
    // *capabilities* — how many waveform modes it carries and over how many temperature ranges. The
    // blob's exact MMC offset varies by board and isn't the u-boot update offset (on the Libra 2 the
    // running waveform sits at 0x700000, not the 0x804000 the updater writes to), so we scan
    // 4KB-aligned candidates and validate each as a genuine .wbf header: a plausible filesize, a sane
    // mode/temperature count, a non-zero checksum, and — the strong signal — a strictly-increasing
    // temperature table at header offset 0x30. The mode count is what tells us whether a non-inverting
    // mode (GL16, index 5) exists alongside the Regal mode used for reading turns. Read-only, silent.
    f = fopen("/dev/mmcblk0", "rb");
    if (f) {
        unsigned char b[80];
        long found = -1;
        unsigned fsz = 0, modes = 0, temps = 0, ver = 0, subver = 0, serial = 0;
        for (long off = 0x100000L; off <= 0x1000000L; off += 0x1000L) {
            if (fseek(f, off, SEEK_SET) != 0)
                break;
            if (fread(b, 1, sizeof(b), f) < 64)
                break;
            unsigned filesize = (unsigned)b[4] | ((unsigned)b[5] << 8) |
                                ((unsigned)b[6] << 16) | ((unsigned)b[7] << 24);
            unsigned checksum = (unsigned)b[0] | ((unsigned)b[1] << 8) |
                                ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
            unsigned m = (unsigned)b[0x25] + 1u;   // b[37] = mode count - 1
            unsigned t = (unsigned)b[0x26] + 1u;   // b[38] = temperature range count - 1
            if (checksum == 0u) continue;
            if (filesize < 0x10000u || filesize > 0x200000u) continue;   // 64 KB .. 2 MB
            if (m < 4u || m > 24u || t < 4u || t > 32u) continue;
            bool rising = true;                    // temperature table at 0x30 strictly increases
            for (unsigned i = 0; i + 1u < t && (0x30u + i + 1u) < sizeof(b); i++)
                if (b[0x30 + i + 1] <= b[0x30 + i]) { rising = false; break; }
            if (!rising) continue;
            found = off; fsz = filesize; modes = m; temps = t;
            ver = b[17]; subver = b[18];
            serial = (unsigned)b[8] | ((unsigned)b[9] << 8) | ((unsigned)b[10] << 16) | ((unsigned)b[11] << 24);
            break;
        }
        fclose(f);
        if (found >= 0) {
            NDS_LOG("startup: ---- EPD waveform capabilities (.wbf @ 0x%lx) ----", found);
            NDS_LOG("startup:   waveform version    = %u.%u (serial %u, %u bytes)", ver, subver, serial, fsz);
            NDS_LOG("startup:   waveform modes       = %u  (indices 0..%u)", modes, modes - 1u);
            NDS_LOG("startup:   temperature ranges   = %u", temps);
            char tt[4 * 24 + 1];
            int p = 0;
            for (unsigned i = 0; i < temps && i < 24u && (0x30u + i) < sizeof(b); i++)
                p += snprintf(tt + p, sizeof(tt) - (size_t)p, "%u ", (unsigned)b[0x30 + i]);
            if (p > 0)
                tt[p - 1] = '\0';
            NDS_LOG("startup:   temp range table (C) = %s", tt);
        } else {
            NDS_LOG("startup: EPD waveform .wbf not found on flash (scanned 0x100000..0x1000000)");
        }
    }
}

#ifdef __cplusplus
}
#endif
#endif
