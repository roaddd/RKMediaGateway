#ifndef __DEF_VALUE_H__
#define __DEF_VALUE_H__

#include "mediaGateway.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the required TOML file and apply its values over the compiled defaults. */
int def_value_init(const char *config_path);

/* Fill the whole gateway config in one call. */
void def_value_get_media_gateway_config(MediaGatewayConfig *config);

/* Whether the last init loaded a config file successfully. */
int def_value_loaded(void);

/* Config path used by the last init. */
const char *def_value_source_path(void);

#ifdef __cplusplus
}
#endif

#endif
