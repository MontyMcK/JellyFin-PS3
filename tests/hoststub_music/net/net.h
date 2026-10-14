#pragma once
// Host stand-in for PSL1GHT's <net/net.h>: the music engine closes a socket on the server path, which no test opens.
static inline int netClose(int) { return 0; }
