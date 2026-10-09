#include "lib/afp_internal.h"
#include "tap.h"

static int version_number(unsigned char *versions, unsigned char requested)
{
    const struct afp_versions *version = pick_version(versions, requested);
    return version ? version->av_number : 0;
}

int main(int argc, char **argv)
{
    unsigned char modern_versions[SERVER_MAX_VERSIONS] = {
        22, 30, 31, 32, 33, 34, 0
    };
    unsigned char all_versions[SERVER_MAX_VERSIONS] = {
        11, 20, 21, 22, 30, 31, 32, 33, 34, 0
    };
    unsigned char full_versions[SERVER_MAX_VERSIONS] = {
        11, 20, 21, 22, 30, 31, 32, 33, 34, 34
    };
    test_tap_init(argc, argv);
    CHECK(version_number(modern_versions, 21) == 0);
    CHECK(version_number(modern_versions, 22) == 22);
    CHECK(version_number(modern_versions, 29) == 22);
    CHECK(version_number(modern_versions, 34) == 34);
    CHECK(version_number(modern_versions, 0) == 34);
    CHECK(version_number(all_versions, 21) == 21);
    CHECK(version_number(all_versions, 35) == 34);
    CHECK(version_number(full_versions, 0) == 34);
    return test_tap_finish();
}
