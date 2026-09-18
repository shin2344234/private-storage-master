cmake_minimum_required(VERSION 3.20)
project(PrivateStorageMaster LANGUAGES CXX C)

# Private Storage Master: an ASI plugin for Crimson Desert that opens Private
# Storage, the housing chests and the other camp and town storage from anywhere. Rebuilt from scratch for
# 2.02.00; Private Storage Anywhere (Stevi2195) and the 2.00 community build
# (jkiip) are references under upstream/, not sources.

if(NOT WIN32)
    message(FATAL_ERROR "Private Storage Master targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd reads build/compile_commands.json
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# MinHook is here only for its HDE64 length decoder, which farhook uses to
# measure the bytes it steals. No MinHook hook is ever created.
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

add_library(PrivateStorageMaster SHARED
    src/dllmain.cpp
    src/core/mod.cpp
    src/core/log.cpp
    src/core/paths.cpp
    src/core/settings.cpp
    src/game/mem.cpp
    src/game/farhook.cpp
    src/game/addresses.cpp
    src/storage/storage.cpp
    src/storage/pad.cpp
    src/storage/capacity.cpp
    src/storage/inventory.cpp
    src/storage/deposit.cpp
    src/api/api.cpp
)

target_include_directories(PrivateStorageMaster PRIVATE src include ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(PrivateStorageMaster PRIVATE minhook)
target_compile_definitions(PrivateStorageMaster PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(PrivateStorageMaster PRIVATE /utf-8 /W3 /EHsc)

set_target_properties(PrivateStorageMaster PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "PrivateStorageMaster")

if(MSVC)
    target_compile_options(PrivateStorageMaster PRIVATE /guard:cf)
    target_link_options(PrivateStorageMaster PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf)
endif()

add_custom_command(TARGET PrivateStorageMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:PrivateStorageMaster>" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Staging dist/"
    VERBATIM
)
