# CMake toolchain file for cross-compiling Windows binaries on a Linux host
# using the MinGW-w64 toolchain (x86_64).
#
# Usage:
#   cmake -S native -B build/win64 \
#         -DCMAKE_TOOLCHAIN_FILE=<repo>/native/cmake/mingw-x86_64.cmake
#   cmake --build build/win64 -j
#
# On Debian / Ubuntu:
#   sudo apt install mingw-w64 g++-mingw-w64-x86_64-posix
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TARGET x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TARGET}-gcc-posix CACHE PATH "")
set(CMAKE_CXX_COMPILER ${TARGET}-g++-posix CACHE PATH "")
set(CMAKE_RC_COMPILER  ${TARGET}-windres   CACHE PATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Win32 threads + Winsock + Win32 Bluetooth; BlueZ and ncurses are off.
set(WFD_WIN32 ON CACHE BOOL "" FORCE)
set(WFD_HAVE_BLUEZ OFF CACHE BOOL "" FORCE)
set(WFD_HAVE_CURSES OFF CACHE BOOL "" FORCE)
