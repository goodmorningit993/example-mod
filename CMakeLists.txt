cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Template VERSION 1.0.0)

# Add all source files inside src (recursively)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

# Set up the mod binary
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

CPMAddPackage("gh:matcool/gd-imgui-cocos#main")

# imgui-cocos was written for older Geode headers, so update its keyboard/IME signatures
file(GLOB_RECURSE IMGUI_COCOS_FILES
    "${gd-imgui-cocos_SOURCE_DIR}/*.hpp"
    "${gd-imgui-cocos_SOURCE_DIR}/*.cpp")
foreach(F ${IMGUI_COCOS_FILES})
    file(READ "${F}" ORIG)
    string(REGEX REPLACE "insertText\\(const char\\* *([A-Za-z_]*), *int *([A-Za-z_]*)\\)" "insertText(const char* \\1, int \\2, cocos2d::enumKeyCodes)" CONTENT "${ORIG}")
    string(REGEX REPLACE "(keyDown|keyUp)\\((cocos2d::)?enumKeyCodes *([A-Za-z_]*)\\)" "\\1(\\2enumKeyCodes \\3, double)" CONTENT "${CONTENT}")
    if(NOT "${CONTENT}" STREQUAL "${ORIG}")
        file(WRITE "${F}" "${CONTENT}")
    endif()
endforeach()

CPMAddPackage("gh:maxnut/GDReplayFormat#gdr2")
target_link_libraries(${PROJECT_NAME} imgui-cocos libGDR)

# Set up dependencies, resources, and link Geode.
setup_geode_mod(${PROJECT_NAME})
