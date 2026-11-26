/* Online save integrity in the game process (docs/ONLINE.md, Save integrity). */
#ifndef DCP_RUNTIME_INTEGRITY_H
#define DCP_RUNTIME_INTEGRITY_H

#ifdef __cplusplus
extern "C" {
#endif

/* The cheat-use sink (runtime_cheats_set_use_sink): marks the campaign save last mounted outside the Boss Battle copy,
 * once per save. Nothing is marked while isolated or before a campaign save was mounted. */
void runtime_integrity_cheat_used(void);

/* integrity_flags for the lobby hello: the mounted campaign save's, or every save of the title before one is mounted;
 * -1 when unknown. */
int runtime_integrity_hello_flags(void);

#ifdef __cplusplus
}
#endif

#endif
