#include "afp.h"
#include "tap.h"

int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    CHECK(afp_identify_machine_type(NULL) == AFP_SERVER_TYPE_UNKNOWN);
    CHECK(afp_identify_machine_type("") == AFP_SERVER_TYPE_UNKNOWN);
    CHECK(afp_identify_machine_type("Netatalk") == AFP_SERVER_TYPE_NETATALK);
    CHECK(afp_identify_machine_type("AirPort") == AFP_SERVER_TYPE_AIRPORT);
    CHECK(afp_identify_machine_type("Mac mini") == AFP_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("iMac") == AFP_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("Xserve") == AFP_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("TimeCapsule") == AFP_SERVER_TYPE_TIMECAPSULE);
    CHECK(afp_identify_machine_type("Windows NT") == AFP_SERVER_TYPE_WINDOWS);
    return test_tap_finish();
}
