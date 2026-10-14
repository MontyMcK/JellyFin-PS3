#pragma once
// The PS3 side of Wake-on-LAN (wol_net.cpp): the saved MAC address and the send.
#include <stdbool.h>
#include <stdint.h>

// jellyfin_wol.txt holds the server PC's MAC address.
bool wol_saved(uint8_t mac[6]);
bool wol_save(const uint8_t mac[6]);

// Broadcast the magic packet (UDP ports 9 and 7, twice each).  True when a
// packet left the console; that says nothing about whether the PC woke.
bool wol_send(const uint8_t mac[6]);
// The same for the saved address; false (and silent) when none is saved.
bool wol_send_saved(void);
