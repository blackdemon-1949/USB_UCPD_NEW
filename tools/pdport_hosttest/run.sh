#!/usr/bin/env bash
# Host test gate for the PDEngine UCPD port driver (M2).
#
# Compiles the self-written pdsink UCPD driver
# (USB_UCPD/Middlewares/PDEngine/port) together with the pdsink core
# objects it depends on and runs the driver test suite against a
# simulated UCPD transport.
#
# GoogleTest is fetched once into the per-user cache directory when
# missing (same bootstrap as tools/pdengine_hosttest/run.sh).
#
# Usage:  tools/pdport_hosttest/run.sh
# Exit:   0 = all suites passed.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="$REPO_ROOT/USB_UCPD/Middlewares/PDEngine"
PORT="$ENGINE/port"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/pdport_hosttest"
GT_VER="v1.14.0"
ETL_INC="$ENGINE/etl/include"
SRC_INC="$ENGINE/pdsink/src"
PD_INC="$ENGINE/pdsink/include"
PORT_INC="$PORT/include/pdport"
OUT="$CACHE/build"
SUITE="$REPO_ROOT/tools/pdport_hosttest/src/test_pdport_driver.cpp"
SIM_DIR="$REPO_ROOT/tools/pdport_hosttest/src"
PASS=0
FAIL=0

echo "PDEngine UCPD port host test gate"
echo "  port  : $PORT"

# ---- GoogleTest bootstrap -------------------------------------------------
if [ ! -f "$CACHE/gtest/libgtest.a" ]; then
  echo "  fetching googletest $GT_VER into $CACHE ..."
  mkdir -p "$CACHE"
  rm -rf "$CACHE/gt-src" "$CACHE/gtest"
  git clone -q --depth 1 --branch "$GT_VER" \
    https://github.com/google/googletest.git "$CACHE/gt-src" || exit 2
  mkdir -p "$CACHE/gtest"
  (cd "$CACHE/gt-src/googletest" && \
     g++ -std=c++17 -c src/gtest-all.cc -I . -I include -o "$CACHE/gtest/gtest-all.o" && \
     g++ -std=c++17 -c src/gtest_main.cc -I . -I include -o "$CACHE/gtest/gtest_main.o" && \
     ar rcs "$CACHE/gtest/libgtest.a" "$CACHE/gtest/gtest-all.o" "$CACHE/gtest/gtest_main.o") || exit 2
  (cd "$CACHE/gt-src/googlemock" && \
     g++ -std=c++17 -c src/gmock-all.cc -I . -I include -I ../googletest/include \
         -o "$CACHE/gtest/gmock-all.o" && \
     ar rcs "$CACHE/gtest/libgmock.a" "$CACHE/gtest/gmock-all.o") || exit 2
fi

# ---- Build ----------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT"
OBJS=()
for f in \
  "$ENGINE/pdsink/src/pd/port.cpp" \
  "$PORT/src/pd_ucpd_driver.cpp" \
  "$SIM_DIR/pd_tr_sim.cpp"; do
  o="$OUT/$(basename "${f%.cpp}").o"
  g++ -std=gnu++17 -Wall -Wextra \
      -I "$ETL_INC" -I "$SRC_INC" -I "$PD_INC" \
      -I "$ENGINE/pdsink/src/pd" -I "$PORT_INC" \
      -c "$f" -o "$o" || exit 3
  OBJS+=("$o")
done

bin="$OUT/test_pdport_driver"
g++ -std=gnu++17 -I "$ETL_INC" -I "$SRC_INC" -I "$PD_INC" \
    -I "$ENGINE/pdsink/src/pd" -I "$PORT_INC" \
    -I "$CACHE/gt-src/googletest/include" \
    -I "$CACHE/gt-src/googlemock/include" \
    "$SUITE" "${OBJS[@]}" \
    "$CACHE/gtest/libgmock.a" "$CACHE/gtest/libgtest.a" -o "$bin" || exit 4

if "$bin" >"$OUT/out.txt" 2>&1; then
  echo "  PASS test_pdport_driver: $(grep -E '^\[  PASSED  \]' "$OUT/out.txt")"
  PASS=$((PASS+1))
else
  echo "  FAIL test_pdport_driver"
  tail -40 "$OUT/out.txt"
  FAIL=$((FAIL+1))
fi

echo "== $PASS suites passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
