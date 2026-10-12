set(REPLAY_TEST_COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(test_pool
	test_pool.c
	../replay.c
	../replay-pool.c)
target_include_directories(test_pool PRIVATE ${REPLAY_TEST_COMMON_INCLUDES})
target_link_libraries(test_pool PRIVATE OBS::libobs)
if(UNIX AND NOT APPLE)
	target_link_libraries(test_pool PRIVATE m)
endif()
add_test(NAME replay_pool COMMAND test_pool)

add_executable(test_convert
	test_convert.c
	../replay.c
	../replay-pool.c
	../replay-convert.c)
target_include_directories(test_convert PRIVATE ${REPLAY_TEST_COMMON_INCLUDES})
target_link_libraries(test_convert PRIVATE OBS::libobs)
if(UNIX AND NOT APPLE)
	target_link_libraries(test_convert PRIVATE m)
endif()
add_test(NAME replay_convert COMMAND test_convert)

add_executable(test_tap
	test_tap.c
	../replay.c
	../replay-pool.c
	../replay-convert.c
	../replay-filter-async.c)
target_include_directories(test_tap PRIVATE ${REPLAY_TEST_COMMON_INCLUDES})
target_link_libraries(test_tap PRIVATE OBS::libobs)
if(UNIX AND NOT APPLE)
	target_link_libraries(test_tap PRIVATE m)
endif()
add_test(NAME replay_tap COMMAND test_tap)

add_executable(test-save-queue
	test-save-queue.c
	../replay.c
	../replay-module.c
	../replay-source.c
	../replay-filter.c
	../replay-filter-audio.c
	../replay-filter-async.c
	../replay-pool.c
	../replay-convert.c
	../replay-save-queue.c)
target_include_directories(test-save-queue PRIVATE ${REPLAY_TEST_COMMON_INCLUDES})
target_link_libraries(test-save-queue PRIVATE
	OBS::${OBS_FRONTEND_API_NAME}
	OBS::libobs)
if(UNIX AND NOT APPLE)
	target_link_libraries(test-save-queue PRIVATE m)
endif()
add_test(NAME replay_save_queue COMMAND test-save-queue)
