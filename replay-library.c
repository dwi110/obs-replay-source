#include "replay-library.h"

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-data.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct game_entry {
	char game_id[64];
	char *name;
};

static pthread_mutex_t lib_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool lib_initialized = false;
static char *lib_dir = NULL;
static char lib_active_game[64] = "game_default";
static uint64_t lib_next_id = 1;

static DARRAY(struct game_entry) lib_games;
static DARRAY(struct replay_library_entry) lib_entries;

static bool has_game_locked(const char *game_id)
{
	if (!game_id || !*game_id)
		return false;
	for (size_t i = 0; i < lib_games.num; i++) {
		if (strcmp(lib_games.array[i].game_id, game_id) == 0)
			return true;
	}
	return false;
}

static bool save_games_file_locked(void)
{
	if (!lib_dir)
		return false;

	struct dstr json_text = {0};
	dstr_cat(&json_text, "[\n");
	for (size_t i = 0; i < lib_games.num; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "gameId", lib_games.array[i].game_id);
		obs_data_set_string(item, "name", lib_games.array[i].name ? lib_games.array[i].name : "");
		const char *item_json = obs_data_get_json(item);
		dstr_cat(&json_text, "  ");
		dstr_cat(&json_text, item_json);
		if (i + 1 < lib_games.num)
			dstr_cat(&json_text, ",\n");
		else
			dstr_cat(&json_text, "\n");
		obs_data_release(item);
	}
	dstr_cat(&json_text, "]\n");

	struct dstr path = {0};
	dstr_printf(&path, "%s/games.json", lib_dir);
	bool ok = os_quick_write_utf8_file_safe(path.array, json_text.array, json_text.len, false, "tmp", NULL);
	dstr_free(&path);
	dstr_free(&json_text);
	return ok;
}

static void load_games_file_locked(void)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/games.json", lib_dir);
	char *content = os_quick_read_utf8_file(path.array);
	dstr_free(&path);

	bool has_default = false;
	if (content) {
		const char *trimmed = content;
		while (*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r' || *trimmed == '\n')
			trimmed++;

		struct dstr wrap = {0};
		if (*trimmed == '[')
			dstr_printf(&wrap, "{\"games\":%s}", trimmed);
		else if (*trimmed == '{')
			dstr_copy(&wrap, trimmed);
		else
			dstr_init(&wrap);

		if (wrap.array && *wrap.array) {
			obs_data_t *data = obs_data_create_from_json(wrap.array);
			if (data) {
				obs_data_array_t *arr = obs_data_get_array(data, "games");
				if (arr) {
					size_t count = obs_data_array_count(arr);
					for (size_t i = 0; i < count; i++) {
						obs_data_t *item = obs_data_array_item(arr, i);
						const char *gid = obs_data_get_string(item, "gameId");
						if (!gid || !*gid)
							gid = obs_data_get_string(item, "game_id");
						const char *name = obs_data_get_string(item, "name");
						if (gid && *gid) {
							struct game_entry ge;
							snprintf(ge.game_id, sizeof(ge.game_id), "%s", gid);
							ge.name = bstrdup(name ? name : "");
							da_push_back(lib_games, &ge);
							if (strcmp(gid, "game_default") == 0)
								has_default = true;
						}
						obs_data_release(item);
					}
					obs_data_array_release(arr);
				}
				obs_data_release(data);
			}
		}
		dstr_free(&wrap);
		bfree(content);
	}

	if (!has_default) {
		struct game_entry ge;
		snprintf(ge.game_id, sizeof(ge.game_id), "game_default");
		ge.name = bstrdup("Default");
		da_insert(lib_games, 0, &ge);
		save_games_file_locked();
	}
}

static bool save_active_game_file_locked(void)
{
	if (!lib_dir)
		return false;

	struct dstr path = {0};
	dstr_printf(&path, "%s/active-game", lib_dir);
	bool ok = os_quick_write_utf8_file_safe(path.array, lib_active_game, strlen(lib_active_game), false, "tmp", NULL);
	dstr_free(&path);
	return ok;
}

static void load_active_game_file_locked(void)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/active-game", lib_dir);
	char *content = os_quick_read_utf8_file(path.array);
	dstr_free(&path);

	if (content) {
		char *p = content;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		char *end = p + strlen(p);
		while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
			end--;
			*end = '\0';
		}
		if (*p && has_game_locked(p)) {
			snprintf(lib_active_game, sizeof(lib_active_game), "%s", p);
		} else {
			snprintf(lib_active_game, sizeof(lib_active_game), "game_default");
			save_active_game_file_locked();
		}
		bfree(content);
	} else {
		snprintf(lib_active_game, sizeof(lib_active_game), "game_default");
		save_active_game_file_locked();
	}
}

