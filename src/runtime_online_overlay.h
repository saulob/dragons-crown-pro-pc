/* What the in-game overlay shows about online play (runtime_online.cpp), without the network headers. */
#ifndef DCP_RUNTIME_ONLINE_OVERLAY_H
#define DCP_RUNTIME_ONLINE_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when Online Play is on (dcp.ini or DCP_ONLINE), even if this build or identity then keeps the game offline. */
int runtime_online_play_enabled(void);
/* The worst P2P round trip in ms among the game's connected peers; -1 when offline or no peer has measured one. */
int runtime_online_peer_ping_ms(void);

#ifdef __cplusplus
}
#endif

#endif
