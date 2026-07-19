#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "discovery/client/discover.h"
#include "tap.h"

static int capture_command(int command_argc, char **command_argv,
                           char *output, size_t output_size)
{
    FILE *capture;
    int saved_stdout;
    int ret;
    size_t size;
    capture = tmpfile();

    if (!capture) {
        return -1;
    }

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);

    if (saved_stdout < 0 || dup2(fileno(capture), STDOUT_FILENO) < 0) {
        fclose(capture);
        return -1;
    }

    ret = afpc_discover_command(command_argc, command_argv);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    rewind(capture);
    size = fread(output, 1, output_size - 1, capture);
    output[size] = '\0';
    fclose(capture);
    return ret;
}

static int count_text(const char *text, const char *needle)
{
    int count = 0;
    size_t needle_len = strlen(needle);

    while ((text = strstr(text, needle))) {
        count++;
        text += needle_len;
    }

    return count;
}

int main(int argc, char **argv)
{
    char output[4096];
    char host[256];
    uint16_t port = 0;
    char *json_argv[] = {
        "discover", "--json", "--timeout", "5", NULL,
    };
    char *verbose_argv[] = {
        "discover", "--verbose", "--timeout", "5", NULL,
    };
    char *human_argv[] = {
        "discover", "--timeout", "5", NULL,
    };
    char *afp_client_help_argv[] = {
        "discover", "--help", NULL,
    };
    test_tap_init(argc, argv);
    CHECK(capture_command(4, json_argv, output, sizeof(output)) == 0);
    CHECK(strstr(output, "\"backend\":\"fake-command\"") != NULL);
    CHECK(strstr(output, "\"name\":\"Office \\\"Mac\\\"\"") != NULL);
    CHECK(strstr(output, "\"device_type\":\"Macmini\"") != NULL);
    CHECK(count_text(output, "\"device_type\":\"Macmini\"") == 2);
    CHECK(strstr(output, "_device-info._tcp") == NULL);
    CHECK(strstr(output, "Apple AFP") == NULL);
    CHECK(strstr(output, "\"target\":\"office.local.\"") != NULL);
    CHECK(strstr(output, "\"port\":548") != NULL);
    CHECK(strstr(output, "\"addresses\":[\"192.0.2.10\"]") != NULL);
    CHECK(strstr(output, "\"txt_hex\":\"036b3d76\"") != NULL);
    CHECK(capture_command(4, verbose_argv, output, sizeof(output)) == 0);
    CHECK(strstr(output, "Name:       Office \"Mac\"") != NULL);
    CHECK(strstr(output, "Type:       _afpovertcp._tcp") != NULL);
    CHECK(strstr(output, "Device:     Macmini") != NULL);
    CHECK(count_text(output, "Type:       _device-info._tcp") == 2);
    CHECK(strstr(output, "Name:       Apple AFP") != NULL);
    CHECK(strstr(output, "Device:     MacBookPro18,3") != NULL);
    CHECK(strstr(output, "Target:     office.local.") != NULL);
    CHECK(strstr(output, "Addresses:  192.0.2.10") != NULL);
    CHECK(strstr(output, "TXT (hex):  036b3d76") != NULL);
    CHECK(capture_command(3, human_argv, output, sizeof(output)) == 0);
    CHECK(strstr(output, "INTERFACE") == NULL);
    CHECK(strstr(output,
                 "NAME                           TARGET"
                 "                                PORT   MODEL") != NULL);
    CHECK(strstr(output, "Macmini") != NULL);
    CHECK(strstr(output, "Apple AFP") == NULL);
    CHECK(count_text(output, "Office \"Mac\"") == 1);
    CHECK(strstr(output, "office.local ") != NULL);
    CHECK(strstr(output, "office.local.") == NULL);
    CHECK(capture_command(2, afp_client_help_argv, output,
                          sizeof(output)) == 0);
    CHECK(strstr(output, "Usage: afp_client discover ") != NULL);
    CHECK(afpc_discover_resolve_service("Office \"Mac\"", host,
                                        sizeof(host), &port, 5) == 0);
    CHECK(strcmp(host, "192.0.2.10") == 0);
    CHECK(port == 548);
    CHECK(afpc_discover_resolve_service("Missing", host, sizeof(host),
                                        &port, 5) == -ENOENT);
    return test_tap_finish();
}
