cmake_minimum_required(VERSION 3.20)

project(emuGBDS LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(EMUGBDS_ENABLE_MELONDS "Build with melonDS core adapter support" OFF)
include(CTest)

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

# Keep core implementation units in one place so the application and tests
# cannot silently drift apart: the shared ARM CPU, the GBA core, and the DS core.
set(EMUGBDS_ARM_CPU_SOURCES
    host/src/arm/ArmCpu.cpp
    host/src/arm/ArmExecute.cpp
    host/src/arm/ThumbExecute.cpp
)

set(EMUGBDS_INHOUSE_GBA_SOURCES
    host/src/ingba/Core.cpp
    host/src/ingba/MemoryBus.cpp
    host/src/ingba/Swi.cpp
    host/src/ingba/Video.cpp
    host/src/ingba/Apu.cpp
)

set(EMUGBDS_INHOUSE_DS_SOURCES
    host/src/inds/Core.cpp
    host/src/inds/MemoryBus.cpp
    host/src/inds/Cp15.cpp
    host/src/inds/Io.cpp
    host/src/inds/GameCard.cpp
    host/src/inds/Backup.cpp
    host/src/inds/Wifi.cpp
    host/src/inds/Spi.cpp
    host/src/inds/MathUnit.cpp
    host/src/inds/Rtc.cpp
    host/src/inds/VideoMemory.cpp
    host/src/inds/Engine2d.cpp
    host/src/inds/Ipc.cpp
    host/src/inds/Timers.cpp
)

# The interactive app's settings file and session wrapper (no SDL needed).
set(EMUGBDS_FRONTEND_SOURCES
    host/src/frontend/Session.cpp
    host/src/frontend/Settings.cpp
)

add_executable(emugbds
    host/src/main.cpp
    ${EMUGBDS_FRONTEND_SOURCES}
    host/src/core/ModeCoordinator.cpp
    host/src/runtime/RuntimeKernel.cpp
    host/src/boot/BootPolicy.cpp
    host/src/scheduler/FrameScheduler.cpp
    ${EMUGBDS_ARM_CPU_SOURCES}
    ${EMUGBDS_INHOUSE_GBA_SOURCES}
    ${EMUGBDS_INHOUSE_DS_SOURCES}
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
    # Dear ImGui (vendored, MIT) draws the menus and panels inside the SDL window.
    set(EMUGBDS_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/host/third_party/imgui")
    add_library(emugbds_imgui STATIC
        ${EMUGBDS_IMGUI_DIR}/imgui.cpp
        ${EMUGBDS_IMGUI_DIR}/imgui_draw.cpp
        ${EMUGBDS_IMGUI_DIR}/imgui_tables.cpp
        ${EMUGBDS_IMGUI_DIR}/imgui_widgets.cpp
        ${EMUGBDS_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${EMUGBDS_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(emugbds_imgui PUBLIC "${EMUGBDS_IMGUI_DIR}" "${EMUGBDS_IMGUI_DIR}/backends")
    target_link_libraries(emugbds_imgui PUBLIC ${EMUGBDS_SDL2_TARGET})

    target_sources(emugbds PRIVATE
        host/src/frontend/App.cpp
        host/src/frontend/FileBrowser.cpp
    )
    target_compile_definitions(emugbds PRIVATE EMUGBDS_HAS_SDL2=1)
    target_link_libraries(emugbds PRIVATE emugbds_imgui ${EMUGBDS_SDL2_TARGET})
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

if(BUILD_TESTING)
    add_executable(emugbds_tests
        tests/TestMain.cpp
        host/src/frontend/Settings.cpp
        host/src/runtime/RuntimeKernel.cpp
        host/src/cores/InHouseDsCoreAdapter.cpp
        host/src/cores/InHouseGbaCoreAdapter.cpp
        ${EMUGBDS_ARM_CPU_SOURCES}
        ${EMUGBDS_INHOUSE_GBA_SOURCES}
        ${EMUGBDS_INHOUSE_DS_SOURCES}
        host/src/gba/GbaCartridgeHeader.cpp
        host/src/nds/NdsCartridgeHeader.cpp
    )
    target_include_directories(emugbds_tests PRIVATE host/include)
    target_compile_definitions(emugbds_tests PRIVATE EMUGBDS_HAS_MELONDS=0 EMUGBDS_HAS_SDL2=0)
    if(WIN32)
        target_compile_definitions(emugbds_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_test(NAME core_unit_tests COMMAND emugbds_tests)

    set(EMUGBDS_TEST_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
    add_test(NAME cli_fixture_setup COMMAND emugbds_tests --emit-fixtures "${EMUGBDS_TEST_FIXTURE_DIR}")
    set_tests_properties(cli_fixture_setup PROPERTIES FIXTURES_SETUP EMUGBDS_CLI_FIXTURES)

    add_test(
        NAME cli_gba_smoke
        COMMAND emugbds --mode gba --gba "${EMUGBDS_TEST_FIXTURE_DIR}/test.gba" --frames 3)
    set_tests_properties(cli_gba_smoke PROPERTIES
        FIXTURES_REQUIRED EMUGBDS_CLI_FIXTURES
        PASS_REGULAR_EXPRESSION "Completed 3 frame\\(s\\) in mode GBA\\.")

    add_test(
        NAME cli_ds_slot2_transition
        COMMAND emugbds --mode ds --slot2 "${EMUGBDS_TEST_FIXTURE_DIR}/test.gba" --boot-mode ds-firmware --trace-transition --frames 3)
    set_tests_properties(cli_ds_slot2_transition PROPERTIES
        FIXTURES_REQUIRED EMUGBDS_CLI_FIXTURES
        PASS_REGULAR_EXPRESSION "Completed 3 frame\\(s\\) in mode GBA\\.")
endif()
