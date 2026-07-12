#include <netatalk-client/afpsl.h>
#include <netatalk-client/transport.h>
#include <netatalk-client/types.h>
#include <netatalk-client/url.h>

int main(void)
{
    struct afpc_url url;
    const afpc_server_t *server = NULL;
    const afpc_volume_t *volume = NULL;
    afp_sl_url_init(&url);

    if (url.protocol != AFPC_TRANSPORT_TCPIP || afp_sl_default_uams() == 0) {
        return 1;
    }

    (void)server;
    (void)volume;
    return 0;
}
