#!/usr/bin/env bash
# Host test gate for the PDEngine UCPD port driver (M2), the full-stack
# SPR bench (M3) and the ST transport source (M4).
#
# Compiles the self-written UCPD driver
# (USB_UCPD/Middlewares/PDEngine/port) together with the pdsink core it
# drives and runs:
#
#   - test_pdport_driver: the M2 driver unit suite (simulated transport)
#   - test_pdport_stack:  the M3 full-stack bench (pdsink Task+TC+PRL+PE+
#                         DPM over the driver, with a scripted source
#                         partner negotiating real SPR contracts)
#   - pd_tr_st.c syntax check: M4 transport parsed with the host gcc
#     against the real STM32 headers and project defines (an ARM
#     toolchain is not required for this; register accesses are plain
#     volatile struct reads and the file must stay warning-free).
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

# ---- M4: syntax check of the ST transport against the real headers --------
# Host gcc parses the ARM register map fine (it is plain volatile structs);
# only the project's own code is checked for warnings this way.
APP="$REPO_ROOT/USB_UCPD/Appli"
TR_SRC="$PORT/src/pd_tr_st.c"
ST_INCS=(-I "$APP/Core/Inc" -I "$APP/USBPD/App" -I "$APP/USBPD/Target" \
  -I "$REPO_ROOT/USB_UCPD/Drivers/STM32H7RSxx_HAL_Driver/Inc" \
  -I "$REPO_ROOT/USB_UCPD/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy" \
  -I "$REPO_ROOT/USB_UCPD/Middlewares/ST/STM32_USBPD_Library/Core/inc" \
  -I "$REPO_ROOT/USB_UCPD/Middlewares/ST/STM32_USBPD_Library/Devices/STM32H7RSXX/inc" \
  -I "$PORT_INC" \
  -I "$REPO_ROOT/USB_UCPD/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include" \
  -I "$REPO_ROOT/USB_UCPD/Drivers/CMSIS/Include" \
  -I "$REPO_ROOT/USB_UCPD/Utilities/TRACER_EMB")
ST_DEFS=(-DUSE_HAL_DRIVER -DSTM32H7R3xx -DUSE_FULL_LL_DRIVER \
  -DUSBPD_PORT_COUNT=1 -D_SNK -D_TRACE -DUSBPDCORE_LIB_PD3_FULL \
  -DUSE_FULL_LL_DRIVERS)
if gcc -fsyntax-only -std=gnu11 -Wall -Wextra "${ST_DEFS[@]}" \
    "${ST_INCS[@]}" "$TR_SRC" 2>"$OUT/pd_tr_st_syntax.txt"; then
  # CMSIS emits pointer-cast warnings when parsed on x86; only warnings
  # from the transport file itself are failures.
  if grep -q "pd_tr_st\.c:[0-9]*:[0-9]*: warning" "$OUT/pd_tr_st_syntax.txt"; then
    echo "  FAIL pd_tr_st.c syntax (warnings in transport source)"
    grep "pd_tr_st\.c:[0-9]*:[0-9]*: warning" "$OUT/pd_tr_st_syntax.txt" | head -20
    FAIL=$((FAIL+1))
  else
    echo "  PASS pd_tr_st.c syntax (project headers, no file warnings)"
    PASS=$((PASS+1))
  fi
else
  echo "  FAIL pd_tr_st.c syntax"
  grep "pd_tr_st\.c:[0-9]*:[0-9]*: error\|error:" "$OUT/pd_tr_st_syntax.txt" | head -20
  FAIL=$((FAIL+1))
fi

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
