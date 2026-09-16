cmake_minimum_required(VERSION 3.20)
project(PrivateStorageMaster LANGUAGES CXX)

# Private Storage Master: an ASI plugin for Crimson Desert that opens Private
# Storage and the five housing chests from anywhere. It continues Private
# Storage Anywhere 1.6.0 by Stevi2195, whose single source file is
# src/dllmain.cpp. The plugin reads the ini and writes the log named after
# itself, so the output name decides both.

if(NOT WIN32)
    message(FATAL_ERROR "Private Storage Master targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

add_library(PrivateStorageMaster SHARED
    src/dllmain.cpp
)

target_include_directories(PrivateStorageMaster PRIVATE src)
target_link_libraries(PrivateStorageMaster PRIVATE psapi)
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
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/PrivateStorageMaster.ini" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Staging dist/"
    VERBATIM
)
