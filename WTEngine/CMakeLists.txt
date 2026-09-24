# Boost.Beast/Asio + OpenSSL (Fetcher.cpp's HTTP client - see its header
# comment for why it isn't WinINet) come from vcpkg, not third_party/ - big
# enough that vendoring the vcpkg-built output wholesale is left as a
# follow-up rather than done here. Point CMAKE_TOOLCHAIN_FILE at vcpkg's
# toolchain before the first project() call (required - CMake only honors
# it that early), unless the caller already set one. Set VCPKG_ROOT to
# wherever vcpkg lives (this project doesn't ship or install vcpkg itself).
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
if(NOT VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "") # static libs, matching qjs/glfw3; dynamic CRT, matching this project's MSVC default
endif()

cmake_minimum_required(VERSION 3.15)
project(WTEngine CXX)

set(CMAKE_CXX_STANDARD 20) # quickjs.h's JS_CFUNC_DEF/JS_CGETSET_DEF macros use designated initializers, C++20 on MSVC
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WTEngine requires a 64-bit build (libs/glfw/glfw3.lib is x64-only)")
endif()

# quickjs-ng: vendored under third_party/, built from its own CMakeLists.txt
# unmodified. Only the "qjs" static library target is used; its CLI/test
# targets (qjsc, qjs_exe, run-test262, ...) come along for free but aren't
# referenced by WTEngine. QJS_ENABLE_INSTALL is turned off since WTEngine
# doesn't install anything and doesn't need quickjs's install rules either.
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/quickjs-ng)

add_executable(WTEngine
    src/AddressBar.cpp
    src/CSS.cpp
    src/Engine.cpp
    src/EngineForms.cpp
    src/TextEditor.cpp
    src/Fetcher.cpp
    src/HTMLParser.cpp
    src/JSBinding.cpp
    src/JSEngine.cpp
    src/Layout.cpp
    src/main.cpp
    src/OpenGLRenderer.cpp
    src/PageLoader.cpp
    src/ResourceLoader.cpp
)

target_include_directories(WTEngine PRIVATE include)

find_library(GLFW3_LIB glfw3 PATHS "${CMAKE_CURRENT_SOURCE_DIR}/libs/glfw" NO_DEFAULT_PATH)

find_package(boost_beast CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED) # gzip/deflate response decompression - see Fetcher.cpp's decompressBody

target_link_libraries(WTEngine PRIVATE "${GLFW3_LIB}" opengl32 wininet qjs
    Boost::beast OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB ws2_32 mswsock crypt32)
