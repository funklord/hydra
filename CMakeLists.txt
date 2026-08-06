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
# Qt WebEngine is desktop-only, and that is not a packaging accident: it does
# not exist for Android at all (architecture doc §19.2), which was verified
# against the installed kits rather than assumed -- `libQt6WebEngine*` is absent
# from every Android ABI and present twenty times over on the desktop one. So
# the component is required where it exists and simply not asked for where it
# does not, and the WebViewBackend seam supplies the difference.
set(HYDRA_QT_COMPONENTS Widgets Network WebChannel Qml)
if(NOT ANDROID)
    list(APPEND HYDRA_QT_COMPONENTS WebEngineWidgets)
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS ${HYDRA_QT_COMPONENTS})

# The only files that name a browser engine. Everything else in this list is
# platform-neutral, which is the property step 3.5 exists to preserve -- and the
# reason an Android build is a new pair of files here rather than a fork.
if(ANDROID)
    set(HYDRA_BACKEND_SOURCES
        src/android_view.cpp
        src/android_intents.cpp
        src/android_dialogs.cpp
        src/android_downloads.cpp
        src/android_downloads.h
        src/android_view.h)
else()
    set(HYDRA_BACKEND_SOURCES
        src/qtwebengine_view.cpp
        src/qtwebengine_view.h
        src/qtwebengine_factory.cpp
        src/qtwebengine_factory.h
        src/qtwebengine_interceptor.cpp
        src/qtwebengine_interceptor.h)
endif()

# libsodium is optional: without it the KeePassXC bridge reports itself
# unusable (the protocol is end-to-end encrypted, so there is no degraded
# mode), but everything else still builds. Install libsodium-dev to enable it.
# Cross builds must not ask the host's pkg-config. It answers, cheerfully, with
# `-L/usr/lib/x86_64-linux-gnu -lsodium` for an arm64 target -- so the configure
# reports the feature *enabled* and the link fails much later with something
# that reads like a toolchain fault. Measured: the first Android configure to
# get this far announced both optional dependencies as found.
#
# Off for Android until the dependency is actually built for it. That is the
# same "found, and the feature is on; absent, and it says so" rule the desktop
# follows -- it just has to be asked the question in the right architecture.
find_package(Qt6 6.4 QUIET COMPONENTS DBus)
find_package(PkgConfig QUIET)
# The guard has to be on the *query*, not on finding pkg-config: Qt's own
# toolchain files look for it first, so PkgConfig_FOUND is already true by the
# time this file runs and guarding the find_package changed nothing at all.
if(PkgConfig_FOUND AND NOT ANDROID)
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
if(NOT ANDROID)
    find_package(LibtorrentRasterbar QUIET)
endif()
if(NOT LibtorrentRasterbar_FOUND AND PkgConfig_FOUND AND NOT ANDROID)
    pkg_check_modules(LIBTORRENT QUIET libtorrent-rasterbar)
endif()
if(LibtorrentRasterbar_FOUND OR LIBTORRENT_FOUND)
    message(STATUS "libtorrent-rasterbar found — BitTorrent downloads enabled")
else()
    message(WARNING "libtorrent-rasterbar not found — BitTorrent downloads disabled")
endif()

# libsecret is optional on the same terms again, and this one degrades rather
# than disappearing: without it the KeePassXC pairing still works, it just lives
# in memory and has to be confirmed again after a restart (architecture doc
# §13.1). That is a real feature loss and the app says so, but it is not a
# reason to refuse to build.
#
# Not a *warning* when missing, unlike the two above. The feature it costs is
# convenience, and a warning here would say "something is wrong" about a build
# that is merely smaller. Off for Android for the reason libsodium is: the
# host's pkg-config will answer questions about the host.
if(PkgConfig_FOUND AND NOT ANDROID)
    pkg_check_modules(SECRET QUIET libsecret-1)
endif()
if(SECRET_FOUND)
    message(STATUS "libsecret found — the KeePassXC pairing will survive a restart")
else()
    message(STATUS "libsecret not found — the KeePassXC pairing will not survive a restart")
endif()

# liblz4 is optional and this one degrades to our own code rather than to
# nothing. Firefox's session file is a raw LZ4 block, and `session_import`
# carries a small bounds-checked decoder for it so the feature works on a
# machine without the library -- including the one it was written on, where the
# only way to test it at all was to implement it. Where liblz4 *is* present it
# is used instead: it is audited and this parses a file another program wrote,
# and "we have a verified implementation" is a weaker argument than "we use the
# one everyone else uses".
if(PkgConfig_FOUND AND NOT ANDROID)
    pkg_check_modules(LZ4 QUIET liblz4)
endif()
if(LZ4_FOUND)
    message(STATUS "liblz4 found — used to read Firefox sessions")
else()
    message(STATUS "liblz4 not found — the built-in LZ4 decoder will be used instead")
endif()

# The sample tree, compiled in. On desktop it is read from the source directory
# or the command line; on Android there is no such directory, so the first run
# has to seed app storage from something the APK carries.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/sample-tree.txt
               ${CMAKE_CURRENT_BINARY_DIR}/sample-tree.txt COPYONLY)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hydra_seed.qrc
"<RCC><qresource prefix=\"/\"><file>sample-tree.txt</file></qresource></RCC>")

