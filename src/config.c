#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "util.h"

typedef struct nds_config_entry_t {
    char *key;
    char *val;
    struct nds_config_entry_t *next;
} nds_config_entry_t;

struct nds_config_t {
    nds_config_entry_t *head;
    nds_config_entry_t *tail;
};

static void nds_config_append(nds_config_t *cfg, const char *key, const char *val) {
    nds_config_entry_t *e = (nds_config_entry_t*)calloc(1, sizeof(nds_config_entry_t));
    if (!e || !(e->key = strdup(key)) || !(e->val = strdup(val))) {
        NDS_LOG("warning: out of memory while parsing config, skipping '%s'", key);
        if (e) {
            free(e->key);
            free(e->val);
            free(e);
        }
        return;
    }

    if (cfg->tail)
        cfg->tail->next = e;
    else
        cfg->head = e;
    cfg->tail = e;
}

// Keys removed or renamed by the config rework (user keys vs nds_debug_* keys). Warn and ignore
// them so an old config file changes nothing silently — the log says what to rename.
static const struct { const char *key; const char *hint; } nds_legacy_keys[] = {
    { "nds_enabled",        "folded into nds_mode (off|observe|sweep)" },
    { "nds_strip_waveform", "renamed to nds_debug_strip_waveform" },
    { "nds_wait",           "renamed to nds_debug_wait" },
    { "nds_cfa_skip",       "renamed to nds_debug_cfa_skip" },
    { "nds_color_skip",     "renamed to nds_debug_color_skip" },
    { "nds_log_ioctl",      "renamed to nds_debug_log_ioctl" },
};

static const char *nds_legacy_hint(const char *key) {
    for (size_t i = 0; i < sizeof(nds_legacy_keys) / sizeof(nds_legacy_keys[0]); i++)
        if (!strcmp(key, nds_legacy_keys[i].key))
            return nds_legacy_keys[i].hint;
    return NULL;
}

// The config file is OPTIONAL and none is shipped: no file means the built-in defaults. A file
// only needs the keys being overridden; delete it to go back to the defaults.
nds_config_t *nds_config_parse(void) {
    nds_config_t *cfg = (nds_config_t*)calloc(1, sizeof(nds_config_t));
    if (!cfg)
        return NULL;

    FILE *f = fopen(NDS_CONFIG_DIR "/config", "r");
    if (!f) {
        if (errno == ENOENT)
            NDS_LOG("no config file at %s/config; using the defaults (create one to override)", NDS_CONFIG_DIR_DISP);
        else
            NDS_LOG("could not open %s/config (%s); using built-in defaults", NDS_CONFIG_DIR_DISP, strerror(errno));
        return cfg;
    }

    char *buf = NULL;
    size_t bufsz = 0;
    ssize_t len;
    int lineno = 0;
    while ((len = getline(&buf, &bufsz, f)) != -1) {
        (void)len;
        lineno++;

        char *hash = strchr(buf, '#');
        if (hash)
            *hash = '\0';

        char *line = strtrim(buf);
        if (!*line)
            continue;

        char *cur = line;
        char *key = strsep(&cur, ":");
        key = strtrim(key);
        if (!key || !*key) {
            NDS_LOG("warning: %s/config: line %d: expected key, ignoring line", NDS_CONFIG_DIR_DISP, lineno);
            continue;
        }
        if (!cur) {
            NDS_LOG("warning: %s/config: line %d: expected ':' after key '%s', ignoring line", NDS_CONFIG_DIR_DISP, lineno, key);
            continue;
        }

        const char *hint = nds_legacy_hint(key);
        if (hint) {
            NDS_LOG("warning: %s/config: line %d: legacy key '%s' (%s); ignoring", NDS_CONFIG_DIR_DISP, lineno, key, hint);
            continue;
        }

        char *val = strtrim(cur);
        nds_config_append(cfg, key, val);
        NDS_LOG("config: %s = %s", key, val);
    }

    free(buf);
    fclose(f);
    return cfg;
}

const char *nds_config_get(nds_config_t *cfg, const char *key) {
    if (!cfg)
        return NULL;
    for (nds_config_entry_t *e = cfg->head; e; e = e->next)
        if (!strcmp(e->key, key))
            return e->val;
    return NULL;
}

bool nds_config_bool(nds_config_t *cfg, const char *key, bool default_value) {
    const char *val = nds_config_get(cfg, key);
    if (!val || !*val)
        return default_value;
    if (!strcmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
        return true;
    if (!strcmp(val, "0") || !strcasecmp(val, "false") || !strcasecmp(val, "no") || !strcasecmp(val, "off"))
        return false;

    NDS_LOG("warning: invalid boolean for '%s': '%s'; using default %d", key, val, default_value ? 1 : 0);
    return default_value;
}

double nds_config_double(nds_config_t *cfg, const char *key, double default_value) {
    const char *val = nds_config_get(cfg, key);
    if (!val || !*val)
        return default_value;

    char *end = NULL;
    errno = 0;
    double parsed = strtod(val, &end);
    bool trailing = false;
    for (const char *p = end; p && *p; p++)
        if (!isspace((unsigned char)*p)) { trailing = true; break; }
    if (errno || end == val || trailing) {
        NDS_LOG("warning: invalid number for '%s': '%s'; using default %.4f", key, val, default_value);
        return default_value;
    }
    return parsed;
}

void nds_config_free(nds_config_t *cfg) {
    if (!cfg)
        return;

    nds_config_entry_t *e = cfg->head;
    while (e) {
        nds_config_entry_t *next = e->next;
        free(e->key);
        free(e->val);
        free(e);
        e = next;
    }
    free(cfg);
}

static nds_config_t *nds_global_config(void) {
    static nds_config_t *global = NULL;
    if (!global)
        global = nds_config_parse();
    return global;
}

const char *nds_global_config_get(const char *key) {
    return nds_config_get(nds_global_config(), key);
}

bool nds_global_config_bool(const char *key, bool default_value) {
    return nds_config_bool(nds_global_config(), key, default_value);
}

double nds_global_config_double(const char *key, double default_value) {
    return nds_config_double(nds_global_config(), key, default_value);
}
