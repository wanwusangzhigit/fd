# Native C++ implementation of WiFi Direct File Transfer
# Cross-platform (Linux + Windows) CLI + interactive UI client that speaks
# the same FDFT v2 binary protocol as the Android app, enabling file
# transfer between desktop and Android devices.
#
# Building:
#   cmake -S native -B build/native
#   cmake --build build/native -j
#   ctest --test-dir build/native --output-on-failure
#
# Required system libraries:
#   Linux:   libbluetooth-dev (BlueZ RFCOMM), libncursesw5-dev
#   Windows: Win32 built-in (Ws2_32, BthProps), PDCurses (optional)

cmake_minimum_required(VERSION 3.16)

project(wfd_transfer
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Cross-platform FDFT-compatible file transfer client"
)

# --- Compile settings -----------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 大量二进制传输路径下未使用异常返回值要避免误报；用警告等级 -Wall -Wextra 即可。
if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Platform detection ---------------------------------------------------

if(WIN32)
    set(WFD_PLATFORM_WINDOWS ON)
    set(WFD_PLATFORM_LINUX OFF)
elseif(UNIX)
    set(WFD_PLATFORM_WINDOWS OFF)
    set(WFD_PLATFORM_LINUX ON)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# --- Library: core (protocol, transports, no UI) --------------------------

find_package(Threads REQUIRED)

add_library(wfd_core STATIC
    src/Protocol.cpp
    src/Platform.cpp
    src/Hash.cpp
    src/TcpTransport.cpp
    src/BluetoothTransport.cpp
    src/Discovery.cpp
    src/FileSink.cpp
    src/Progress.cpp
    src/Util.cpp
)
target_include_directories(wfd_core PUBLIC src)
target_link_libraries(wfd_core PUBLIC Threads::Threads)

if(WFD_PLATFORM_WINDOWS)
    target_link_libraries(wfd_core PUBLIC ws2_32 bthprops)
    # Target Windows Vista+ so inet_ntop / GetTickCount64 / inet_pton are
    # declared. MinGW defaults to `_WIN32_WINNT=0x0502` (XP) which omits
    # those APIs from `<ws2tcpip.h>` / `<windows.h>`.
    target_compile_definitions(wfd_core PUBLIC
        WFD_WIN32=1
        _WIN32_WINNT=0x0600
        WINVER=0x0600
        _WIN32_IE=0x0600
    )
    # Statically link the C++ runtime so the produced .exe runs on Windows
    # machines that don't have libgcc_s_seh / libstdc++ installed.
    target_link_options(wfd_core PUBLIC -static-libgcc -static-libstdc++)
else()
    # Linux BlueZ + ncurses 仅作为可链接库
    find_library(BLUEZ_LIB bluetooth)
    find_path(BLUEZ_INCLUDE bluetooth/rfcomm.h)
    if(BLUEZ_LIB AND BLUEZ_INCLUDE)
        target_link_libraries(wfd_core PUBLIC ${BLUEZ_LIB})
        target_include_directories(wfd_core PUBLIC ${BLUEZ_INCLUDE})
        target_compile_definitions(wfd_core PUBLIC WFD_HAVE_BLUEZ=1)
    else()
        message(WARNING "BlueZ dev headers not found; Bluetooth transport will be a stub")
    endif()
endif()

# --- Executable: CLI ------------------------------------------------------

add_executable(wfd_transfer src/main.cpp)
target_link_libraries(wfd_transfer PRIVATE wfd_core)
if(WFD_PLATFORM_WINDOWS)
    target_link_options(wfd_transfer PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

# --- Interactive UI executable (ncurses / PDCurses) -----------------------

if(WFD_PLATFORM_LINUX)
    set(CURSES_LIB ncursesw)
    set(CURSES_NEED_NCURSES ON)
elseif(WFD_PLATFORM_WINDOWS)
    set(CURSES_LIB pdcurses)
endif()
# 用 check_include / find_library 自己手工探测以避免 CMake 旧模块的怪癖
if(WFD_PLATFORM_LINUX)
    find_path(CURSES_INCLUDE_DIR NAMES ncurses.h)
    find_library(CURSES_LIBRARY NAMES ncursesw ncurses)
    if(CURSES_INCLUDE_DIR AND CURSES_LIBRARY)
        add_executable(wfd_tui src/tui.cpp)
        target_include_directories(wfd_tui PRIVATE ${CURSES_INCLUDE_DIR})
        target_link_libraries(wfd_tui PRIVATE wfd_core ${CURSES_LIBRARY})
        target_compile_definitions(wfd_tui PRIVATE WFD_HAVE_CURSES=1)
        message(STATUS "ncurses found: ${CURSES_LIBRARY}")
    else()
        message(STATUS "ncurses not found; skipping wfd_tui (install libncursesw5-dev)")
    endif()
endif()

# --- Tests ----------------------------------------------------------------

enable_testing()

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE wfd_core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_util tests/test_util.cpp)
target_link_libraries(test_util PRIVATE wfd_core)
add_test(NAME util COMMAND test_util)

add_executable(test_hash tests/test_hash.cpp)
target_link_libraries(test_hash PRIVATE wfd_core)
add_test(NAME hash COMMAND test_hash)

add_executable(test_filesink tests/test_filesink.cpp)
target_link_libraries(test_filesink PRIVATE wfd_core)
add_test(NAME filesink COMMAND test_filesink)

add_executable(test_integration tests/test_integration.cpp)
target_link_libraries(test_integration PRIVATE wfd_core)
add_test(NAME integration COMMAND test_integration)

# Discovery test binds UDP broadcast; skip via `ctest -E discovery` on hosts
# where that's not allowed (containers without CAP_NET_RAW, etc.).
add_executable(test_discovery tests/test_discovery.cpp)
target_link_libraries(test_discovery PRIVATE wfd_core)
add_test(NAME discovery COMMAND test_discovery)

# --- Installation ---------------------------------------------------------

install(TARGETS wfd_transfer wfd_core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES
    src/Protocol.h
    src/Hash.h
    src/Platform.h
    src/Util.h
    src/FileSink.h
    src/Discovery.h
    src/TcpTransport.h
    src/BluetoothTransport.h
    src/Progress.h
    DESTINATION include/wfd
)

# --- Packaging ------------------------------------------------------------

set(CPACK_PACKAGE_NAME "wfd_transfer")
set(CPACK_PACKAGE_VENDOR "wfd-transfer")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Cross-platform FDFT-compatible file transfer (Linux + Windows)")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/../LICENSE")
set(CPACK_PACKAGE contact "wfd@example.com")

if(WFD_PLATFORM_LINUX)
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libbluetooth3, libncursesw6")
    set(CPACK_DEBIAN_PACKAGE_SECTION "comm")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "wfd <wfd@example.com>")
elseif(WFD_PLATFORM_WINDOWS)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_PACKAGE_NAME "wfd_transfer")
endif()

include(CPack)
