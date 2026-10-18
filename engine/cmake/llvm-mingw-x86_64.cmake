# Windows (x64) from Linux with llvm-mingw (https://github.com/mstorsjo/llvm-mingw):
#   cmake -S engine -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=engine/cmake/llvm-mingw-x86_64.cmake
# LLVM_MINGW: the toolchain's folder (default /opt/llvm-mingw). The result runs under Wine.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW)
    set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
if(NOT LLVM_MINGW)
    set(LLVM_MINGW /opt/llvm-mingw)
endif()
set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# (the host's pkg-config would find Linux libraries)
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
# one .exe: the C++ runtime and winpthreads linked in
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