static bool save_next_id_file_locked(void)
{
	if (!lib_dir)
		return false;

	struct dstr path = {0};
	dstr_printf(&path, "%s/next-id", lib_dir);
	char buf[64];
	snprintf(buf, sizeof(buf), "%" PRIu64 "\n", lib_next_id);
	bool ok = os_quick_write_utf8_file_safe(path.array, buf, strlen(buf), false, "tmp", NULL);
	dstr_free(&path);
	return ok;
}

static void load_next_id_file_locked(void)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/next-id", lib_dir);
	char *content = os_quick_read_utf8_file(path.array);
	dstr_free(&path);

	lib_next_id = 1;
	if (content) {
		uint64_t val = 0;
		if (sscanf(content, "%" SCNu64, &val) == 1 && val > 0)
			lib_next_id = val;
		bfree(content);
	} else {
		save_next_id_file_locked();
	}
}

static bool save_entry_file_locked(const struct replay_library_entry *e)
{
	if (!lib_dir || !e || !e->library_id[0])
		return false;

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "libraryId", e->library_id);
	obs_data_set_string(data, "gameId", e->game_id);
	obs_data_set_string(data, "angle", e->angle);
	obs_data_set_string(data, "label", e->label);
	obs_data_set_string(data, "tags", e->tags);
	obs_data_set_string(data, "state", e->state);
	obs_data_set_string(data, "error", e->error);
	obs_data_set_string(data, "file", e->file);
	obs_data_set_int(data, "createdAt", e->created_ms);
	obs_data_set_int(data, "durationMs", e->duration_ms);
	obs_data_set_int(data, "rangeStartNs", e->range_start_ns);
	obs_data_set_int(data, "rangeEndNs", e->range_end_ns);

	struct dstr path = {0};
	dstr_printf(&path, "%s/entries/%s.json", lib_dir, e->library_id);
	bool ok = obs_data_save_json_safe(data, path.array, "tmp", NULL);
	dstr_free(&path);
	obs_data_release(data);
	return ok;
}

static void load_entries_dir_locked(void)
{
	struct dstr entries_path = {0};
	dstr_printf(&entries_path, "%s/entries", lib_dir);

	os_dir_t *dir = os_opendir(entries_path.array);
	if (!dir) {
		dstr_free(&entries_path);
		return;
	}

	struct os_dirent *ent;
	while ((ent = os_readdir(dir)) != NULL) {
		if (ent->directory)
			continue;
		const char *ext = os_get_path_extension(ent->d_name);
		if (!ext || strcmp(ext, ".json") != 0)
			continue;

		struct dstr file_path = {0};
		dstr_printf(&file_path, "%s/%s", entries_path.array, ent->d_name);
		char *content = os_quick_read_utf8_file(file_path.array);
		dstr_free(&file_path);

		if (!content)
			continue;

		obs_data_t *data = obs_data_create_from_json(content);
		bfree(content);
		if (!data)
			continue;

		struct replay_library_entry e;
		memset(&e, 0, sizeof(e));

		const char *s;
		s = obs_data_get_string(data, "libraryId");
		if (!s || !*s)
			s = obs_data_get_string(data, "library_id");
		if (s)
			snprintf(e.library_id, sizeof(e.library_id), "%s", s);

		s = obs_data_get_string(data, "gameId");
		if (!s || !*s)
			s = obs_data_get_string(data, "game_id");
		if (s)
			snprintf(e.game_id, sizeof(e.game_id), "%s", s);

		s = obs_data_get_string(data, "angle");
		if (s)
			snprintf(e.angle, sizeof(e.angle), "%s", s);

		s = obs_data_get_string(data, "label");
		if (s)
			snprintf(e.label, sizeof(e.label), "%s", s);

		s = obs_data_get_string(data, "tags");
		if (s)
			snprintf(e.tags, sizeof(e.tags), "%s", s);

		s = obs_data_get_string(data, "state");
		if (s)
			snprintf(e.state, sizeof(e.state), "%s", s);

		s = obs_data_get_string(data, "error");
		if (s)
			snprintf(e.error, sizeof(e.error), "%s", s);

		s = obs_data_get_string(data, "file");
		if (s)
			snprintf(e.file, sizeof(e.file), "%s", s);

		if (obs_data_has_user_value(data, "createdAt"))
			e.created_ms = obs_data_get_int(data, "createdAt");
		else
			e.created_ms = obs_data_get_int(data, "created_ms");

		if (obs_data_has_user_value(data, "durationMs"))
			e.duration_ms = obs_data_get_int(data, "durationMs");
		else
			e.duration_ms = obs_data_get_int(data, "duration_ms");

		if (obs_data_has_user_value(data, "rangeStartNs"))
			e.range_start_ns = obs_data_get_int(data, "rangeStartNs");
		else
			e.range_start_ns = obs_data_get_int(data, "range_start_ns");

		if (obs_data_has_user_value(data, "rangeEndNs"))
			e.range_end_ns = obs_data_get_int(data, "rangeEndNs");
		else
			e.range_end_ns = obs_data_get_int(data, "range_end_ns");

		obs_data_release(data);

		if (e.library_id[0] != '\0') {
			da_push_back(lib_entries, &e);

			uint64_t num = 0;
			if (sscanf(e.library_id, "lib-%" SCNu64, &num) == 1) {
				if (num >= lib_next_id) {
					lib_next_id = num + 1;
					save_next_id_file_locked();
				}
			}
		}
	}

	os_closedir(dir);
	dstr_free(&entries_path);
}

