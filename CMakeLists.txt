# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.19)

# Rename the project here (and the executable name below) to whatever final
# name you pick — that's the entire rename cost.
project(Hydra VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets WebEngineWidgets)

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
    src/request_interceptor.cpp
    src/request_interceptor.h
    src/site_policy_dialog.cpp
    src/site_policy_dialog.h
    src/node.h
)

target_link_libraries(hydra PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
)

# Copy the sample tree next to the binary so the app finds it on first run.
add_custom_command(TARGET hydra POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/sample-tree.txt
            $<TARGET_FILE_DIR:hydra>/sample-tree.txt)
