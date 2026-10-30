#!/usr/bin/env bash
# Build, install and zip a self-contained Windows release:
#   release/DeviceManager-<version>-win64.zip
# Contents: bin/ (exe + Qt + MinGW + python3x.dll), devices/, sdk/python, stations/, sequences/, data/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
QT_ROOT="${QT_ROOT:-D:/Qt/6.8.3/mingw_64}"
MINGW_ROOT="${MINGW_ROOT:-D:/Qt/Tools/mingw1310_64}"
export PATH="$(cygpath -u "$MINGW_ROOT")/bin:$(cygpath -u "$QT_ROOT")/bin:$PATH"

bash "$ROOT/scripts/build.sh"
VERSION=$(py -3 -c "import re,sys; print(re.search(r'project\(\w+ VERSION ([0-9.]+)', open(sys.argv[1]).read()).group(1))" "$ROOT/CMakeLists.txt")
NAME="DeviceManager-$VERSION-win64"
STAGE="$ROOT/release/$NAME/DeviceManager"
rm -rf "$ROOT/release/$NAME"
cmake --install "$BUILD" --prefix "$STAGE" > /dev/null
windeployqt --release --qmldir "$ROOT/DeviceManager/qml" --no-translations --no-opengl-sw \
    --no-system-d3d-compiler "$STAGE/bin/DeviceManager.exe" > /dev/null
mkdir -p "$STAGE/data"
cp "$ROOT/DeviceManager/DEPLOY_8881.md" "$STAGE/"
cp "$ROOT/DeviceManager/README.md" "$STAGE/README_DeviceManager.md"
find "$STAGE" -name __pycache__ -prune -exec rm -rf {} +
rm -rf "$STAGE/devices/ni_digital/_generated"
sed -i "s#\"../../data\"#\"../data\"#" "$STAGE"/stations/*.json            # data inside the package

(cd "$ROOT/release/$NAME" && py -3 -c "import shutil; shutil.make_archive('../$NAME', 'zip', '.')")
du -sh "$ROOT/release/$NAME.zip"
echo "release/$NAME.zip"
