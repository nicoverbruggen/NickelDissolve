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

static void nds_config_write_default(void) {
    mkdir(NDS_CONFIG_DIR, 0755);

    FILE *src = fopen(NDS_CONFIG_DIR "/default", "r");
    if (!src) {
        NDS_LOG("warning: no default config template at %s/default (%s); leaving config absent", NDS_CONFIG_DIR_DISP, strerror(errno));
        return;
    }

    FILE *dst = fopen(NDS_CONFIG_DIR "/config", "w");
    if (!dst) {
        NDS_LOG("warning: could not write default config to %s/config (%s)", NDS_CONFIG_DIR_DISP, strerror(errno));
        fclose(src);
        return;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            NDS_LOG("warning: could not fully write default config to %s/config", NDS_CONFIG_DIR_DISP);
            break;
        }
    }

    fclose(src);
    fclose(dst);
    NDS_LOG("wrote default config to %s/config from template", NDS_CONFIG_DIR_DISP);
}

nds_config_t *nds_config_parse(void) {
    nds_config_t *cfg = (nds_config_t*)calloc(1, sizeof(nds_config_t));
    if (!cfg)
        return NULL;

    FILE *f = fopen(NDS_CONFIG_DIR "/config", "r");
    if (!f && errno == ENOENT) {
        NDS_LOG("no config file at %s/config; writing a default one", NDS_CONFIG_DIR_DISP);
        nds_config_write_default();
        f = fopen(NDS_CONFIG_DIR "/config", "r");
    }
    if (!f) {
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
