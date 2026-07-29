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
    src/MainWindow.cpp
    src/MainWindow.h
    src/TabTreeModel.cpp
    src/TabTreeModel.h
    src/TreeSortProxy.cpp
    src/TreeSortProxy.h
    src/TreeOutline.cpp
    src/TreeOutline.h
    src/StateStore.cpp
    src/StateStore.h
    src/Policy.cpp
    src/Policy.h
    src/PolicyEngine.cpp
    src/PolicyEngine.h
    src/RequestInterceptor.cpp
    src/RequestInterceptor.h
    src/SitePolicyDialog.cpp
    src/SitePolicyDialog.h
    src/Node.h
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
