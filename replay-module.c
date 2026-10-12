#include "version.h"
#include <obs-module.h>
#include "replay-save-queue.h"

/* Module entry points live here (not in replay.c) so unit tests can link the shared
 * helpers in replay.c without pulling in every source type and the save queue. */

extern struct obs_source_info replay_filter_info;
extern struct obs_source_info replay_filter_audio_info;
extern struct obs_source_info replay_filter_async_info;
extern struct obs_source_info replay_source_info;

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Replay Source] loaded version %s", PROJECT_VERSION);
	obs_register_source(&replay_source_info);
	obs_register_source(&replay_filter_info);
	obs_register_source(&replay_filter_audio_info);
	obs_register_source(&replay_filter_async_info);
	replay_save_queue_init();
	return true;
}

void obs_module_unload(void)
{
	replay_save_queue_shutdown();
}
