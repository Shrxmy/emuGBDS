cmake_minimum_required(VERSION 3.20)

project(emuGBDS LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(EMUGBDS_ENABLE_MELONDS "Build with melonDS core adapter support" OFF)

set(EMUGBDS_SDL2_TARGET "")
find_package(SDL2 QUIET CONFIG)
if(SDL2_FOUND)
    set(EMUGBDS_SDL2_TARGET SDL2::SDL2)
endif()

if(NOT SDL2_FOUND)
    set(EMUGBDS_LOCAL_SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdl")
    set(EMUGBDS_LOCAL_SDL2_INCLUDE "${EMUGBDS_LOCAL_SDL_DIR}/include")
    set(EMUGBDS_LOCAL_SDL2_LIB "")
    if(EXISTS "${EMUGBDS_LOCAL_SDL_DIR}/lib/x64/SDL2.lib")
        set(EMUGBDS_LOCAL_SDL2_LIB "${EMUGBDS_LOCAL_SDL_DIR}/lib/x64/SDL2.lib")
    elseif(EXISTS "${EMUGBDS_LOCAL_SDL_DIR}/lib/SDL2.lib")
        set(EMUGBDS_LOCAL_SDL2_LIB "${EMUGBDS_LOCAL_SDL_DIR}/lib/SDL2.lib")
    endif()

    if(EXISTS "${EMUGBDS_LOCAL_SDL2_INCLUDE}/SDL.h" AND EMUGBDS_LOCAL_SDL2_LIB)
        add_library(EMUGBDS_SDL2 INTERFACE IMPORTED)
        target_include_directories(EMUGBDS_SDL2 INTERFACE "${EMUGBDS_LOCAL_SDL2_INCLUDE}")
        target_link_libraries(EMUGBDS_SDL2 INTERFACE "${EMUGBDS_LOCAL_SDL2_LIB}")
        set(SDL2_FOUND TRUE)
        set(EMUGBDS_SDL2_TARGET EMUGBDS_SDL2)
    endif()
endif()

if(SDL2_FOUND)
    set(EMUGBDS_HAS_SDL2_DEFAULT ON)
else()
    set(EMUGBDS_HAS_SDL2_DEFAULT OFF)
endif()

set(EMUGBDS_BUILD_WITH_MELONDS OFF)
if(EMUGBDS_ENABLE_MELONDS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/melonDS-master")
        message(WARNING "EMUGBDS_ENABLE_MELONDS=ON but melonDS-master directory is missing; DS third-party integration will be disabled.")
    else()
    set(ENABLE_JIT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
    set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
    set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
    set(DIRENT_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/dirent" CACHE PATH "" FORCE)
    add_subdirectory(melonDS-master EXCLUDE_FROM_ALL)
    if(TARGET core)
        set_target_properties(core PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON)
    endif()
        set(EMUGBDS_BUILD_WITH_MELONDS ON)
    endif()
endif()

add_executable(emugbds
    host/src/main.cpp
    host/src/core/ModeCoordinator.cpp
    host/src/runtime/RuntimeKernel.cpp
    host/src/boot/BootPolicy.cpp
    host/src/scheduler/FrameScheduler.cpp
    host/src/ingba/Core.cpp
    host/src/inds/Core.cpp
    host/src/gba/GbaCartridgeHeader.cpp
    host/src/nds/NdsCartridgeHeader.cpp
    host/src/cores/InHouseDsCoreAdapter.cpp
    host/src/cores/InHouseGbaCoreAdapter.cpp
)

if(EMUGBDS_BUILD_WITH_MELONDS)
    target_sources(emugbds PRIVATE
        host/src/cores/MelonDsCoreAdapter.cpp
        host/src/nds/NdsPlatformShim.cpp
    )
endif()

target_include_directories(emugbds PRIVATE host/include)

if(EMUGBDS_BUILD_WITH_MELONDS)
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_MELONDS=1)
    target_link_libraries(emugbds PRIVATE core)
else()
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_MELONDS=0)
endif()

if(SDL2_FOUND)
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_SDL2=1)
    target_link_libraries(emugbds PRIVATE ${EMUGBDS_SDL2_TARGET})
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdl/SDL2.dll")
        add_custom_command(TARGET emugbds POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/sdl/SDL2.dll"
                "$<TARGET_FILE_DIR:emugbds>/SDL2.dll")
    endif()
else()
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_SDL2=0)
endif()

if(WIN32)
    target_compile_definitions(emugbds PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
