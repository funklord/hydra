# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.19)

# Rename the project here (and the executable name below) to whatever final
# name you pick — that's the entire rename cost.
project(Hydra VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 6.4 is a real floor, not a guess: the menu bar uses the
# addAction(text, shortcut, receiver, member) argument order, which is the
# replacement Qt 6.4 introduced (the older order is deprecated from 6.4 on).
# Asking for it here turns "too old Qt" into a clear configure-time failure
# instead of a confusing compile error. Developed and tested against 6.8.2.
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets WebEngineWidgets Network)

add_executable(hydra
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
    src/download_manager.cpp
    src/download_manager.h
    src/filter_list.cpp
    src/filter_list.h
    src/filter_signals.cpp
    src/filter_signals.h
    src/filter_dialog.cpp
    src/filter_dialog.h
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
)

# Copy the sample tree next to the binary so the app finds it on first run.
add_custom_command(TARGET hydra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/sample-tree.txt
            $<TARGET_FILE_DIR:hydra>/sample-tree.txt)
