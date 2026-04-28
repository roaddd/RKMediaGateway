#include <stdio.h>

#include "defValue.h"

extern "C"
{
#include "mediaGateway.h"
}

int main(int argc, char **argv)
{
    MediaGatewayCtx gateway;
    MediaGatewayConfig config;
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "rtsp_gateway.conf";

    def_value_init(config_path);
    if (def_value_loaded())
    {
        printf("[INFO] loaded config file: %s\n", def_value_source_path());
    }
    else
    {
        printf("[WARN] config file not found, fallback to defaults: %s\n", def_value_source_path());
    }

    def_value_get_media_gateway_config(&config);
    if (media_gateway_init(&gateway, &config) < 0)
    {
        return -1;
    }

    int ret = media_gateway_run(&gateway);

    media_gateway_deinit(&gateway);
    return ret;
}
