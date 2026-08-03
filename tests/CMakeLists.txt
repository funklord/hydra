# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test harnesses. Built separately from the app: they link the same sources
# rather than the binary, so each suite pulls in only what it exercises.
#
#   cmake -S tests -B tests/build && cmake --build tests/build -j2
#
# Always name a job count. `-j` with no number is *unlimited* under Make, not
# one per core, and each live driver links Qt WebEngine — that has exhausted
# memory and taken the desktop session down.
#
# Then see tests/README.md — several suites need a local server or a model,
# and the ones under live/ need a running X display and drive the real shell.
cmake_minimum_required(VERSION 3.19)
project(HydraTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Qml Test)
find_package(Qt6 6.4 QUIET COMPONENTS WebEngineWidgets WebChannel)
find_package(PkgConfig QUIET)
find_package(LibtorrentRasterbar QUIET)

set(H ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# --- suites that need nothing but a build ---------------------------------
add_executable(test_seam test_seam.cpp fake_sources.h
    ${H}/download_manager.cpp ${H}/download_source.h
    ${H}/http_download_source.cpp ${H}/media_detector.cpp)
target_link_libraries(test_seam PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(test_pick test_pick.cpp fake_sources.h
    ${H}/download_manager.cpp ${H}/download_source.h
    ${H}/downloads_dialog.cpp ${H}/local_proxy.cpp
    ${H}/player_launcher.cpp ${H}/media_detector.cpp)
target_link_libraries(test_pick PRIVATE Qt6::Widgets Qt6::Network)

add_executable(test_extractor test_extractor.cpp ${H}/antiadblock_watch.cpp
    ${H}/site_rules.cpp
    ${H}/site_extractor.cpp ${H}/extractor_helpers.cpp
    ${H}/extractor_dialog.cpp ${H}/extractor_signals.cpp
    ${H}/stream_probe.cpp ${H}/local_proxy.cpp
    ${H}/request_filter.cpp ${H}/filter_list.cpp ${H}/policy.cpp ${H}/policy_engine.cpp ${H}/ai_provider.h)
target_link_libraries(test_extractor PRIVATE Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Test)

add_executable(test_extloop test_extloop.cpp
    ${H}/site_extractor.cpp ${H}/extractor_helpers.cpp
    ${H}/extractor_dialog.cpp ${H}/extractor_signals.cpp
    ${H}/stream_probe.cpp ${H}/local_proxy.cpp
    ${H}/request_filter.cpp ${H}/filter_list.cpp ${H}/policy.cpp ${H}/policy_engine.cpp ${H}/ai_provider.h)
target_link_libraries(test_extloop PRIVATE Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Test)

add_executable(test_fetcher test_fetcher.cpp ${H}/network_fetcher.cpp
    ${H}/extractor_helpers.cpp ${H}/site_extractor.cpp ${H}/stream_probe.cpp
    ${H}/local_proxy.cpp)
target_link_libraries(test_fetcher PRIVATE Qt6::Core Qt6::Qml Qt6::Network Qt6::Test)

add_executable(test_helpers test_helpers.cpp ${H}/extractor_helpers.cpp
    ${H}/site_extractor.cpp ${H}/stream_probe.cpp ${H}/local_proxy.cpp
    ${H}/policy.cpp ${H}/policy_engine.cpp)
target_link_libraries(test_helpers PRIVATE Qt6::Core Qt6::Qml Qt6::Network Qt6::Test)

add_executable(test_streamtype test_streamtype.cpp ${H}/stream_probe.cpp
    ${H}/local_proxy.cpp)
target_link_libraries(test_streamtype PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(test_settings test_settings.cpp
    ${H}/settings_dialog.cpp ${H}/download_manager.cpp ${H}/download_source.h
    ${H}/torrent_download_source.cpp ${H}/player_launcher.cpp
    ${H}/media_detector.cpp ${H}/ollama_provider.cpp ${H}/claude_provider.cpp
    ${H}/policy.cpp ${H}/policy_engine.cpp ${H}/request_filter.cpp
    ${H}/filter_list.cpp ${H}/site_rules.cpp ${H}/scheme_rules.cpp
    ${H}/cosmetic_filters.cpp
    ${H}/ai_provider.h)
target_link_libraries(test_settings PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)

foreach(t test_seam test_pick test_extractor test_extloop test_settings
         test_streamtype test_helpers test_fetcher)
    target_include_directories(${t} PRIVATE ${H} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# --- suites that need a helper server (see README) -------------------------
add_executable(test_headers test_headers.cpp ${H}/local_proxy.cpp)
target_link_libraries(test_headers PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(test_dlheaders test_dlheaders.cpp
    ${H}/download_manager.cpp ${H}/http_download_source.cpp
    ${H}/media_detector.cpp ${H}/download_source.h)
target_link_libraries(test_dlheaders PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(test_helpers_live test_helpers_live.cpp ${H}/extractor_helpers.cpp
    ${H}/network_fetcher.cpp ${H}/site_extractor.cpp ${H}/stream_probe.cpp
    ${H}/local_proxy.cpp ${H}/extractor_dialog.cpp ${H}/extractor_signals.cpp
    ${H}/request_filter.cpp ${H}/filter_list.cpp ${H}/policy.cpp ${H}/policy_engine.cpp
    ${H}/ai_provider.h)
target_link_libraries(test_helpers_live PRIVATE Qt6::Widgets Qt6::Qml Qt6::Network)

add_executable(test_probe test_probe.cpp ${H}/ollama_provider.cpp ${H}/ai_provider.h)
target_link_libraries(test_probe PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(test_probe_ui test_probe_ui.cpp
    ${H}/settings_dialog.cpp ${H}/download_manager.cpp ${H}/download_source.h
    ${H}/torrent_download_source.cpp ${H}/player_launcher.cpp
    ${H}/media_detector.cpp ${H}/ollama_provider.cpp ${H}/claude_provider.cpp
    ${H}/policy.cpp ${H}/policy_engine.cpp ${H}/request_filter.cpp
    ${H}/filter_list.cpp ${H}/site_rules.cpp ${H}/scheme_rules.cpp
    ${H}/cosmetic_filters.cpp
    ${H}/ai_provider.h)
target_link_libraries(test_probe_ui PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)

add_executable(test_tree test_tree.cpp ${H}/tree_outline.cpp ${H}/tree_serializer.cpp)
target_link_libraries(test_tree PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test_tree PRIVATE ${H} ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_hls test_hls.cpp ${H}/hls_playlist.cpp)
target_link_libraries(test_hls PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test_hls PRIVATE ${H} ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_bridge test_bridge.cpp ${H}/bridge_invoker.cpp)
target_link_libraries(test_bridge PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test_bridge PRIVATE ${H} ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_ytdlp test_ytdlp.cpp ${H}/ytdlp_resolver.cpp)
target_link_libraries(test_ytdlp PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(test_ytdlp_live test_ytdlp_live.cpp ${H}/ytdlp_resolver.cpp)
target_link_libraries(test_ytdlp_live PRIVATE Qt6::Core Qt6::Network)

add_executable(test_live_model test_live_model.cpp
    ${H}/site_extractor.cpp ${H}/extractor_helpers.cpp
    ${H}/extractor_dialog.cpp ${H}/extractor_signals.cpp
    ${H}/stream_probe.cpp ${H}/local_proxy.cpp
    ${H}/request_filter.cpp ${H}/filter_list.cpp ${H}/policy.cpp ${H}/policy_engine.cpp
    ${H}/ollama_provider.cpp ${H}/ai_provider.h)
target_link_libraries(test_live_model PRIVATE Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Test)

foreach(t test_headers test_dlheaders test_probe test_probe_ui test_ytdlp
          test_ytdlp_live test_live_model test_helpers_live)
    target_include_directories(${t} PRIVATE ${H} ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# --- suites needing libtorrent --------------------------------------------
if(LibtorrentRasterbar_FOUND)
    add_executable(test_torrent test_torrent.cpp
        ${H}/download_manager.cpp ${H}/download_source.h ${H}/torrent_download_source.cpp)
    add_executable(test_watch test_watch.cpp
        ${H}/download_manager.cpp ${H}/download_source.h
        ${H}/torrent_download_source.cpp ${H}/local_proxy.cpp)
    foreach(t test_torrent test_watch)
        target_include_directories(${t} PRIVATE ${H} ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${t} PRIVATE HYDRA_HAVE_LIBTORRENT)
        target_link_libraries(${t} PRIVATE Qt6::Core Qt6::Network Qt6::Test
            LibtorrentRasterbar::torrent-rasterbar)
    endforeach()
else()
    message(STATUS "libtorrent-rasterbar not found — test_torrent/test_watch skipped")
endif()

# --- live drivers: real shell, real display -------------------------------
if(Qt6WebEngineWidgets_FOUND)
    file(GLOB APPSRC CONFIGURE_DEPENDS ${H}/*.cpp)
    # main.cpp has its own main(); the android_* files need JNI headers that
    # exist only in the NDK, so a desktop driver that globbed them in would fail
    # to compile files it never calls. By pattern rather than by name: naming
    # them one at a time has already broken this build twice, once per new
    # Android file, and the next one would break it again.
    list(REMOVE_ITEM APPSRC ${H}/main.cpp)
    file(GLOB ANDROID_ONLY_SRC ${H}/android_*.cpp)
    if(ANDROID_ONLY_SRC)
        list(REMOVE_ITEM APPSRC ${ANDROID_ONLY_SRC})
    endif()
    file(GLOB APPHDR CONFIGURE_DEPENDS ${H}/*.h)
    # And their headers with them: AUTOMOC generates the meta-object for any
    # Q_OBJECT it is handed, so leaving a header in emits a vtable referencing
    # the .cpp that was just excluded.
    file(GLOB ANDROID_ONLY_HDR ${H}/android_*.h)
    if(ANDROID_ONLY_HDR)
        list(REMOVE_ITEM APPHDR ${ANDROID_ONLY_HDR})
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM QUIET libsodium)
    endif()
    file(GLOB LIVE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/live/try_*.cpp)
    foreach(src ${LIVE})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src} ${APPSRC} ${APPHDR})
        target_include_directories(${name} PRIVATE ${H} ${SODIUM_INCLUDE_DIRS})
        target_link_libraries(${name} PRIVATE Qt6::Widgets Qt6::WebEngineWidgets
            Qt6::Network Qt6::WebChannel Qt6::Qml ${SODIUM_LIBRARIES})
        if(SODIUM_FOUND)
            target_compile_definitions(${name} PRIVATE HYDRA_HAVE_SODIUM)
        endif()
        if(LibtorrentRasterbar_FOUND)
            target_compile_definitions(${name} PRIVATE HYDRA_HAVE_LIBTORRENT)
            target_link_libraries(${name} PRIVATE LibtorrentRasterbar::torrent-rasterbar)
        endif()
    endforeach()
else()
    message(STATUS "Qt WebEngine not found — live drivers skipped")
endif()
