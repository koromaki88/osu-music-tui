cmake_minimum_required(VERSION 3.20)

project(osu_music_player VERSION 0.1.0 LANGUAGES CXX)

option(OSU_PLAYER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(osu_player_set_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
        )
    endif()
endfunction()

function(osu_player_enable_sanitizers target)
    if(OSU_PLAYER_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
pkg_check_modules(JSON_C REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
find_package(Threads REQUIRED)

add_library(osu_player_core
    src/library.cpp
    src/library_loader.cpp
    src/storage_path.cpp
    src/track.cpp
    src/player_controller.cpp
    src/library_view.cpp
    src/terminal_text.cpp
    src/library_refresh.cpp
    src/settings.cpp
)

target_include_directories(osu_player_core PUBLIC include)
target_link_libraries(osu_player_core PRIVATE PkgConfig::JSON_C Threads::Threads)
osu_player_set_warnings(osu_player_core)
osu_player_enable_sanitizers(osu_player_core)

add_library(osu_player_audio
    src/audio_player.cpp
    src/audio_probe.cpp
)
target_include_directories(osu_player_audio PUBLIC include)
target_link_libraries(osu_player_audio PRIVATE PkgConfig::FFMPEG PkgConfig::SDL2)
osu_player_set_warnings(osu_player_audio)
osu_player_enable_sanitizers(osu_player_audio)

add_executable(osu-player src/main.cpp src/play_command.cpp src/tui_command.cpp)

target_link_libraries(osu-player PRIVATE osu_player_core osu_player_audio PkgConfig::NCURSESW PkgConfig::FFMPEG)
osu_player_set_warnings(osu-player)
osu_player_enable_sanitizers(osu-player)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(storage-path-tests tests/storage_path_test.cpp)
    target_link_libraries(storage-path-tests PRIVATE osu_player_core)
    osu_player_set_warnings(storage-path-tests)
    osu_player_enable_sanitizers(storage-path-tests)
    add_test(NAME storage-path COMMAND storage-path-tests)

    add_executable(library-tests tests/library_test.cpp)
    target_link_libraries(library-tests PRIVATE osu_player_core)
    osu_player_set_warnings(library-tests)
    osu_player_enable_sanitizers(library-tests)
    add_test(NAME library COMMAND library-tests)

    add_executable(player-controller-tests tests/player_controller_test.cpp)
    target_link_libraries(player-controller-tests PRIVATE osu_player_core)
    osu_player_set_warnings(player-controller-tests)
    osu_player_enable_sanitizers(player-controller-tests)
    add_test(NAME player-controller COMMAND player-controller-tests)

    add_executable(tui-model-tests tests/tui_model_test.cpp)
    target_link_libraries(tui-model-tests PRIVATE osu_player_core)
    osu_player_set_warnings(tui-model-tests)
    osu_player_enable_sanitizers(tui-model-tests)
    add_test(NAME tui-model COMMAND tui-model-tests)
    add_test(NAME tui COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/tui_integration.py --player-binary $<TARGET_FILE:osu-player>)
    add_test(NAME refresh COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/refresh_integration.py --player-binary $<TARGET_FILE:osu-player>)

    add_executable(audio-player-tests tests/audio_player_test.cpp)
    target_link_libraries(audio-player-tests PRIVATE osu_player_audio Threads::Threads)
    osu_player_set_warnings(audio-player-tests)
    osu_player_enable_sanitizers(audio-player-tests)
    add_test(NAME audio COMMAND Python3::Interpreter
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_integration.py
        --test-binary $<TARGET_FILE:audio-player-tests>
        --player-binary $<TARGET_FILE:osu-player>)
    add_test(NAME indexer-verification COMMAND Python3::Interpreter
        -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p "test_*.py")
endif()
