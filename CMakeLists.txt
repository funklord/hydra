# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.19)

# Rename the project here (and the executable name below) to whatever final
# name you pick — that's the entire rename cost.
project(Hydra VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 6.4 is a real floor, not a guess: the menu bar uses the
# addAction(text, shortcut, receiver, member) argument order, which is the
# replacement Qt 6.4 introduced (the older order is deprecated from 6.4 on).
# Asking for it here turns "too old Qt" into a clear configure-time failure
# instead of a confusing compile error. Developed and tested against 6.8.2.
# Qml is here for QJSEngine only — the sandbox that runs generated site
# extractors (architecture doc §11.5). No QML is used anywhere in the UI.
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets WebEngineWidgets Network WebChannel Qml)

# libsodium is optional: without it the KeePassXC bridge reports itself
# unusable (the protocol is end-to-end encrypted, so there is no degraded
# mode), but everything else still builds. Install libsodium-dev to enable it.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SODIUM QUIET libsodium)
endif()
if(SODIUM_FOUND)
    message(STATUS "libsodium found — KeePassXC password manager enabled")
else()
    message(WARNING "libsodium not found — KeePassXC password manager disabled")
endif()

# libtorrent-rasterbar is optional on the same terms: without it the BitTorrent
# download source reports itself unavailable and the shell does not add it, so
# everything else still builds. There is no degraded mode -- a torrent engine
# cannot be faked. Install libtorrent-rasterbar-dev to enable it.
#
# NOTE the name: both rasterbar's and rakshasa's libraries install a pkg-config
# file called "libtorrent", and the bare name resolves to rakshasa's, which is a
# different library with a different API (architecture doc 11.4). Ask for the
# CMake package, which is unambiguous, and fall back to the *qualified*
# pkg-config name -- never the bare one.
find_package(LibtorrentRasterbar QUIET)
if(NOT LibtorrentRasterbar_FOUND AND PkgConfig_FOUND)
    pkg_check_modules(LIBTORRENT QUIET libtorrent-rasterbar)
endif()
if(LibtorrentRasterbar_FOUND OR LIBTORRENT_FOUND)
    message(STATUS "libtorrent-rasterbar found — BitTorrent downloads enabled")
else()
    message(WARNING "libtorrent-rasterbar not found — BitTorrent downloads disabled")
endif()

