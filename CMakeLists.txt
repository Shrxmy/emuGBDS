cmake_minimum_required(VERSION 3.20)

project(emuGBDS LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(EMUGBDS_DEFAULT_MELONDS OFF)
else()
    set(EMUGBDS_DEFAULT_MELONDS ON)
endif()

option(EMUGBDS_ENABLE_MELONDS "Build with melonDS core adapter support" ${EMUGBDS_DEFAULT_MELONDS})
option(EMUGBDS_ENABLE_MGBA "Build with mGBA core adapter support" ON)

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

if(EMUGBDS_ENABLE_MELONDS)
    set(ENABLE_JIT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
    set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
    set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
    set(DIRENT_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/dirent" CACHE PATH "" FORCE)
    add_subdirectory(melonDS-master EXCLUDE_FROM_ALL)
endif()

if(EMUGBDS_ENABLE_MGBA)
    set(LIBMGBA_ONLY ON CACHE BOOL "" FORCE)
    set(M_CORE_GBA ON CACHE BOOL "" FORCE)
    set(M_CORE_GB OFF CACHE BOOL "" FORCE)
    add_subdirectory(mgba-master EXCLUDE_FROM_ALL)
endif()

add_executable(emugbds
    host/src/main.cpp
    host/src/core/ModeCoordinator.cpp
    host/src/frontend/GbaSdlFrontend.cpp
    host/src/cores/MGbaCoreAdapter.cpp
    host/src/cores/MelonDsCoreAdapter.cpp
    host/src/melonds/MelonDsPlatformShim.cpp
)

target_include_directories(emugbds PRIVATE host/include)

if(EMUGBDS_ENABLE_MELONDS)
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_MELONDS=1)
    target_link_libraries(emugbds PRIVATE core)
else()
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_MELONDS=0)
endif()

if(EMUGBDS_ENABLE_MGBA)
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_MGBA=1)
    target_compile_definitions(emugbds PRIVATE ENABLE_VFS ENABLE_VFS_FD ENABLE_DIRECTORIES)
    target_link_libraries(emugbds PRIVATE mgba)
else()
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_MGBA=0)
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