static void shutdown_locked(void)
{
	for (size_t i = 0; i < lib_games.num; i++)
		bfree(lib_games.array[i].name);
	da_free(lib_games);
	da_free(lib_entries);
	bfree(lib_dir);
	lib_dir = NULL;
	snprintf(lib_active_game, sizeof(lib_active_game), "game_default");
	lib_next_id = 1;
	lib_initialized = false;
}

void replay_library_init(const char *dir)
{
	pthread_mutex_lock(&lib_mutex);

	if (lib_initialized)
		shutdown_locked();

	if (!dir || !*dir) {
		pthread_mutex_unlock(&lib_mutex);
		return;
	}

	os_mkdirs(dir);

	struct dstr entries_dir = {0};
	dstr_printf(&entries_dir, "%s/entries", dir);
	os_mkdirs(entries_dir.array);
	dstr_free(&entries_dir);

	lib_dir = bstrdup(dir);
	da_init(lib_games);
	da_init(lib_entries);

	load_next_id_file_locked();
	load_games_file_locked();
	load_active_game_file_locked();
	load_entries_dir_locked();

	lib_initialized = true;

	pthread_mutex_unlock(&lib_mutex);
}

void replay_library_shutdown(void)
{
	pthread_mutex_lock(&lib_mutex);
	shutdown_locked();
	pthread_mutex_unlock(&lib_mutex);
}

const char *replay_library_dir(void)
{
	pthread_mutex_lock(&lib_mutex);
	const char *ret = lib_dir ? lib_dir : "";
	pthread_mutex_unlock(&lib_mutex);
	return ret;
}

bool replay_library_create_game(const char *name, char *id_out, size_t id_size)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !name || !*name) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	char id_buf[64];
	snprintf(id_buf, sizeof(id_buf), "game-%" PRIu64, lib_next_id++);
	save_next_id_file_locked();

	struct game_entry ge;
	snprintf(ge.game_id, sizeof(ge.game_id), "%s", id_buf);
	ge.name = bstrdup(name);
	da_push_back(lib_games, &ge);

	save_games_file_locked();

	if (id_out && id_size > 0)
		snprintf(id_out, id_size, "%s", id_buf);

	pthread_mutex_unlock(&lib_mutex);
	return true;
}

bool replay_library_has_game(const char *game_id)
{
	pthread_mutex_lock(&lib_mutex);
	bool ret = lib_initialized ? has_game_locked(game_id) : false;
	pthread_mutex_unlock(&lib_mutex);
	return ret;
}

bool replay_library_set_active_game(const char *game_id)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !has_game_locked(game_id)) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	snprintf(lib_active_game, sizeof(lib_active_game), "%s", game_id);
	save_active_game_file_locked();

	pthread_mutex_unlock(&lib_mutex);
	return true;
}

const char *replay_library_active_game(void)
{
	pthread_mutex_lock(&lib_mutex);
	const char *ret = lib_active_game;
	pthread_mutex_unlock(&lib_mutex);
	return ret;
}

