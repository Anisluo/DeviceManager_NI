#!/usr/bin/env bash
# Configure + build + test on Windows (Git Bash) with Qt's MinGW toolchain.
#   QT_ROOT     Qt kit, default D:/Qt/6.8.3/mingw_64
#   MINGW_ROOT  matching MinGW, default D:/Qt/Tools/mingw1310_64
#   PYTHON_ROOT Python used for embedding (stdlib + site-packages at run time)
#   PYBIND11_DIR pybind11 CMake config dir
#   DEPLOY=1    also run windeployqt so build/bin runs without Qt on PATH
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_ROOT="${QT_ROOT:-D:/Qt/6.8.3/mingw_64}"
MINGW_ROOT="${MINGW_ROOT:-D:/Qt/Tools/mingw1310_64}"
PYTHON_ROOT="${PYTHON_ROOT:-C:/Users/Admin/AppData/Local/Programs/Python/Python313}"
PYBIND11_DIR="${PYBIND11_DIR:-D:/Qt/_deps/pybind11/pybind11/share/cmake/pybind11}"
BUILD="${BUILD:-$ROOT/build}"
export PATH="$(cygpath -u "$MINGW_ROOT")/bin:$(cygpath -u "$QT_ROOT")/bin:$PATH"

if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$MINGW_ROOT/bin/gcc.exe" \
        -DCMAKE_CXX_COMPILER="$MINGW_ROOT/bin/g++.exe" \
        -DCMAKE_PREFIX_PATH="$QT_ROOT"         -DPython_ROOT_DIR="$PYTHON_ROOT" -Dpybind11_DIR="$PYBIND11_DIR"
fi
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure

if [ "${DEPLOY:-0}" = "1" ]; then
    windeployqt --release --qmldir "$ROOT/DeviceManager/qml" --no-translations \
        --no-opengl-sw --no-system-d3d-compiler "$BUILD/bin/DeviceManager.exe" > /dev/null
fi
