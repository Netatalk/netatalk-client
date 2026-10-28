/*
    test_path_conversion_allocs.c: allocation counts of the path conversion

    Copyright (C) 2026 Andy Lemin (andylemin)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/afp_protocol.h"
#include "lib/codepage.h"
#include "tap.h"

/* The conversion sources compile into this test, so this is their
 * allocator: blocks are carved from the arena, counted and never
 * returned; a 16-byte header holds each size. */
static _Alignas(16) unsigned char arena[1U << 20];
static size_t arena_used;
static unsigned int allocations;

/*!
 * @brief Carve a block from the arena
 *
 * @param[in] size  bytes wanted
 * @returns the block, or NULL when the arena is full
 */
static void *carve(size_t size)
{
    size_t rounded = (size + 15U) & ~(size_t)15U;
    unsigned char *block;

    if (arena_used + 16U + rounded > sizeof(arena)) {
        return NULL;
    }

    block = arena + arena_used;
    memcpy(block, &size, sizeof(size));
    arena_used += 16U + rounded;
    return block + 16U;
}

/*!
 * @brief Count an allocation and carve it
 *
 * @param[in] size  bytes wanted
 * @returns the block, or NULL when the arena is full
 */
void *malloc(size_t size)
{
    allocations++;
    return carve(size);
}

/*!
 * @brief Keep the block; the arena takes nothing back
 *
 * @param[in] ptr  the block
 */
void free(void *ptr)
{
    (void) ptr;
}

/*!
 * @brief Count an allocation and carve it zeroed
 *
 * @param[in] count  elements
 * @param[in] size   bytes per element
 * @returns the block, or NULL when the product overflows or the arena is
 *          full
 */
void *calloc(size_t count, size_t size)
{
    unsigned char *block;

    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }

    allocations++;
    block = carve(count * size);

    if (block) {
        for (size_t i = 0; i < count * size; i++) {
            block[i] = 0;
        }
    }

    return block;
}

/*!
 * @brief Count an allocation and carve a copy
 *
 * @param[in] ptr   the block to copy, or NULL
 * @param[in] size  bytes wanted
 * @returns the block, or NULL when the arena is full
 */
void *realloc(void *ptr, size_t size)
{
    unsigned char *block;
    size_t old_size;
    allocations++;
    block = carve(size);

    if (!block || !ptr) {
        return block;
    }

    memcpy(&old_size, (unsigned char *)ptr - 16U, sizeof(old_size));
    memcpy(block, ptr, old_size < size ? old_size : size);
    return block;
}

/*!
 * @brief Run the allocation-count checks as TAP
 *
 * @param[in] argc  argument count
 * @param[in] argv  arguments for the TAP harness
 * @returns the harness's exit status
 */
int main(int argc, char **argv)
{
    char out[AFPC_MAX_NAME_BYTES * 4U];
    char ascii_afp[] = "/dir/file.txt";
    char decomposed_afp[] = "/e\xcc\x81";
    char long_name[1U + 300U * 2U + 1U];
    unsigned int before;
    test_tap_init(argc, argv);
    /* The first decomposition builds the index, and some C libraries'
     * qsort() allocates; the counts start after it. */
    CHECK(convert_path_to_afp(kFPUTF8Name, out, "/caf\xc3\xa9",
                              sizeof(out)) == 0);
    before = allocations;
    /* An ASCII path allocates nothing in either direction. */
    CHECK(convert_path_to_afp(kFPUTF8Name, out, "/dir/file.txt",
                              sizeof(out)) == 0);
    CHECK(convert_path_to_afp(kFPLongName, out, "/dir/file.txt",
                              sizeof(out)) == 0);
    CHECK(convert_path_to_unix(kFPUTF8Name, out, ascii_afp,
                               sizeof(out)) == 0);
    CHECK(allocations == before);
    /* A short name beyond ASCII converts on the stack. */
    CHECK(convert_path_to_afp(kFPUTF8Name, out, "/caf\xc3\xa9",
                              sizeof(out)) == 0);
    CHECK(convert_path_to_afp(kFPLongName, out, "/caf\xc3\xa9",
                              sizeof(out)) == 0);
    CHECK(convert_path_to_unix(kFPUTF8Name, out, decomposed_afp,
                               sizeof(out)) == 0);
    CHECK(allocations == before);
    /* The decomposition of 300 characters outgrows its stack buffer: the
     * heap serves it, which shows that the allocator counts. */
    long_name[0] = '/';

    for (size_t i = 0; i < 300U; i++) {
        long_name[1U + i * 2U] = (char)0xc3;
        long_name[2U + i * 2U] = (char)0xa9;
    }

    long_name[sizeof(long_name) - 1U] = '\0';
    CHECK(convert_path_to_afp(kFPUTF8Name, out, long_name,
                              sizeof(out)) == 0);
    CHECK(allocations > before);
    return test_tap_finish();
}
