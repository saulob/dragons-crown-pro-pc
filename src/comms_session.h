/* Comms Wheel as the rest of the port sees it: whether the wheel may open and what sending does. */
#ifndef DCP_COMMS_SESSION_H
#define DCP_COMMS_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the wheel may open: Online Play on, an online room of at least two players, the game's balloon bridge installed
 * and this player's unit running. Cheap; any thread. */
int comms_session_available(void);

/* A message chosen on the wheel: 1 when it was accepted (valid id, not rate limited), then shown on this client's own
 * balloon, sent as an id to the other players of the room and counted. 0 when it was dropped. Any thread. */
int comms_session_send(int id);

/* Hooks the session's per-frame work (telemetry reports, receiving) into the online pump; once, at startup. */
void comms_session_register(void);

#ifdef __cplusplus
}
#endif

#endif
