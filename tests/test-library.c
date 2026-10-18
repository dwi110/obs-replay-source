#include "replay-library.h"

#include <obs-data.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                          \
			fprintf(stderr, "\n");                                 \
			return 1;                                              \
		}                                                              \
	} while (0)

static void remove_dir_recursive(const char *path)
{
	os_dir_t *dir = os_opendir(path);
	if (!dir)
		return;

	struct os_dirent *ent;
	while ((ent = os_readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		char subpath[1024];
		snprintf(subpath, sizeof(subpath), "%s/%s", path, ent->d_name);
		if (ent->directory)
			remove_dir_recursive(subpath);
		else
			os_unlink(subpath);
	}
	os_closedir(dir);
	os_rmdir(path);
}

int main(void)
{
	printf("[test-library] Starting persistent replay library tests...\n");

	char temp_dir[512];
#ifdef _WIN32
	snprintf(temp_dir, sizeof(temp_dir), "test_replay_library_tmp");
	os_mkdirs(temp_dir);
#else
	snprintf(temp_dir, sizeof(temp_dir), "/tmp/test-replay-library-XXXXXX");
	char *res = mkdtemp(temp_dir);
	CHECK(res != NULL, "mkdtemp failed");
#endif

	printf("[test-library] Using temp dir: %s\n", temp_dir);

	/* 1. Init on directory */
	replay_library_init(temp_dir);
	CHECK(strcmp(replay_library_dir(), temp_dir) == 0, "replay_library_dir mismatch");
	CHECK(strcmp(replay_library_active_game(), "game_default") == 0,
	      "initial active game must be game_default, got '%s'", replay_library_active_game());
	CHECK(replay_library_has_game("game_default"), "initial has_game game_default failed");

	/* 2. Create game & reject empty name */
	CHECK(!replay_library_create_game("", NULL, 0), "create_game should reject empty name");
	CHECK(!replay_library_create_game(NULL, NULL, 0), "create_game should reject NULL name");

	char game1_id[64] = {0};
	CHECK(replay_library_create_game("Championship Final", game1_id, sizeof(game1_id)),
	      "create_game Championship Final failed");
	CHECK(strlen(game1_id) > 0, "game1_id is empty");
	CHECK(strncmp(game1_id, "game-", 5) == 0, "game1_id must start with 'game-', got '%s'", game1_id);
	CHECK(replay_library_has_game(game1_id), "has_game failed for newly created game");
	printf("[test-library] Created game: %s ('Championship Final')\n", game1_id);

	/* Verify replay_library_games appends both default and created game */
	obs_data_array_t *games_arr = obs_data_array_create();
	replay_library_games(games_arr);
	CHECK(obs_data_array_count(games_arr) == 2, "expected 2 games in list, got %zu",
	      obs_data_array_count(games_arr));
	obs_data_array_release(games_arr);

	/* 3. Set active game */
	CHECK(replay_library_set_active_game(game1_id), "set_active_game failed");
	CHECK(strcmp(replay_library_active_game(), game1_id) == 0, "active_game mismatch after set");

	/* 4. Reject unknown game */
	CHECK(!replay_library_set_active_game("unknown_game_999"),
	      "set_active_game should return false for unknown id");
	CHECK(strcmp(replay_library_active_game(), game1_id) == 0,
	      "active_game must remain unchanged when set fails");

	/* 5. Add two entries */
	struct replay_library_entry e1;
	memset(&e1, 0, sizeof(e1));
	snprintf(e1.game_id, sizeof(e1.game_id), "%s", game1_id);
	snprintf(e1.angle, sizeof(e1.angle), "cam1");
	snprintf(e1.label, sizeof(e1.label), "Angle 1 Header Goal");
	snprintf(e1.tags, sizeof(e1.tags), "goal, header");
	snprintf(e1.file, sizeof(e1.file), "/tmp/angle1.mp4");
	snprintf(e1.state, sizeof(e1.state), "ready");
	e1.created_ms = 1000;
	e1.duration_ms = 5000;
	e1.range_start_ns = 10000000000LL;
	e1.range_end_ns = 15000000000LL;
	CHECK(replay_library_add_entry(&e1), "add_entry e1 failed");

	struct replay_library_entry e2;
	memset(&e2, 0, sizeof(e2));
	snprintf(e2.game_id, sizeof(e2.game_id), "%s", game1_id);
	snprintf(e2.angle, sizeof(e2.angle), "cam2");
	snprintf(e2.label, sizeof(e2.label), "Angle 2 Goalie Save");
	snprintf(e2.tags, sizeof(e2.tags), "save, defense");
	snprintf(e2.file, sizeof(e2.file), "/tmp/angle2.mp4");
	snprintf(e2.state, sizeof(e2.state), "ready");
	e2.created_ms = 2000;
	e2.duration_ms = 6000;
	e2.range_start_ns = 20000000000LL;
	e2.range_end_ns = 26000000000LL;
	CHECK(replay_library_add_entry(&e2), "add_entry e2 failed");

	/* 6. Find merge hits only the matching span */
	struct replay_library_entry found;
	CHECK(replay_library_find_merge("cam1", game1_id, 10000000000LL, 15000000000LL, &found),
	      "find_merge failed to hit matching span for e1");
	CHECK(strcmp(found.label, "Angle 1 Header Goal") == 0, "found label mismatch");
	CHECK(strncmp(found.library_id, "lib-", 4) == 0, "found library_id invalid: '%s'", found.library_id);
	char e1_id[40];
	snprintf(e1_id, sizeof(e1_id), "%s", found.library_id);
	printf("[test-library] e1 assigned library_id: %s\n", e1_id);

	CHECK(replay_library_find_merge("cam2", game1_id, 20000000000LL, 26000000000LL, &found),
	      "find_merge failed to hit matching span for e2");
	CHECK(strcmp(found.label, "Angle 2 Goalie Save") == 0, "found label mismatch");
	char e2_id[40];
	snprintf(e2_id, sizeof(e2_id), "%s", found.library_id);
	printf("[test-library] e2 assigned library_id: %s\n", e2_id);

	/* Non-matching spans */
	CHECK(!replay_library_find_merge("cam1", game1_id, 99999LL, 15000000000LL, &found),
	      "find_merge should not hit mismatched start");
	CHECK(!replay_library_find_merge("cam1", game1_id, 10000000000LL, 99999LL, &found),
	      "find_merge should not hit mismatched end");
	CHECK(!replay_library_find_merge("cam2", game1_id, 10000000000LL, 15000000000LL, &found),
	      "find_merge should not hit mismatched angle");
	CHECK(!replay_library_find_merge("cam1", "game_default", 10000000000LL, 15000000000LL, &found),
	      "find_merge should not hit mismatched game_id");

	/* 7. Set tags persists */
	CHECK(replay_library_set_tags(e1_id, "goal, highlight, supergoal"), "set_tags failed");
	struct replay_library_entry check_e;
	CHECK(replay_library_get(e1_id, &check_e), "get e1 failed");
	CHECK(strcmp(check_e.tags, "goal, highlight, supergoal") == 0, "tags mismatch after set_tags: '%s'",
	      check_e.tags);

	/* 8. Update entry to ready then failed */
	CHECK(replay_library_update_entry(e1_id, "saving", "/tmp/updated1.mp4", NULL),
	      "update_entry to saving failed");
	CHECK(replay_library_get(e1_id, &check_e), "get e1 failed");
	CHECK(strcmp(check_e.state, "saving") == 0, "state not saving");
	CHECK(strcmp(check_e.file, "/tmp/updated1.mp4") == 0, "file not updated");

	CHECK(replay_library_update_entry(e1_id, "ready", NULL, NULL), "update_entry to ready failed");
	CHECK(replay_library_get(e1_id, &check_e), "get e1 failed");
	CHECK(strcmp(check_e.state, "ready") == 0, "state not ready");

	CHECK(replay_library_update_entry(e1_id, "failed", NULL, "disk error during write"),
	      "update_entry to failed failed");
	CHECK(replay_library_get(e1_id, &check_e), "get e1 failed");
	CHECK(strcmp(check_e.state, "failed") == 0, "state not failed");
	CHECK(strcmp(check_e.error, "disk error during write") == 0, "error string mismatch");

	/* find_merge must exclude failed entries */
	CHECK(!replay_library_find_merge("cam1", game1_id, 10000000000LL, 15000000000LL, &found),
	      "find_merge must exclude failed entries");

	/* Restore e1 to ready for playlist testing */
	CHECK(replay_library_update_entry(e1_id, "ready", NULL, ""), "update e1 back to ready failed");
	CHECK(replay_library_get(e1_id, &check_e), "get e1 failed");
	CHECK(strcmp(check_e.state, "ready") == 0, "state not restored to ready");

	/* 9. Playlist filters by game AND tag and orders by created_ms */
	/* Add 3rd entry in game_default with created_ms 500 and tag 'goal' */
	struct replay_library_entry e3;
	memset(&e3, 0, sizeof(e3));
	snprintf(e3.game_id, sizeof(e3.game_id), "game_default");
	snprintf(e3.angle, sizeof(e3.angle), "cam1");
	snprintf(e3.label, sizeof(e3.label), "Default Game Scrimmage Goal");
	snprintf(e3.tags, sizeof(e3.tags), "goal, scrimmage");
	snprintf(e3.file, sizeof(e3.file), "/tmp/clip3.mp4");
	snprintf(e3.state, sizeof(e3.state), "ready");
	e3.created_ms = 500;
	e3.duration_ms = 3000;
	e3.range_start_ns = 5000000000LL;
	e3.range_end_ns = 8000000000LL;
	CHECK(replay_library_add_entry(&e3), "add_entry e3 failed");

	CHECK(replay_library_find_merge("cam1", "game_default", 5000000000LL, 8000000000LL, &found),
	      "find_merge failed for e3");
	char e3_id[40];
	snprintf(e3_id, sizeof(e3_id), "%s", found.library_id);
	printf("[test-library] e3 assigned library_id: %s\n", e3_id);

	/* Playlist test 1: All games (""), all tags ("") -> e3 (500), e1 (1000), e2 (2000) */
	char **ids = NULL;
	size_t n = replay_library_playlist("", "", &ids);
	CHECK(n == 3, "expected 3 items in playlist, got %zu", n);
	CHECK(strcmp(ids[0], e3_id) == 0, "playlist order[0] must be oldest (e3: 500ms), got '%s'", ids[0]);
	CHECK(strcmp(ids[1], e1_id) == 0, "playlist order[1] must be e1 (1000ms), got '%s'", ids[1]);
	CHECK(strcmp(ids[2], e2_id) == 0, "playlist order[2] must be e2 (2000ms), got '%s'", ids[2]);
	for (size_t i = 0; i < n; i++)
		bfree(ids[i]);
	bfree(ids);
	ids = NULL;

	/* Playlist test 2: Specific game (game1_id), all tags ("") -> e1 (1000), e2 (2000) */
	n = replay_library_playlist(game1_id, "", &ids);
	CHECK(n == 2, "expected 2 items for game1, got %zu", n);
	CHECK(strcmp(ids[0], e1_id) == 0, "expected e1 first");
	CHECK(strcmp(ids[1], e2_id) == 0, "expected e2 second");
	for (size_t i = 0; i < n; i++)
		bfree(ids[i]);
	bfree(ids);
	ids = NULL;

	/* Playlist test 3: Specific game (game1_id), tag 'save' -> e2 */
	n = replay_library_playlist(game1_id, "save", &ids);
	CHECK(n == 1, "expected 1 item for tag 'save', got %zu", n);
	CHECK(strcmp(ids[0], e2_id) == 0, "expected e2");
	for (size_t i = 0; i < n; i++)
		bfree(ids[i]);
	bfree(ids);
	ids = NULL;

	/* Playlist test 4: Tag 'goal' across all games ("") -> e3 (500), e1 (1000) */
	n = replay_library_playlist("", "goal", &ids);
	CHECK(n == 2, "expected 2 items with 'goal' tag across all games, got %zu", n);
	CHECK(strcmp(ids[0], e3_id) == 0, "expected e3 first");
	CHECK(strcmp(ids[1], e1_id) == 0, "expected e1 second");
	for (size_t i = 0; i < n; i++)
		bfree(ids[i]);
	bfree(ids);
	ids = NULL;

	/* Playlist test 5: Tag 'nonexistent' -> 0 */
	n = replay_library_playlist("", "nonexistent_tag", &ids);
	CHECK(n == 0, "expected 0 items for nonexistent tag, got %zu", n);
	CHECK(ids == NULL, "expected ids == NULL on count 0");

	/* 10. Shutdown + init reloads everything */
	printf("[test-library] Testing restart persistence (shutdown + re-init)...\n");
	replay_library_shutdown();

	replay_library_init(temp_dir);
	CHECK(strcmp(replay_library_active_game(), game1_id) == 0,
	      "active game was not reloaded after restart: expected '%s', got '%s'", game1_id,
	      replay_library_active_game());
	CHECK(replay_library_has_game(game1_id), "game1 not found after restart");
	CHECK(replay_library_has_game("game_default"), "game_default not found after restart");

	CHECK(replay_library_get(e1_id, &check_e), "get e1 after restart failed");
	CHECK(strcmp(check_e.tags, "goal, highlight, supergoal") == 0,
	      "e1 tags not preserved across restart: '%s'", check_e.tags);
	CHECK(strcmp(check_e.file, "/tmp/updated1.mp4") == 0,
	      "e1 file not preserved across restart: '%s'", check_e.file);
	CHECK(replay_library_get(e2_id, &check_e), "get e2 after restart failed");
	CHECK(replay_library_get(e3_id, &check_e), "get e3 after restart failed");

	/* Playlist works identically after restart */
	n = replay_library_playlist(game1_id, "", &ids);
	CHECK(n == 2, "playlist count after restart mismatch");
	for (size_t i = 0; i < n; i++)
		bfree(ids[i]);
	bfree(ids);
	ids = NULL;

	/* 11. Entries() field names match contract */
	obs_data_array_t *entries_arr = obs_data_array_create();
	replay_library_entries(entries_arr);
	size_t entries_count = obs_data_array_count(entries_arr);
	CHECK(entries_count == 3, "entries() count mismatch: expected 3, got %zu", entries_count);

	obs_data_t *item0 = obs_data_array_item(entries_arr, 0);
	CHECK(obs_data_has_user_value(item0, "libraryId"), "entries missing 'libraryId'");
	CHECK(obs_data_has_user_value(item0, "gameId"), "entries missing 'gameId'");
	CHECK(obs_data_has_user_value(item0, "angle"), "entries missing 'angle'");
	CHECK(obs_data_has_user_value(item0, "label"), "entries missing 'label'");
	CHECK(obs_data_has_user_value(item0, "tags"), "entries missing 'tags'");
	CHECK(obs_data_has_user_value(item0, "state"), "entries missing 'state'");
	CHECK(obs_data_has_user_value(item0, "createdAt"), "entries missing 'createdAt'");
	CHECK(obs_data_has_user_value(item0, "durationMs"), "entries missing 'durationMs'");
	CHECK(obs_data_has_user_value(item0, "error"), "entries missing 'error'");
	CHECK(obs_data_has_user_value(item0, "file"), "entries missing 'file'");
	CHECK(obs_data_has_user_value(item0, "rangeStartNs"), "entries missing 'rangeStartNs'");
	CHECK(obs_data_has_user_value(item0, "rangeEndNs"), "entries missing 'rangeEndNs'");
	obs_data_release(item0);
	obs_data_array_release(entries_arr);

	/* 12. Cleanup */
	replay_library_shutdown();
	remove_dir_recursive(temp_dir);

	printf("[test-library] ALL TESTS PASSED cleanly.\n");
	return 0;
}
