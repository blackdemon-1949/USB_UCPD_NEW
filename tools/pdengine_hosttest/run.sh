#!/usr/bin/env bash
# Host test gate for the PDEngine (pdsink) core.
#
# Compiles the vendored pdsink protocol core (USB_UCPD/Middlewares/PDEngine)
# with the vendored ETL headers and runs its upstream unit-test suites on the
# host, exactly as upstream CI does (platformio test --environment
# test-desktop, googletest framework).
#
# GoogleTest is not vendored into the repo; it is fetched once into the
# per-user cache directory when missing (MIT/Apache-2.0, used only by tests).
#
# Usage:  tools/pdengine_hosttest/run.sh
# Exit:   0 = all suites passed.

set -u

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="$REPO_ROOT/USB_UCPD/Middlewares/PDEngine"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/pdengine_hosttest"
GT_VER="v1.14.0"
ETL_INC="$ENGINE/etl/include"
SRC_INC="$ENGINE/pdsink/src"
PD_INC="$ENGINE/pdsink/include"
OUT="$CACHE/build"
PASS=0
FAIL=0

echo "PDEngine host test gate"
echo "  engine : $ENGINE"

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

# ---- Build the engine core ------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT"
OBJS=()
for f in "$ENGINE/pdsink/src/pd"/*.cpp "$ENGINE/pdsink/src/pd/utils"/*.cpp; do
  o="$OUT/$(basename "${f%.cpp}").o"
  g++ -std=gnu++17 -Wall -Wextra -I "$ETL_INC" -I "$SRC_INC" -I "$PD_INC" \
      -c "$f" -o "$o" || exit 3
  OBJS+=("$o")
done

# ---- Run each upstream test suite -----------------------------------------
SUITES=(afsm atomic_bits leapsync spsc_overwrite_queue timer_pack validate_source_caps)
for t in "${SUITES[@]}"; do
  src="$ENGINE/pdsink/test/test_$t/test_$t.cpp"
  bin="$OUT/test_$t"
  g++ -std=gnu++17 -I "$ETL_INC" -I "$SRC_INC" -I "$PD_INC" \
      -I "$CACHE/gt-src/googletest/include" \
      -I "$CACHE/gt-src/googlemock/include" \
      "$src" "${OBJS[@]}" \
      "$CACHE/gtest/libgmock.a" "$CACHE/gtest/libgtest.a" -o "$bin" || { FAIL=$((FAIL+1)); continue; }
  if "$bin" >"$OUT/out_$t.txt" 2>&1; then
    echo "  PASS $t: $(grep -E '^\[  PASSED  \]' "$OUT/out_$t.txt")"
    PASS=$((PASS+1))
  else
    echo "  FAIL $t"
    tail -20 "$OUT/out_$t.txt"
    FAIL=$((FAIL+1))
  fi
done

echo "== $PASS suites passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
