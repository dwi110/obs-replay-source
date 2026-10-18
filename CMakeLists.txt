# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.18)
endif()

project(replay-source VERSION 1.8.1)
set(PROJECT_FULL_NAME "Replay Source")

# Set new UUIDs when you start to create a new plugin.
set(MACOS_PACKAGE_UUID "A0B79A76-08BC-420B-AEB9-D1334C3C6244")
set(MACOS_INSTALLER_UUID "22902D34-AB9A-49EE-8012-F60DCAA89EB8")

add_library(${PROJECT_NAME} MODULE)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${PROJECT_NAME} PRIVATE
	replay.c
	replay-module.c
	replay-source.c
	replay-filter.c
	replay-filter-audio.c
	replay-filter-async.c
	replay-save-queue.c
	replay-save-queue.h
	replay-save-fast.c
	replay-save-fast.h
	replay-decode.c
	replay-decode.h
	replay-control.c
	replay-control.h
	replay-contract.c
	replay-contract.h
	replay-library.c
	replay-library.h
	obs-websocket-api.h
	replay.h
	replay-pool.c
	replay-pool.h
	replay-convert.c
	replay-convert.h
	replay-blend.c
	replay-blend.h
	replay-audio-capture.c
	replay-audio-capture.h
	version.h)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  include(cmake/ObsPluginHelpers.cmake)
  set(OBS_FRONTEND_API_NAME "obs-frontend-api")
else()
	set(OBS_FRONTEND_API_NAME "frontend-api")
endif()

if(OS_WINDOWS)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
	target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
elseif(OS_MACOS)
		set(MACOS_BUNDLEID "com.exeldro.${PROJECT_NAME}")
		set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
		set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
		set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
		configure_file(cmake/bundle/macos/installer-macos.pkgproj.in ${CMAKE_BINARY_DIR}/installer-macos.generated.pkgproj)
		target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
	OBS::${OBS_FRONTEND_API_NAME}
	OBS::libobs)

# Faster-than-real-time saving encodes directly with libavcodec/libavformat (FFmpeg >= 6 API). Without FFmpeg the
# plugin builds and saves at real time only. Windows: point REPLAY_FFMPEG_DIR at the FFmpeg (include/, lib/) that the
# target OBS ships; the DLLs are delay-loaded so the plugin still loads on an OBS with another FFmpeg version.
set(REPLAY_FFMPEG_DIR "" CACHE PATH "FFmpeg prefix (include/, lib/) for the fast save path")
if(NOT REPLAY_FFMPEG_DIR AND NOT WIN32)
	find_package(PkgConfig QUIET)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(REPLAY_FFMPEG QUIET IMPORTED_TARGET GLOBAL libavcodec>=60 libavformat>=60 libavutil>=58)
	endif()
endif()

function(replay_enable_fast_save target)
	if(REPLAY_FFMPEG_DIR)
		target_include_directories(${target} BEFORE PRIVATE "${REPLAY_FFMPEG_DIR}/include")
		target_compile_definitions(${target} PRIVATE REPLAY_FAST_SAVE)
		if(MSVC)
			foreach(lib avcodec avformat avutil)
				target_link_libraries(${target} PRIVATE "${REPLAY_FFMPEG_DIR}/lib/${lib}.lib")
			endforeach()
			foreach(hdr libavcodec/version_major.h libavformat/version_major.h libavutil/version.h)
				file(STRINGS "${REPLAY_FFMPEG_DIR}/include/${hdr}" _major REGEX "^#define LIBAV[A-Z]+_VERSION_MAJOR")
				string(REGEX MATCH "LIBAV([A-Z]+)_VERSION_MAJOR +([0-9]+)" _m "${_major}")
				string(TOLOWER "${CMAKE_MATCH_1}" _name)
				target_link_options(${target} PRIVATE "/DELAYLOAD:av${_name}-${CMAKE_MATCH_2}.dll")
			endforeach()
			target_link_libraries(${target} PRIVATE delayimp)
		else()
			foreach(lib avcodec avformat avutil)
				find_library(_ff_${lib} ${lib} PATHS "${REPLAY_FFMPEG_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
				target_link_libraries(${target} PRIVATE "${_ff_${lib}}")
			endforeach()
		endif()
	elseif(TARGET PkgConfig::REPLAY_FFMPEG)
		target_compile_definitions(${target} PRIVATE REPLAY_FAST_SAVE)
		target_link_libraries(${target} PRIVATE PkgConfig::REPLAY_FFMPEG)
	endif()
endfunction()

replay_enable_fast_save(${PROJECT_NAME})
if(REPLAY_FFMPEG_DIR OR TARGET PkgConfig::REPLAY_FFMPEG)
	message(STATUS "replay-source: faster-than-real-time saving enabled")
else()
	message(STATUS "replay-source: no FFmpeg >= 6 found, saving at real time only")
endif()

if(BUILD_OUT_OF_TREE)
    if(NOT LIB_OUT_DIR)
        set(LIB_OUT_DIR "/lib/obs-plugins")
    endif()
    if(NOT DATA_OUT_DIR)
        set(DATA_OUT_DIR "/share/obs/obs-plugins/${PROJECT_NAME}")
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
    install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/${LIB_OUT_DIR})
    install(DIRECTORY data/locale
        DESTINATION ${CMAKE_INSTALL_PREFIX}/${DATA_OUT_DIR})
    setup_plugin_target(${PROJECT_NAME})
else()
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/UI/obs-frontend-api")
    if(OBS_CMAKE_VERSION VERSION_GREATER_EQUAL 3.0.0)
        set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro" PREFIX "")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro")
        setup_plugin_target(${PROJECT_NAME})
    endif()
endif()

option(REPLAY_BUILD_TESTS "Build unit tests" OFF)
if(BUILD_OUT_OF_TREE AND REPLAY_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