# `qt_add_executable`, not `add_executable`, and the difference only shows up on
# Android: there an app is a *shared library* that the Java launcher loads
# (`libhydra_arm64-v8a.so`), and androiddeployqt has nothing to package if the
# target is an ELF executable. That is exactly what happened -- the `apk` target
# reported success and produced no APK, because there was nothing for it to put
# in one. On desktop this is the same executable it always was.
qt_add_executable(hydra
    icons/hydra.qrc
    ${CMAKE_CURRENT_BINARY_DIR}/hydra_seed.qrc
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/tab_tree_model.cpp
    src/tab_tree_model.h
    src/tab_tree_view.cpp
    src/tab_tree_view.h
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
    src/bridge_invoker.cpp
    src/settings_bundle.cpp
    src/theme.cpp
    src/theme.h
    src/scheme_rules.cpp
    src/cosmetic_filters.cpp
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
    src/credential_store.cpp
    src/session_import.cpp
    src/session_import.h
    src/session_mirror.cpp
    src/session_mirror.h
    src/credential_store.h
    src/keepass_protocol.cpp
    src/keepass_protocol.h
    src/keepass_bridge.cpp
    src/keepass_bridge.h
    src/autofill_controller.cpp
    src/autofill_controller.h
    src/consent_blocker.cpp
    src/consent_blocker.h
    src/annoyance_log.cpp
    src/tree_invariants.cpp
    src/annoyed_dialog.cpp
    src/site_rules.cpp
    src/site_rules.h
    src/consent_dialog.cpp
    src/consent_dialog.h
    src/antiadblock_watch.cpp
    src/antiadblock_watch.h
    src/autofill_script.h
    src/auth_dialog.cpp
    src/auth_dialog.h
    src/find_bar.cpp
    src/find_bar.h
    src/element_picker.cpp
    src/element_picker.h
    src/picker_script.h
    # The web view seam: the shell talks only to these two interfaces
    # (architecture doc §19.2); the qtwebengine_* pair is the desktop
    # implementation, and an Android one slots in beside it later.
    src/web_view_backend.h
    src/web_view_factory.h
    ${HYDRA_BACKEND_SOURCES}
)

if(NOT ANDROID)
    # Warnings on, and not as a gesture: the whole of src/ compiles clean under
# these on both the Qt this is built against and the newest one it has been
# tried on, which was measured rather than hoped. The point of a clean build is
# that the next warning is visible instead of being the sixth in a list nobody
# reads. Not -Werror: a warning that appears on somebody else's compiler should
# not stop them building.
if(NOT MSVC)
    target_compile_options(hydra PRIVATE -Wall -Wextra)
endif()

target_link_libraries(hydra PRIVATE Qt6::WebEngineWidgets)
endif()

# The desktop's colour scheme comes over the XDG portal, which is DBus. Optional
# on purpose: it is how *Linux* answers, and Windows, macOS and Android all have
# their own answer that Qt already reads. Without it the theme falls back to
# Qt's hint and then to reading the palette it was given.
if(TARGET Qt6::DBus)
    target_link_libraries(hydra PRIVATE Qt6::DBus)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_DBUS)
else()
    message(STATUS "Qt6::DBus not found — the desktop colour-scheme portal will not be asked")
endif()

# The Android package's own files: manifest and launcher icons, generated from
# the same drawing the desktop uses (`icons/`). Without this the app wears Qt's
# default icon and is called by the target name.
if(ANDROID)
    set_property(TARGET hydra PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
                 ${CMAKE_CURRENT_SOURCE_DIR}/android)

    # The package had no version at all: `aapt2 dump badging` reported
    # versionCode='' and versionName='', because the manifest carried neither
    # attribute and androiddeployqt only fills in placeholders it can find. An
    # apk with no versionCode installs, and then nothing can ever be an upgrade
    # of it -- Android compares that integer and treats absent as zero.
    #
    # Both come from project(VERSION) so there is one place to change them.
    # ANDROID_VERSION_CODE is separate and must rise by one on every release
    # that is meant to replace an earlier one; a dotted version cannot be
    # compared as an integer, so it cannot be derived.
    set(ANDROID_VERSION_CODE "1" CACHE STRING
        "Android versionCode; must increase for each published build")
    set_property(TARGET hydra PROPERTY QT_ANDROID_VERSION_NAME ${PROJECT_VERSION})
    set_property(TARGET hydra PROPERTY QT_ANDROID_VERSION_CODE ${ANDROID_VERSION_CODE})
endif()

target_link_libraries(hydra PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::WebChannel
    Qt6::Qml
)

if(SODIUM_FOUND)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_SODIUM)
    target_include_directories(hydra PRIVATE ${SODIUM_INCLUDE_DIRS})
    target_link_libraries(hydra PRIVATE ${SODIUM_LIBRARIES})
endif()

if(LZ4_FOUND)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_LZ4)
    target_include_directories(hydra PRIVATE ${LZ4_INCLUDE_DIRS})
    target_link_libraries(hydra PRIVATE ${LZ4_LIBRARIES})
endif()

if(SECRET_FOUND)
    target_compile_definitions(hydra PRIVATE HYDRA_HAVE_SECRET)
    target_include_directories(hydra PRIVATE ${SECRET_INCLUDE_DIRS})
    target_link_libraries(hydra PRIVATE ${SECRET_LIBRARIES})
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
