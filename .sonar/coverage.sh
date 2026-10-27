#!/bin/sh
# Coverage for SonarQube. sonarqube/scan.sh builds .sonar/Dockerfile and runs this in it, with the repository at
# /usr/src and one job; the report is .sonar-reports/cobertura.xml, its paths relative to the repository root.
#   1. tests/ (a stand-alone CMake project) built natively against Debian's Qt 6 with the daemon's and the QML
#      plugin's sources compiled with --coverage;
#   2. the Qt Test programs run by ctest inside one dbus-run-session (the Logger and the data loaders use D-Bus;
#      the Logger test points the "system" bus at the same private bus), QT_QPA_PLATFORM=offscreen;
#      each program also writes JUnit XML to .sonar-reports/xunit/ (SonarQube's test counts);
#   3. gcovr: lines and branches of common.*, daemon/ and qmlplugin/ as Cobertura (tests/ is analysed as tests).
# Exit status (bits): 1 the build or a test failed, 2 no coverage report.
# Without Docker (Qt 6 with QtSensors, cmake, gcovr and dbus-run-session installed): sh .sonar/coverage.sh
set -u
cd "$(dirname "$0")/.." || exit 2

reports=.sonar-reports
build=$reports/build-tests
jobs=${JOBS:-1}
status=0

rm -rf "$build" "$reports/xunit"
mkdir -p "$reports/xunit"
export QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
# dbus-daemon needs a passwd entry for the user it runs as (the image's /etc/passwd is writable for this)
if ! id -un > /dev/null 2>&1 && [ -w /etc/passwd ]; then
    echo "scan:x:$(id -u):$(id -g):scan:${HOME:-/tmp}:/bin/sh" >> /etc/passwd
fi

echo "== build"
if cmake -S tests -B "$build" -DCMAKE_BUILD_TYPE=Debug -DSENSORLOGD_COVERAGE=ON \
        -DSENSORLOGD_JUNIT_DIR="$PWD/$reports/xunit" > "$reports/cmake.log" 2>&1 \
    && cmake --build "$build" -j "$jobs"; then
    echo "== tests"
    dbus-run-session -- ctest --test-dir "$build" --output-on-failure -j 1 || status=$((status | 1))
else
    cat "$reports/cmake.log"
    echo "build FAILED"
    status=$((status | 1))
fi

echo "== coverage"
gcovr --root . --object-directory "$build" \
    --filter '^common\.(cpp|h)$' --filter '^daemon/' --filter '^qmlplugin/' \
    --exclude-unreachable-branches --exclude-throw-branches \
    --cobertura "$reports/cobertura.xml" --cobertura-pretty --print-summary "$build" || status=$((status | 2))
[ -s "$reports/cobertura.xml" ] || status=$((status | 2))

exit "$status"
