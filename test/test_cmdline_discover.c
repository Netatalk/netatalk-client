#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmdline/discover.h"
#include "tap.h"

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

static int capture_picker(const char *input, char *url, size_t url_size,
                          char *output, size_t output_size)
{
    FILE *capture_in = NULL;
    FILE *capture_out = NULL;
    int saved_stdin = -1;
    int saved_stdout = -1;
    int ret = -1;
    size_t size;
    capture_in = tmpfile();
    capture_out = tmpfile();

    if (!capture_in || !capture_out || fputs(input, capture_in) == EOF) {
        goto done;
    }

    rewind(capture_in);
    fflush(stdout);
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);

    if (saved_stdin < 0 || saved_stdout < 0
            || dup2(fileno(capture_in), STDIN_FILENO) < 0
            || dup2(fileno(capture_out), STDOUT_FILENO) < 0) {
        goto done;
    }

    clearerr(stdin);
    ret = cmdline_discover_url(url, url_size);
    fflush(stdout);
done:

    if (saved_stdin >= 0) {
        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);
        clearerr(stdin);
    }

    if (saved_stdout >= 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }

    if (capture_out) {
        rewind(capture_out);
        size = fread(output, 1, output_size - 1, capture_out);
        output[size] = '\0';
    }

    if (capture_in) {
        fclose(capture_in);
    }

    if (capture_out) {
        fclose(capture_out);
    }

    return ret;
}

int main(int argc, char **argv)
{
    char output[4096];
    char url[512];
    test_tap_init(argc, argv);
    memset(url, 0, sizeof(url));
    CHECK(capture_picker("1\n", url, sizeof(url), output,
                         sizeof(output)) == 0);
    CHECK(strcmp(url, "afp://192.0.2.10:548") == 0);
    CHECK(strstr(output, "AFP servers") != NULL);
    CHECK(strstr(output, "1  Office \"Mac\"") != NULL);
    CHECK(count_text(output, "Office \"Mac\"") == 1);
    CHECK(strstr(output, "q  Quit") != NULL);
    CHECK(strstr(output, "Searching for AFP services") == NULL);
    CHECK(strstr(output, "manual") == NULL);
    memset(url, 0, sizeof(url));
    CHECK(capture_picker("q\n", url, sizeof(url), output,
                         sizeof(output)) == 1);
    CHECK(url[0] == '\0');
    return test_tap_finish();
}
