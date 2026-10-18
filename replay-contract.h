#pragma once

#include <obs-module.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MulticamReplay operator contract (docs/BEDIENVERTRAG.md). One replay channel per OBS instance. */

void replay_contract_init(void);
void replay_contract_shutdown(void);
void replay_contract_register_vendor(void);

/* Handles one contract request. Both obs_data may be NULL only for hotkey dispatch; response is filled
 * with ok/code/message. Safe to call from tests without obs-websocket. */
void replay_contract_request(const char *type, obs_data_t *request, obs_data_t *response);

/* Test support: sets the program angle without an OBS frontend scene change. */
void replay_contract_test_set_program_angle(const char *angle);

#ifdef __cplusplus
}
#endif
