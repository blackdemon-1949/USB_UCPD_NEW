#!/usr/bin/env bash
# Host test gate for the PDEngine UCPD port driver (M2) and the
# full-stack SPR bench (M3).
#
# Compiles the self-written UCPD driver
# (USB_UCPD/Middlewares/PDEngine/port) together with the pdsink core it
# drives and runs:
#
#   - test_pdport_driver: the M2 driver unit suite (simulated transport)
#   - test_pdport_stack:  the M3 full-stack bench (pdsink Task+TC+PRL+PE+
#                         DPM over the driver, with a scripted source
#                         partner negotiating real SPR contracts)
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
SUITE_DIR="$REPO_ROOT/tools/pdport_hosttest/src"
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
INCS=(-I "$ETL_INC" -I "$SRC_INC" -I "$PD_INC" -I "$ENGINE/pdsink/src/pd" -I "$PORT_INC")

# pdsink protocol core (shared by both suites)
CORE_OBJS=()
for f in "$ENGINE/pdsink/src/pd"/*.cpp "$ENGINE/pdsink/src/pd/utils"/*.cpp; do
  o="$OUT/core_$(basename "${f%.cpp}").o"
  g++ -std=gnu++17 -Wall -Wextra "${INCS[@]}" -c "$f" -o "$o" || exit 3
  CORE_OBJS+=("$o")
done

# M2 port objects (driver + simulated transport)
PORT_OBJS=()
for f in "$PORT/src/pd_ucpd_driver.cpp" "$SUITE_DIR/pd_tr_sim.cpp"; do
  o="$OUT/port_$(basename "${f%.cpp}").o"
  g++ -std=gnu++17 -Wall -Wextra "${INCS[@]}" -c "$f" -o "$o" || exit 3
  PORT_OBJS+=("$o")
done

GT_INCS=(-I "$CACHE/gt-src/googletest/include" -I "$CACHE/gt-src/googlemock/include")
GT_LIBS=("$CACHE/gtest/libgmock.a" "$CACHE/gtest/libgtest.a")

run_suite() {
  local name="$1" src="$2"
  local bin="$OUT/$name"
  g++ -std=gnu++17 "${INCS[@]}" "${GT_INCS[@]}" "$src" \
      "${CORE_OBJS[@]}" "${PORT_OBJS[@]}" "${GT_LIBS[@]}" -o "$bin" || exit 4
  if "$bin" >"$OUT/$name.txt" 2>&1; then
    echo "  PASS $name: $(grep -E '^\[  PASSED  \]' "$OUT/$name.txt")"
    PASS=$((PASS+1))
  else
    echo "  FAIL $name"
    tail -60 "$OUT/$name.txt"
    FAIL=$((FAIL+1))
  fi
}

run_suite test_pdport_driver "$SUITE_DIR/test_pdport_driver.cpp"
run_suite test_pdport_stack  "$SUITE_DIR/test_pdport_stack.cpp"

echo "== $PASS suites passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
