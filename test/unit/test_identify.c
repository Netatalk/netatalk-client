/*
    test_identify.c: the identification of servers and its predicates

    Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <stdio.h>
#include <string.h>

#include "lib/afp_internal.h"
#include "lib/client.h"
#include "lib/compat.h"
#include "tap.h"

static void stderr_log_for_client(
    void *priv _U_,
    enum logtypes logtype _U_,
    int loglevel _U_,
    const char *message)
{
    fprintf(stderr, "%s\n", message);
}

static void identify(struct afp_server *server, const char *machine_type)
{
    snprintf(server->machine_type, sizeof(server->machine_type), "%s",
             machine_type);
    afp_server_identify(server);
}

/*!
 * @brief Check the identification of servers and the predicates it feeds
 *
 * @param[in] argc  argument count
 * @param[in] argv  --tap for TAP output
 * @returns EXIT_SUCCESS, or EXIT_FAILURE at the first failed check
 */
int main(int argc, char **argv)
{
    struct libafpclient test_client = {
        .log_for_client = stderr_log_for_client,
    };
    struct afp_versions afp22 = { "AFP2.2", 22 };
    struct afp_versions afp30 = { "AFP3.0", 30 };
    struct afp_versions afp33 = { "AFP3.3", 33 };
    struct afp_versions afp34 = { "AFP3.4", 34 };
    struct afp_server server;
    struct afpc_server_version version;
    test_tap_init(argc, argv);
    libafpclient_register(&test_client);
    memset(&server, 0, sizeof(server));
    CHECK(afp_identify_machine_type(NULL) == AFPC_SERVER_TYPE_UNKNOWN);
    CHECK(afp_identify_machine_type("") == AFPC_SERVER_TYPE_UNKNOWN);
    CHECK(afp_identify_machine_type("Netatalk") == AFPC_SERVER_TYPE_NETATALK);
    CHECK(afp_identify_machine_type("AirPort") == AFPC_SERVER_TYPE_AIRPORT);
    CHECK(afp_identify_machine_type("Mac mini") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("iMac") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("Xserve") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("PowerMac7,3")
          == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("PowerBook6,8")
          == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("RackMac3,1")
          == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("iBook") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("eMac") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("VMware7,1") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("TimeCapsule") == AFPC_SERVER_TYPE_TIMECAPSULE);
    CHECK(afp_identify_machine_type("Windows NT") == AFPC_SERVER_TYPE_WINDOWS);
    server.server_type = AFPC_SERVER_TYPE_TIMECAPSULE;
    CHECK(afp_server_is_apple(&server));
    server.server_type = AFPC_SERVER_TYPE_AIRPORT;
    CHECK(afp_server_is_apple(&server));
    server.server_type = AFPC_SERVER_TYPE_MACINTOSH;
    server.using_version = &afp34;
    CHECK(afp_server_is_apple(&server));
    CHECK(!afp_server_copies_files(&server));
    CHECK(!afp_server_reports_fork_length(&server));
    server.server_type = AFPC_SERVER_TYPE_NETATALK;
    CHECK(!afp_server_is_apple(&server));
    CHECK(afp_server_copies_files(&server));
    CHECK(afp_server_reports_fork_length(&server));
    server.using_version = &afp33;
    CHECK(!afp_server_copies_files(&server));
    server.using_version = &afp30;
    CHECK(afp_server_reports_fork_length(&server));
    server.using_version = &afp22;
    CHECK(!afp_server_reports_fork_length(&server));
    server.server_type = AFPC_SERVER_TYPE_UNKNOWN;
    CHECK(!afp_server_is_apple(&server));
    CHECK(server_version_parse("4.7.0dev", &version) == 0
          && version.major == 4 && version.minor == 7 && version.patch == 0);
    CHECK(server_version_parse("3.1.12", &version) == 0
          && version.major == 3 && version.minor == 1 && version.patch == 12);
    CHECK(server_version_parse("4.6", &version) == 0
          && version.major == 4 && version.minor == 6 && version.patch == 0);
    CHECK(server_version_parse("", &version) == -1 && version.major == 0);
    CHECK(server_version_parse("dev", &version) == -1);
    CHECK(server_version_parse("4.10.0", &version) == 0
          && server_version_at_least(&version, 4, 5, 0));
    CHECK(server_version_parse("4.4.9", &version) == 0
          && !server_version_at_least(&version, 4, 5, 0));
    CHECK(server_version_parse("4.5.0", &version) == 0
          && server_version_at_least(&version, 4, 5, 0)
          && !server_version_at_least(&version, 4, 5, 1));
    identify(&server, "Netatalk4.2.3");
    CHECK(server.server_version.major == 4 && server.server_version.minor == 2
          && server.server_version.patch == 3);
    CHECK(afp_server_drops_long_commands(&server));
    identify(&server, "Netatalk4.4.9");
    CHECK(afp_server_drops_long_commands(&server));
    identify(&server, "Netatalk4.5.0");
    CHECK(!afp_server_drops_long_commands(&server));
    identify(&server, "Netatalk4.7.0dev");
    CHECK(!afp_server_drops_long_commands(&server));
    CHECK(afp_server_sets_directory_owner(&server));
    CHECK(afp_server_keeps_unset_ids(&server));
    CHECK(!afp_server_keeps_directory_modes(&server));
    identify(&server, "Netatalk3.0.0");
    CHECK(afp_server_sets_directory_owner(&server));
    CHECK(!afp_server_keeps_directory_modes(&server));
    identify(&server, "Netatalk2.4.10");
    CHECK(!afp_server_sets_directory_owner(&server));
    CHECK(afp_server_keeps_unset_ids(&server));
    CHECK(afp_server_keeps_directory_modes(&server));
    identify(&server, "Mac mini");
    CHECK(server.server_version.major == 0 && server.server_version.minor == 0
          && server.server_version.patch == 0);
    CHECK(!afp_server_drops_long_commands(&server));
    CHECK(!afp_server_sets_directory_owner(&server));
    CHECK(!afp_server_keeps_unset_ids(&server));
    CHECK(!afp_server_keeps_directory_modes(&server));
    identify(&server, "Netatalk");
    CHECK(afp_server_drops_long_commands(&server));
    CHECK(!afp_server_sets_directory_owner(&server));
    CHECK(afp_server_keeps_unset_ids(&server));
    CHECK(afp_server_keeps_directory_modes(&server));
    libafpclient_register(NULL);
    return test_tap_finish();
}
