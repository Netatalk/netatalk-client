#include <errno.h>
#include <stddef.h>

#include <netatalk-client/transport.h>

static void discard_log(void *context, int loglevel, const char *message)
{
    (void)context;
    (void)loglevel;
    (void)message;
}

int main(void)
{
    struct afpc_transport_options options = {0};
    struct afpc_transport *transport = NULL;

    if (afpc_transport_init(discard_log, NULL) != 0) {
        return 1;
    }

    if (afpc_transport_default_uams() == 0) {
        return 2;
    }

    if (afpc_transport_uam_mask("guest") != AFPC_UAM_NO_USER_AUTH) {
        return 3;
    }

    if (afpc_transport_supported_uams(NULL) != 0
            || afpc_transport_using_uam(NULL) != 0
            || afpc_transport_using_version(NULL) != 0
            || afpc_transport_socket(NULL) != -1) {
        return 4;
    }

    if (afpc_transport_connect(&options, &transport) != -EINVAL) {
        return 5;
    }

    if (afpc_transport_raw_command(NULL, NULL, 0, AFPC_DSI_COMMAND,
                                   0, 0, NULL, NULL, 0, NULL) != -EINVAL) {
        return 6;
    }

    if (afpc_transport_get_server_parameters(NULL) != -EINVAL) {
        return 7;
    }

    afpc_transport_close(&transport);
    return 0;
}
