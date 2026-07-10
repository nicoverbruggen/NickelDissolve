#ifndef NDS_CONFIG_H
#define NDS_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#if !(defined(NDS_CONFIG_DIR) && defined(NDS_CONFIG_DIR_DISP))
#error "NDS_CONFIG_DIR not set (it should be done by the Makefile)"
#endif

typedef struct nds_config_t nds_config_t;

nds_config_t *nds_config_parse(void);
const char *nds_config_get(nds_config_t *cfg, const char *key);
bool nds_config_bool(nds_config_t *cfg, const char *key, bool default_value);
double nds_config_double(nds_config_t *cfg, const char *key, double default_value);
void nds_config_free(nds_config_t *cfg);
const char *nds_global_config_get(const char *key);
bool nds_global_config_bool(const char *key, bool default_value);
double nds_global_config_double(const char *key, double default_value);

#ifdef __cplusplus
}
#endif
#endif