void replay_library_games(obs_data_array_t *out)
{
	if (!out)
		return;

	pthread_mutex_lock(&lib_mutex);

	if (lib_initialized) {
		for (size_t i = 0; i < lib_games.num; i++) {
			obs_data_t *obj = obs_data_create();
			obs_data_set_string(obj, "gameId", lib_games.array[i].game_id);
			obs_data_set_string(obj, "name", lib_games.array[i].name ? lib_games.array[i].name : "");
			obs_data_array_push_back(out, obj);
			obs_data_release(obj);
		}
	}

	pthread_mutex_unlock(&lib_mutex);
}

bool replay_library_add_entry(struct replay_library_entry *entry)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !entry) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	struct replay_library_entry e = *entry;

	if (e.library_id[0] == '\0') {
		snprintf(e.library_id, sizeof(e.library_id), "lib-%06" PRIu64, lib_next_id++);
		save_next_id_file_locked();
	} else {
		uint64_t num = 0;
		if (sscanf(e.library_id, "lib-%" SCNu64, &num) == 1 && num >= lib_next_id) {
			lib_next_id = num + 1;
			save_next_id_file_locked();
		}
	}

	if (e.game_id[0] == '\0')
		snprintf(e.game_id, sizeof(e.game_id), "%s", lib_active_game);

	if (e.created_ms == 0)
		e.created_ms = (int64_t)(os_gettime_ns() / 1000000ULL);

	if (e.state[0] == '\0')
		snprintf(e.state, sizeof(e.state), "ready");

	bool found = false;
	for (size_t i = 0; i < lib_entries.num; i++) {
		if (strcmp(lib_entries.array[i].library_id, e.library_id) == 0) {
			lib_entries.array[i] = e;
			found = true;
			break;
		}
	}
	if (!found)
		da_push_back(lib_entries, &e);

	save_entry_file_locked(&e);
	*entry = e;

	pthread_mutex_unlock(&lib_mutex);
	return true;
}

bool replay_library_update_entry(const char *library_id, const char *state, const char *file, const char *error)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !library_id || !*library_id) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	struct replay_library_entry *target = NULL;
	for (size_t i = 0; i < lib_entries.num; i++) {
		if (strcmp(lib_entries.array[i].library_id, library_id) == 0) {
			target = &lib_entries.array[i];
			break;
		}
	}

	if (!target) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	if (state)
		snprintf(target->state, sizeof(target->state), "%s", state);
	if (file)
		snprintf(target->file, sizeof(target->file), "%s", file);
	if (error)
		snprintf(target->error, sizeof(target->error), "%s", error);

	save_entry_file_locked(target);

	pthread_mutex_unlock(&lib_mutex);
	return true;
}

bool replay_library_set_tags(const char *library_id, const char *tags)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !library_id || !*library_id) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	struct replay_library_entry *target = NULL;
	for (size_t i = 0; i < lib_entries.num; i++) {
		if (strcmp(lib_entries.array[i].library_id, library_id) == 0) {
			target = &lib_entries.array[i];
			break;
		}
	}

	if (!target) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	snprintf(target->tags, sizeof(target->tags), "%s", tags ? tags : "");
	save_entry_file_locked(target);

	pthread_mutex_unlock(&lib_mutex);
	return true;
}

bool replay_library_get(const char *library_id, struct replay_library_entry *out)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !library_id || !*library_id || !out) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	for (size_t i = 0; i < lib_entries.num; i++) {
		if (strcmp(lib_entries.array[i].library_id, library_id) == 0) {
			*out = lib_entries.array[i];
			pthread_mutex_unlock(&lib_mutex);
			return true;
		}
	}

	pthread_mutex_unlock(&lib_mutex);
	return false;
}

bool replay_library_find_merge(const char *angle, const char *game_id, int64_t range_start_ns, int64_t range_end_ns,
			       struct replay_library_entry *out)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized || !angle || !game_id) {
		pthread_mutex_unlock(&lib_mutex);
		return false;
	}

	for (size_t i = 0; i < lib_entries.num; i++) {
		const struct replay_library_entry *e = &lib_entries.array[i];
		if (strcmp(e->state, "failed") == 0)
			continue;
		if (strcmp(e->angle, angle) != 0)
			continue;
		if (strcmp(e->game_id, game_id) != 0)
			continue;
		if (e->range_start_ns != range_start_ns || e->range_end_ns != range_end_ns)
			continue;

		if (out)
			*out = *e;
		pthread_mutex_unlock(&lib_mutex);
		return true;
	}

	pthread_mutex_unlock(&lib_mutex);
	return false;
}

