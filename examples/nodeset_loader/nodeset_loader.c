/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

#include <open62541/plugin/log_stdout.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/nodesetloader.h>

UA_Boolean running = true;
static void stopHandler(int sign) {
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "received ctrl-c");
    running = false;
}

int main(int argc, const char *argv[]) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    UA_Server *server = UA_Server_new();
    UA_ServerConfig_setDefault(UA_Server_getConfig(server));
    UA_Server_run_startup(server);

    for (int cnt = 1; cnt < argc; cnt++) {
        if (UA_StatusCode_isBad(UA_Server_loadNodeset(server, argv[cnt], NULL))) {
            printf("Nodeset %s could not be loaded, exit\n", argv[cnt]);
            return EXIT_FAILURE;
        }
    }

    UA_Server_run_shutdown(server);
    UA_Server_delete(server);

    return EXIT_SUCCESS;
}
