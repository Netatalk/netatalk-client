/*!
 * @file
 * @brief The sl namespace of afpc
 *
 * The interface of controller_sl.c.
 * Lane: control.
 * Northbound: afpc's dispatcher.
 */

#ifndef NETATALK_CLIENT_CONTROL_CONTROLLER_SL_H
#define NETATALK_CLIENT_CONTROL_CONTROLLER_SL_H

int afpc_sl_command(int argc, char **argv);

#endif
