#include "lib/utils.h"
#include "tap.h"

int main(int argc, char **argv)
{
    int version;
    test_tap_init(argc, argv);
    CHECK(afp_parse_version("3.1", &version) == 0);
    CHECK(version == 31);
    CHECK(afp_parse_version("31", &version) == 0);
    CHECK(version == 31);
    CHECK(afp_parse_version("4.0", &version) == 0);
    CHECK(version == 40);
    CHECK(afp_parse_version("31junk", &version) != 0);
    CHECK(afp_parse_version("3.10", &version) != 0);
    CHECK(afp_parse_version(NULL, &version) != 0);
    CHECK(afp_parse_version("3.1", NULL) != 0);
    return test_tap_finish();
}