add_executable(hydra
    icons/hydra.qrc
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/tab_tree_model.cpp
    src/tab_tree_model.h
    src/tree_sort_proxy.cpp
    src/tree_sort_proxy.h
    src/tree_outline.cpp
    src/tree_outline.h
    src/state_store.cpp
    src/state_store.h
    src/policy.cpp
    src/policy.h
    src/policy_engine.cpp
    src/policy_engine.h
    src/request_filter.cpp
    src/request_filter.h
    src/site_policy_dialog.cpp
    src/site_policy_dialog.h
    src/node.h
    src/kiosk_controller.cpp
    src/kiosk_controller.h
    # AI reorganizer (architecture doc §9): provider seam, payload
    # serialization, the diff/accept pipeline, and its dialog.
    src/ai_provider.h
    src/ollama_provider.cpp
    src/ollama_provider.h
    src/claude_provider.cpp
    src/claude_provider.h
    src/tree_serializer.cpp
    src/tree_serializer.h
    src/tree_diff.cpp
    src/tree_diff.h
    src/reorganize_dialog.cpp
    src/reorganize_dialog.h
    # Interceptor consumers (architecture doc §11–§12): media detection with
    # downloads and external-player handoff, and the filter-evolution loop.
    src/media_detector.cpp
    src/media_detector.h
    src/media_dialog.cpp
    src/media_dialog.h
    src/player_launcher.cpp
    src/player_launcher.h
    # yt-dlp handoff (architecture doc §11.5): the first thing to try, because
    # a real URL makes every downstream feature work normally.
    src/ytdlp_resolver.cpp
    src/ytdlp_resolver.h
    # The Media Source tap (architecture doc §11.6): main-world hook, isolated
    # relay, and the shell-side record of what a page is actually playing.
    src/mse_tap.cpp
    src/mse_tap.h
    src/capture_source.cpp
    src/capture_source.h
    # Generated site extractors (architecture doc §11.5): the sandbox they run
    # in, the rule that a proposal cannot invent a URL, and the per-site store.
    src/site_extractor.cpp
    src/extractor_helpers.cpp
    src/extractor_helpers.h
    src/network_fetcher.cpp
    src/network_fetcher.h
    src/site_extractor.h
    src/extractor_signals.cpp
    src/extractor_signals.h
    src/extractor_dialog.cpp
    src/extractor_dialog.h
    src/download_manager.cpp
    src/download_manager.h
    # The download transport seam (architecture doc §11.4): the manager owns
    # the queue and consent, a source owns the bytes. http_download_source is
    # the first implementation; a torrent source slots in beside it.
    src/download_source.h
    src/http_download_source.cpp
    src/http_download_source.h
    src/torrent_download_source.cpp
    src/torrent_download_source.h
    src/downloads_dialog.cpp
    src/downloads_dialog.h
    src/settings_dialog.cpp
    src/settings_dialog.h
    src/local_proxy.cpp
    src/local_proxy.h
    src/stream_probe.cpp
    src/stream_probe.h
    src/hls_playlist.cpp
    src/hls_playlist.h
    src/hls_assembler.cpp
    src/hls_assembler.h
    src/filter_list.cpp
    src/filter_list.h
    src/filter_signals.cpp
    src/filter_signals.h
    src/filter_dialog.cpp
    src/filter_dialog.h
    # Password manager (architecture doc §13): KeePassXC-Browser protocol
    # client, the QWebChannel autofill layer, and Autofill as a policy feature.
    src/box_crypto.cpp
    src/box_crypto.h
    src/keepass_protocol.cpp
    src/keepass_protocol.h
    src/keepass_bridge.cpp
    src/keepass_bridge.h
    src/autofill_controller.cpp
    src/autofill_controller.h
    src/consent_blocker.cpp
    src/consent_blocker.h
    src/consent_rules.cpp
    src/consent_rules.h
    src/consent_dialog.cpp
    src/consent_dialog.h
    src/autofill_script.h
    src/element_picker.cpp
    src/element_picker.h
    src/picker_script.h
    # The web view seam: the shell talks only to these two interfaces
    # (architecture doc §19.2); the qtwebengine_* pair is the desktop
    # implementation, and an Android one slots in beside it later.
    src/web_view_backend.h
    src/web_view_factory.h
    src/qtwebengine_view.cpp
    src/qtwebengine_view.h
    src/qtwebengine_factory.cpp
    src/qtwebengine_factory.h
    src/qtwebengine_interceptor.cpp
    src/qtwebengine_interceptor.h
)

target_link_libraries(hydra PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::Network
    Qt6::WebChannel
    Qt6::Qml
)

if(SODIUM_FOUND)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_SODIUM)
    target_include_directories(hydra PRIVATE ${SODIUM_INCLUDE_DIRS})
    target_link_libraries(hydra PRIVATE ${SODIUM_LIBRARIES})
endif()

if(LibtorrentRasterbar_FOUND)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_LIBTORRENT)
    target_link_libraries(hydra PRIVATE LibtorrentRasterbar::torrent-rasterbar)
elseif(LIBTORRENT_FOUND)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_LIBTORRENT)
    target_include_directories(hydra PRIVATE ${LIBTORRENT_INCLUDE_DIRS})
    target_link_libraries(hydra PRIVATE ${LIBTORRENT_LIBRARIES})
endif()

# Copy the sample tree next to the binary so the app finds it on first run.
add_custom_command(TARGET hydra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/sample-tree.txt
            $<TARGET_FILE_DIR:hydra>/sample-tree.txt)
