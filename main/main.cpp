#include <stdio.h>

#include "defValue.h"
#include "logger.h"

extern "C"
{
#include "mediaGateway.h"
#include "debugCommandServer.h"
#include "systemDebug.h"
}

int main(int argc, char **argv)
{
    MediaGatewayCtx gateway;
    MediaGatewayConfig config;
    const char *config_path = (argc > 1 && argv[1] && argv[1][0] != '\0') ? argv[1] : "media_gateway.conf";
    int shell_ready = 0;
    int ret = 0;

    def_value_init(config_path);
    def_value_get_media_gateway_config(&config);
    log_set_level((LogLevel)config.system.log.level);
    if (def_value_loaded())
    {
        LOG_INFO("loaded config file: %s", def_value_source_path());
    }
    else
    {
        LOG_WARN("config file not found, fallback to defaults: %s", def_value_source_path());
    }

    if (debug_command_server_init(NULL) == 0)
    {
        shell_ready = 1;
        if (system_debug_init() != 0)
        {
            LOG_WARN("system debug init failed");
        }
    }
    else
    {
        LOG_WARN("shell command server init failed, runtime shell debug disabled");
    }

    if (media_gateway_init(&gateway, &config) < 0)
    {
        if (shell_ready)
            debug_command_server_deinit();
        return -1;
    }

    if (shell_ready && debug_command_server_start() != 0)
    {
        LOG_WARN("shell command server start failed, runtime shell debug disabled");
    }

    ret = media_gateway_run(&gateway);

    media_gateway_deinit(&gateway);
    if (shell_ready)
        debug_command_server_deinit();
    return ret;
}
