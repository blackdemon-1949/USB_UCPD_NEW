#!/usr/bin/env python3
"""Build and run the host-side (x86) unit tests for the firmware's pure C
modules.  Nothing here is flashed; the point is to exercise protocol logic
against known-good vectors on the development machine."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
APPLI = os.path.join(ROOT, 'USB_UCPD', 'Appli')
INC = [os.path.join(APPLI, 'Core', 'Inc'),
       # app_epr.c uses the real ST notification enum values, so the
       # middleware headers must be visible to the host build too.
       os.path.join(APPLI, 'USBPD', 'Middlewares', 'ST',
                    'STM32_USBPD_Library', 'Core', 'inc')]
# usbpd_def.h lives under the Appli copy of the middleware; fall back to the
# shared Middlewares tree when building from a checkout that has only one.
_shared = os.path.join(ROOT, 'USB_UCPD', 'Middlewares', 'ST',
                       'STM32_USBPD_Library', 'Core', 'inc')
if os.path.isdir(_shared):
    INC.append(_shared)
# Minimal cmsis_compiler.h shim, see hostinc/cmsis_compiler.h.  Listed last
# so a real CMSIS tree always wins.
INC.append(os.path.join(HERE, 'hostinc'))

SUITES = [
    ('app_dec', ['test_app_dec.c',
                 os.path.join(APPLI, 'Core', 'Src', 'app_dec.c')]),
    ('app_cap', ['test_app_cap.c',
                 os.path.join(APPLI, 'Core', 'Src', 'app_cap.c')]),
    ('engines', ['test_engines.c'] + [os.path.join(APPLI, 'Core', 'Src', f)
                  for f in ('app_cable.c', 'app_txn.c', 'app_pwr.c',
                            'app_dec.c', 'app_cap.c')]),
    ('vdm', ['test_vdm.c', 'log_stub.c'] +
     [os.path.join(APPLI, 'Core', 'Src', f)
      for f in ('app_vdm.c', 'app_vdm_target.c')]),
    ('fuzz', ['test_fuzz.c', 'log_stub.c', 'target_stub.c'] +
     [os.path.join(APPLI, 'Core', 'Src', f)
      for f in ('app_fuzz.c', 'app_dec.c', 'app_ext.c', 'app_txn.c',
                'app_cap.c')]),
    ('vectors', ['test_vectors_main.c', 'log_stub.c', 'target_stub.c', 'pe_stub.c'] + [os.path.join(APPLI, 'Core', 'Src', f)
                 for f in ('app_test.c', 'app_pps.c', 'app_epr.c', 'app_cable.c',
                           'app_txn.c', 'app_dec.c', 'app_cap.c')]),
    ('pps', ['test_pps.c', 'log_stub.c', 'pe_stub.c'] + [os.path.join(APPLI, 'Core', 'Src', f)
             for f in ('app_pps.c', 'app_epr.c', 'app_cable.c',
                       'app_txn.c', 'app_dec.c', 'app_cap.c')]),
]

# The same core defines the Appli target build uses, so the ST headers select
# identical struct layouts and feature guards on host and target.
DEFS = ['-DUSBPDCORE_LIB_PD3_FULL', '-DUSBPD_PORT_COUNT=1']

CFLAGS = ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Wshadow',
          '-Wconversion', '-O2', '-g', '-fsanitize=address,undefined',
          '-fno-omit-frame-pointer']


def main():
    failed = 0
    for name, srcs in SUITES:
        exe = os.path.join('/tmp', 'hosttest_' + name)
        cmd = ['gcc'] + CFLAGS + DEFS + ['-I' + i for i in INC] + \
              [os.path.join(HERE, s) if not os.path.isabs(s) else s for s in srcs] + \
              ['-o', exe]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print('BUILD FAILED %s\n%s' % (name, r.stderr))
            failed += 1
            continue
        if r.stderr.strip():
            print('build warnings for %s:\n%s' % (name, r.stderr))
        r = subprocess.run([exe], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.stderr.strip():
            print(r.stderr, end='')
        if r.returncode != 0:
            print('SUITE FAILED: %s (exit %d)' % (name, r.returncode))
            failed += 1
    print('=' * 60)
    print('HOST TESTS: %s' % ('PASS' if failed == 0 else 'FAIL'))
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
