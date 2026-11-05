/* Discord Rich Presence (docs/DISCORD.md). Reads discord_presence and discord_client_id from dcp.ini and starts a
 * background thread; does nothing when off or without a client id. */
#ifndef DCP_DISCORD_PRESENCE_H
#define DCP_DISCORD_PRESENCE_H

#ifdef __cplusplus
extern "C" {
#endif

void discord_presence_start(const char *settings_file);

#ifdef __cplusplus
}
#endif

#endif