void replay_library_entries(obs_data_array_t *out)
{
	if (!out)
		return;

	pthread_mutex_lock(&lib_mutex);

	if (lib_initialized) {
		for (size_t i = 0; i < lib_entries.num; i++) {
			const struct replay_library_entry *e = &lib_entries.array[i];
			obs_data_t *obj = obs_data_create();
			obs_data_set_string(obj, "libraryId", e->library_id);
			obs_data_set_string(obj, "gameId", e->game_id);
			obs_data_set_string(obj, "angle", e->angle);
			obs_data_set_string(obj, "label", e->label);
			obs_data_set_string(obj, "tags", e->tags);
			obs_data_set_string(obj, "state", e->state);
			obs_data_set_int(obj, "createdAt", e->created_ms);
			obs_data_set_int(obj, "durationMs", e->duration_ms);
			obs_data_set_string(obj, "error", e->error);
			obs_data_set_string(obj, "file", e->file);
			obs_data_set_int(obj, "rangeStartNs", e->range_start_ns);
			obs_data_set_int(obj, "rangeEndNs", e->range_end_ns);

			obs_data_array_push_back(out, obj);
			obs_data_release(obj);
		}
	}

	pthread_mutex_unlock(&lib_mutex);
}

static bool tag_matches(const char *query_csv, const char *entry_tags)
{
	if (!query_csv || !*query_csv)
		return true;
	if (!entry_tags || !*entry_tags)
		return false;

	const char *q_p = query_csv;
	while (*q_p) {
		while (*q_p == ' ' || *q_p == '\t' || *q_p == ',')
			q_p++;
		if (!*q_p)
			break;
		const char *q_end = q_p;
		while (*q_end && *q_end != ',')
			q_end++;
		const char *q_trim = q_end;
		while (q_trim > q_p && (q_trim[-1] == ' ' || q_trim[-1] == '\t'))
			q_trim--;

		size_t q_len = (size_t)(q_trim - q_p);
		if (q_len > 0) {
			const char *e_p = entry_tags;
			while (*e_p) {
				while (*e_p == ' ' || *e_p == '\t' || *e_p == ',')
					e_p++;
				if (!*e_p)
					break;
				const char *e_end = e_p;
				while (*e_end && *e_end != ',')
					e_end++;
				const char *e_trim = e_end;
				while (e_trim > e_p && (e_trim[-1] == ' ' || e_trim[-1] == '\t'))
					e_trim--;

				size_t e_len = (size_t)(e_trim - e_p);
				if (e_len == q_len && strncmp(q_p, e_p, q_len) == 0)
					return true;

				e_p = e_end;
			}
		}

		q_p = q_end;
	}

	return false;
}

static int compare_entries_asc(const void *a, const void *b)
{
	const struct replay_library_entry *e1 = *(const struct replay_library_entry **)a;
	const struct replay_library_entry *e2 = *(const struct replay_library_entry **)b;
	if (e1->created_ms < e2->created_ms)
		return -1;
	if (e1->created_ms > e2->created_ms)
		return 1;
	return 0;
}

size_t replay_library_playlist(const char *game_id, const char *tags_csv, char ***ids_out)
{
	pthread_mutex_lock(&lib_mutex);

	if (!lib_initialized) {
		if (ids_out)
			*ids_out = NULL;
		pthread_mutex_unlock(&lib_mutex);
		return 0;
	}

	DARRAY(const struct replay_library_entry *) matches;
	da_init(matches);

	for (size_t i = 0; i < lib_entries.num; i++) {
		const struct replay_library_entry *e = &lib_entries.array[i];
		if (strcmp(e->state, "ready") != 0)
			continue;
		if (game_id && *game_id && strcmp(e->game_id, game_id) != 0)
			continue;
		if (!tag_matches(tags_csv, e->tags))
			continue;

		da_push_back(matches, &e);
	}

	if (matches.num > 1) {
		qsort(matches.array, matches.num, sizeof(const struct replay_library_entry *), compare_entries_asc);
	}

	size_t count = matches.num;
	if (ids_out) {
		if (count > 0) {
			char **ids = bmalloc(count * sizeof(char *));
			for (size_t i = 0; i < count; i++) {
				ids[i] = bstrdup(matches.array[i]->library_id);
			}
			*ids_out = ids;
		} else {
			*ids_out = NULL;
		}
	}

	da_free(matches);
	pthread_mutex_unlock(&lib_mutex);
	return count;
}
