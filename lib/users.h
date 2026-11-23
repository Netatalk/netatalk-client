/*!
 * @file
 * @brief A volume's ID mapping: its detection and its translation of IDs
 *
 * The ID mapping's interface, outside the layers.
 * Lane: shared.
 * Northbound: the mid level and the listings.
 */

#ifndef __USERS_H_
#define __USERS_H_

int translate_uidgid_to_client(struct afp_volume * volume,
                               unsigned int *newuid, unsigned int *newgid);

int afp_detect_mapping(struct afp_volume * volume);


#endif
