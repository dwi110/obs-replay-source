set(REPLAY_TEST_COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test-save-queue
	test-save-queue.c
	../replay.c
	../replay-source.c
	../replay-filter.c
	../replay-filter-audio.c
	../replay-filter-async.c
	../replay-save-queue.c)
target_include_directories(test-save-queue PRIVATE ${REPLAY_TEST_COMMON_INCLUDES})
target_link_libraries(test-save-queue PRIVATE
	OBS::${OBS_FRONTEND_API_NAME}
	OBS::libobs)
if(UNIX AND NOT APPLE)
	target_link_libraries(test-save-queue PRIVATE m)
endif()
add_test(NAME replay_save_queue COMMAND test-save-queue)
