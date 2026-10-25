cmake_minimum_required(VERSION 3.20)

# The one place the version lives: shown by --version, the window title, and Help > About.
project(emuGBDS VERSION 0.3.0 LANGUAGES C CXX)

# Link the C runtime statically so the released exe runs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(EMUGBDS_ENABLE_MELONDS "Build with melonDS core adapter support" OFF)
include(CTest)

# Whole-program optimization for Release: the CPU interpreter's hot paths (bus reads, cycle
# timing) cross source files, and inlining them across files is a large speedup.
include(CheckIPOSupported)
check_ipo_supported(RESULT EMUGBDS_IPO_SUPPORTED OUTPUT EMUGBDS_IPO_ERROR LANGUAGES CXX)
if(EMUGBDS_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
# MSVC's default /Ob2 leaves the interpreter's small per-access helpers as calls; /Ob3 inlines them
# (Thumb code runs about 1.5x faster).
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Ob3>")

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
    host/src/ingba/Backup.cpp
    host/src/ingba/CartRtc.cpp
)

# Shared by both cores.
set(EMUGBDS_CORE_COMMON_SOURCES
    host/src/core/SaveFile.cpp
)

set(EMUGBDS_INHOUSE_DS_SOURCES
    host/src/inds/Core.cpp
    host/src/inds/MemoryBus.cpp
    host/src/inds/Cp15.cpp
    host/src/inds/Io.cpp
    host/src/inds/GameCard.cpp
    host/src/inds/Gpu3d.cpp
    host/src/inds/Gpu3dRender.cpp
    host/src/inds/Key1.cpp
    host/src/inds/Backup.cpp
    host/src/inds/Wifi.cpp
    host/src/inds/Sound.cpp
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
    host/src/core/HostPerformance.cpp
    ${EMUGBDS_FRONTEND_SOURCES}
    host/src/core/ModeCoordinator.cpp
    host/src/runtime/RuntimeKernel.cpp
    host/src/boot/BootPolicy.cpp
    host/src/scheduler/FrameScheduler.cpp
    ${EMUGBDS_ARM_CPU_SOURCES}
    ${EMUGBDS_INHOUSE_GBA_SOURCES}
    ${EMUGBDS_INHOUSE_DS_SOURCES}
    ${EMUGBDS_CORE_COMMON_SOURCES}
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
target_compile_definitions(emugbds PRIVATE EMUGBDS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_sources(emugbds PRIVATE host/src/app.rc) # application icon
endif()

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
        ${EMUGBDS_CORE_COMMON_SOURCES}
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

    add_test(NAME cli_version COMMAND emugbds --version)
    set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^emuGBDS ${PROJECT_VERSION}")

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

# Release package: cpack -G ZIP -C Release --config build/CPackConfig.cmake
install(TARGETS emugbds RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md CHANGELOG.md DESTINATION .)
install(FILES host/third_party/imgui/LICENSE.txt DESTINATION licenses RENAME imgui-LICENSE.txt)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdl/SDL2.dll")
    install(FILES sdl/SDL2.dll DESTINATION .)
    install(FILES sdl/LICENSE.txt DESTINATION licenses RENAME SDL2-LICENSE.txt)
endif()
set(CPACK_PACKAGE_FILE_NAME "emuGBDS-${PROJECT_VERSION}-windows-x64")
set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
