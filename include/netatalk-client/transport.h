#ifndef NETATALK_CLIENT_TRANSPORT_H
#define NETATALK_CLIENT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct afpc_transport;

enum afpc_uam {
    AFPC_UAM_NO_USER_AUTH = 0x1,
    AFPC_UAM_CLEARTEXT = 0x2,
    AFPC_UAM_RANDNUM = 0x4,
    AFPC_UAM_TWO_WAY_RANDNUM = 0x8,
    AFPC_UAM_DHCAST128 = 0x10,
    AFPC_UAM_KERBEROS = 0x20,
    AFPC_UAM_DHX2 = 0x40,
    AFPC_UAM_RECONNECT = 0x80,
    AFPC_UAM_SRP = 0x100,
};

enum afpc_dsi_command {
    AFPC_DSI_COMMAND = 2,
    AFPC_DSI_WRITE = 6,
};

struct afpc_transport_options {
    const char *host;
    int port;
    int requested_version;
    const char *username;
    const char *password;
    unsigned int uam_mask;
};

typedef void (*afpc_transport_log_callback)(void *context, int loglevel,
        const char *message);

/* Initializes the process-wide libafpclient event loop and UAM registry.
 * Repeated calls update the logging callback without starting another loop. */
int afpc_transport_init(afpc_transport_log_callback callback, void *context);

unsigned int afpc_transport_default_uams(void);
unsigned int afpc_transport_uam_mask(const char *name);
const char *afpc_transport_uam_name(unsigned int mask);

int afpc_transport_connect(const struct afpc_transport_options *options,
                           struct afpc_transport **transport);
void afpc_transport_close(struct afpc_transport **transport);

unsigned int afpc_transport_supported_uams(
    const struct afpc_transport *transport);
unsigned int afpc_transport_using_uam(
    const struct afpc_transport *transport);
int afpc_transport_using_version(const struct afpc_transport *transport);
int afpc_transport_socket(const struct afpc_transport *transport);
unsigned int afpc_transport_tx_quantum(
    const struct afpc_transport *transport);
unsigned int afpc_transport_rx_quantum(
    const struct afpc_transport *transport);
unsigned int afpc_transport_attention_quantum(
    const struct afpc_transport *transport);

/* Refreshes the server's volume parameters. Returns zero on success and a
 * negative errno-style or AFP result on failure. */
int afpc_transport_get_server_parameters(struct afpc_transport *transport);

/* Sends one AFP payload through an established DSI session. The payload begins
 * with the AFP command byte; libafpclient owns DSI framing and request IDs.
 * A received AFP result, including a negative AFP error, is returned through
 * afp_result while the function itself returns zero. Transport failures return
 * a negative errno-style value. Reply data excludes the DSI header. */
int afpc_transport_raw_command(struct afpc_transport *transport,
                               const void *payload, size_t payload_len,
                               unsigned char dsi_command,
                               uint32_t data_offset, int timeout,
                               int *afp_result, void *reply,
                               size_t reply_capacity, size_t *reply_len);

#ifdef __cplusplus
}
#endif

#endif
