cmake_minimum_required(VERSION 3.28)
project(qqq_options_terminal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(TERMINAL_USE_IMPLOT3D "Use ImPlot3D for vol surfaces (falls back to a 2D heat-slice view if OFF)" ON)

include(FetchContent)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(implot GIT_REPOSITORY https://github.com/epezent/implot.git GIT_TAG v0.16)
FetchContent_MakeAvailable(implot)

if(TERMINAL_USE_IMPLOT3D)
    FetchContent_Declare(implot3d GIT_REPOSITORY https://github.com/brenocq/implot3d.git GIT_TAG main)
    FetchContent_MakeAvailable(implot3d)
endif()

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(implot STATIC ${implot_SOURCE_DIR}/implot.cpp ${implot_SOURCE_DIR}/implot_items.cpp)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

if(TERMINAL_USE_IMPLOT3D)
    add_library(implot3d STATIC
        ${implot3d_SOURCE_DIR}/implot3d.cpp
        ${implot3d_SOURCE_DIR}/implot3d_items.cpp
        ${implot3d_SOURCE_DIR}/implot3d_meshes.cpp
    )
    target_include_directories(implot3d PUBLIC ${implot3d_SOURCE_DIR})
    target_link_libraries(implot3d PUBLIC imgui)
endif()

add_executable(qqq_terminal src/main.cpp)
target_include_directories(qqq_terminal PRIVATE include)
target_link_libraries(qqq_terminal PRIVATE imgui implot OpenGL::GL)

if(TERMINAL_USE_IMPLOT3D)
    target_link_libraries(qqq_terminal PRIVATE implot3d)
    target_compile_definitions(qqq_terminal PRIVATE TERMINAL_HAS_IMPLOT3D=1)
endif()

target_compile_features(qqq_terminal PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(qqq_terminal PRIVATE /W4)
else()
    target_compile_options(qqq_terminal PRIVATE -Wall -Wextra)
endif()

# --- IBKR (optional, off by default; see TODOs in main.cpp) ----------------
option(TERMINAL_ENABLE_IBKR "Build with IBKR TWS C++ API client" OFF)
set(IBKR_API_ROOT "" CACHE PATH "Path to IBKR TWS API C++ client source")
if(TERMINAL_ENABLE_IBKR)
    if(NOT IBKR_API_ROOT)
        message(FATAL_ERROR "TERMINAL_ENABLE_IBKR=ON requires -DIBKR_API_ROOT=/path/to/twsapi")
    endif()
    file(GLOB IBKR_SOURCES ${IBKR_API_ROOT}/client/*.cpp)
    add_library(twsapi STATIC ${IBKR_SOURCES})
    target_include_directories(twsapi PUBLIC ${IBKR_API_ROOT}/client)
    target_link_libraries(qqq_terminal PRIVATE twsapi)
    target_compile_definitions(qqq_terminal PRIVATE TERMINAL_HAS_IBKR=1)
endif()
