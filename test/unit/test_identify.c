#include "lib/afp_internal.h"
#include "tap.h"

int main(int argc, char **argv)
{
    test_tap_init(argc, argv);
    CHECK(afp_identify_machine_type(NULL) == AFPC_SERVER_TYPE_UNKNOWN);
    CHECK(afp_identify_machine_type("") == AFPC_SERVER_TYPE_UNKNOWN);
    CHECK(afp_identify_machine_type("Netatalk") == AFPC_SERVER_TYPE_NETATALK);
    CHECK(afp_identify_machine_type("AirPort") == AFPC_SERVER_TYPE_AIRPORT);
    CHECK(afp_identify_machine_type("Mac mini") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("iMac") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("Xserve") == AFPC_SERVER_TYPE_MACINTOSH);
    CHECK(afp_identify_machine_type("TimeCapsule") == AFPC_SERVER_TYPE_TIMECAPSULE);
    CHECK(afp_identify_machine_type("Windows NT") == AFPC_SERVER_TYPE_WINDOWS);
    return test_tap_finish();
}
