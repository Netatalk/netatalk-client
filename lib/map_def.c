/*
 *  map_def.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <string.h>
#include "afp.h"
#include "map_def.h"

static char *afp_map_strings[] = {
    "Unknown",
    "Common user directory",
    "Login ids",
    "Name mapped",
    "",
};

char *get_mapping_name(struct afp_volume * volume)
{
    return afp_map_strings[volume->mapping];
}

unsigned int map_string_to_num(const char * name)
{
    int i;

    /* Check short aliases first */
    if (strcasecmp(name, "common") == 0) {
        return AFP_MAPPING_COMMON;
    } else if (strcasecmp(name, "loginids") == 0) {
        return AFP_MAPPING_LOGINIDS;
    } else if (strcasecmp(name, "name") == 0) {
        return AFP_MAPPING_NAME;
    }

    /* Check full names */
    for (i = 0; strlen(afp_map_strings[i]) > 0; i++) {
        if (strcasecmp(name, afp_map_strings[i]) == 0) {
            return i;
        }
    }

    return 0;
}
