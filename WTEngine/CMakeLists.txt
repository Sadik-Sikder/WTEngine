cmake_minimum_required(VERSION 3.15)
project(WTEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WTEngine requires a 64-bit build (libs/glfw/glfw3.lib is x64-only)")
endif()

add_executable(WTEngine
    src/AddressBar.cpp
    src/CSS.cpp
    src/Engine.cpp
    src/EngineForms.cpp
    src/TextEditor.cpp
    src/Fetcher.cpp
    src/HTMLParser.cpp
    src/Layout.cpp
    src/main.cpp
    src/OpenGLRenderer.cpp
)

target_include_directories(WTEngine PRIVATE include)

find_library(GLFW3_LIB glfw3 PATHS "${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw" NO_DEFAULT_PATH)

target_link_libraries(WTEngine PRIVATE "${GLFW3_LIB}" opengl32 wininet)
