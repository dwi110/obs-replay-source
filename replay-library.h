#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent replay library for the MulticamReplay contract. One directory per OBS instance
 * (plugin_config/replay-source/library). The file format is internal; callers use this API only.
 * All functions are thread-safe. Strings the caller keeps must be copied; pointers returned are
 * valid until the next mutating call on the same thread only, so copy immediately. */

void replay_library_init(const char *dir);
void replay_library_shutdown(void);
const char *replay_library_dir(void);

/* Games. game_default always exists and is the initial active game. */
bool replay_library_create_game(const char *name, char *id_out, size_t id_size);
bool replay_library_has_game(const char *game_id);
bool replay_library_set_active_game(const char *game_id);
const char *replay_library_active_game(void);
void replay_library_games(obs_data_array_t *out); /* appends {gameId, name} */

/* Entries. range_start_ns/range_end_ns identify the saved span in source-clock nanoseconds and are
 * the identity used for tag merging (same angle, same game, same span). */
struct replay_library_entry {
	char library_id[40];
	char game_id[64];
	char angle[64];
	char label[128];
	char tags[256];
	char file[1024];
	char state[16]; /* saving, ready, failed */
	char error[256];
	int64_t created_ms;
	int64_t duration_ms;
	int64_t range_start_ns;
	int64_t range_end_ns;
};

bool replay_library_add_entry(struct replay_library_entry *entry);
bool replay_library_update_entry(const char *library_id, const char *state, const char *file, const char *error);
bool replay_library_set_tags(const char *library_id, const char *tags);
bool replay_library_get(const char *library_id, struct replay_library_entry *out);
/* Finds an entry with the same angle, game and span to merge tags into. */
bool replay_library_find_merge(const char *angle, const char *game_id, int64_t range_start_ns, int64_t range_end_ns,
			       struct replay_library_entry *out);
void replay_library_entries(obs_data_array_t *out); /* appends one object per entry, contract field names */

/* Playlist selection: entries in state "ready" matching the game ("" = all games) AND at least one tag
 * ("" = all tags), oldest first. Returns the count and a bfree'd array of bfree'd id strings. */
size_t replay_library_playlist(const char *game_id, const char *tags_csv, char ***ids_out);

#ifdef __cplusplus
}
#endif
