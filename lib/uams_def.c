/*
 *  uams_def.c
 *
 *  Copyright (C) 2007 Alex deVries <alexthepuffin@gmail.com>
 *  Copyright (C) 2007 Derrik Pates <dpates@dsdk12.net>
 *  Copyright (C) 2025-2026 Daniel Markstedt <daniel@mindani.net>
 *
 */

#include <string.h>
#include <stdlib.h>

static char *afp_strings[] = { "No User Authent",
                               "Cleartxt Passwrd",
                               "Randnum Exchange",
                               "2-Way Randnum Exchange",
                               "DHCAST128",
                               "Client Krb v2",
                               "DHX2",
                               "Recon1",
                               "SRP",
                               NULL
                             };

/* UAM shorthand aliases following Netatalk naming conventions */
struct uam_alias {
    const char *shorthand;
    const char *full_name;
};

static struct uam_alias uam_aliases[] = {
    { "guest",       "No User Authent" },
    { "clrtxt",      "Cleartxt Passwrd" },
    { "randnum",     "Randnum Exchange" },
    { "randnum2", "2-Way Randnum Exchange" },
    { "dhx",         "DHCAST128" },
    { "dhx2",        "DHX2" },
    { "srp",         "SRP" },
    { NULL, NULL }
};

/*
 * Resolve UAM shorthand to full internal name.
 * Returns the full name if a shorthand is matched, or the input name unchanged.
 */
const char *resolve_uam_shorthand(const char *name)
{
    for (int i = 0; uam_aliases[i].shorthand != NULL; i++) {
        if (strcasecmp(name, uam_aliases[i].shorthand) == 0) {
            return uam_aliases[i].full_name;
        }
    }

    return name;
}

int uam_string_to_bitmap(const char *name)
{
    const char *resolved_name = resolve_uam_shorthand(name);

    for (int i = 0; afp_strings[i] != NULL; i++)
        if (strcasecmp(resolved_name, afp_strings[i]) == 0) {
            return 1 << i;
        }

    return 0;
}

char *uam_bitmap_to_string(unsigned int bitmap)
{
    int max_index;

    for (max_index = 0; afp_strings[max_index] != NULL; max_index++);

    max_index--;

    for (int i = max_index; i >= 0; i--)
        if (bitmap & (1 << i)) {
            return afp_strings[i];
        }

    return NULL;
}
