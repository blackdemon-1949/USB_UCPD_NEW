#!/usr/bin/env python3
"""
build.py - command-line build of the STM32CubeIDE projects in this repository.

It reproduces the STM32CubeIDE managed build: the source list, include paths
and preprocessor symbols are read from `<project>/.project` (linked resources)
and `<project>/.cproject` (the managed-build option blocks), so a source file
that CubeIDE would compile is compiled here too, and a file CubeIDE would not
compile is not.

Usage:
    tools/build.py [--project Boot|Appli] [--config Debug|Release] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
# Workspace root holding USB_UCPD/{Boot,Appli}; override to build another tree.
ROOT = REPO
EXTRA_DEFINES = []

# STM32H7R3Z8Jx : Cortex-M7, single precision D16 FPU, hard float ABI
MCUFLAGS = ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard']

CFG_CFLAGS = {
    # Debug: the .cproject stores no optimisation value for the Debug
    # configuration, so the STM32CubeIDE toolchain default (-Og) applies.
    'Debug':   ['-Og', '-g3', '-Wall', '-fdata-sections', '-ffunction-sections'],
    'Release': ['-Os', '-g0', '-Wall', '-fdata-sections', '-ffunction-sections'],
}


def linked_sources(proj_dir):
    """External files the CubeIDE project links in (Drivers / Middlewares / ...)."""
    tree = ET.parse(os.path.join(proj_dir, '.project'))
    out = []
    for link in tree.getroot().iter('link'):
        name = link.findtext('name')
        loc = link.findtext('locationURI') or link.findtext('location')
        if loc is None or not name.lower().endswith(('.c', '.s')):
            continue
        m = re.match(r'PARENT-(\d+)-PROJECT_LOC/(.*)', loc)
        assert m, loc
        depth = int(m.group(1))
        base = proj_dir
        for _ in range(depth):
            base = os.path.dirname(base)
        out.append(os.path.normpath(os.path.join(base, m.group(2))))
    return out


def local_sources(proj_dir, folders):
    out = []
    for folder in folders:
        d = os.path.join(proj_dir, folder)
        if not os.path.isdir(d):
            continue
        for root, _, files in os.walk(d):
            for f in sorted(files):
                if f.lower().endswith(('.c', '.s')):
                    out.append(os.path.normpath(os.path.join(root, f)))
    return sorted(set(out))


def cproject_options(proj_dir, config):
    """Return (defines, includes, source_folders, libs, ldscript) for a config."""
    tree = ET.parse(os.path.join(proj_dir, '.cproject'))
    root = tree.getroot()
    cfg = None
    for c in root.iter('configuration'):
        if c.get('name') == config:
            cfg = c
            break
    assert cfg is not None, 'configuration %s not found in %s' % (config, proj_dir)

    defines, includes, folders, libs, ldscript = [], [], [], [], None

    # The "Defaults" option is a |-separated digest CubeIDE writes for the
    # whole managed build; use it as the authoritative include/source/lib list.
    for opt in cfg.iter('option'):
        if (opt.get('superClass') or '').endswith('.managedbuild.option.defaults'):
            parts = opt.get('value').split(' || ')
            includes = [p for p in parts[10].split('|') if p]
            defines = [p for p in parts[13].split('|') if p]
            folders = [p for p in parts[15].split('|') if p]
            libs = [p for p in parts[17].split('|') if p]
            break

    # ...but honour explicit per-configuration overrides.
    for opt in cfg.iter('option'):
        sc = opt.get('superClass') or ''
        vals = [v.get('value') for v in opt.iter('listOptionValue')]
        if sc.endswith('c.compiler.option.definedsymbols') and vals:
            defines = vals
        elif sc.endswith('c.compiler.option.includepaths') and vals:
            includes = vals
        elif sc.endswith('c.linker.option.script') and opt.get('value'):
            ldscript = re.sub(r'^\$\{workspace_loc:/\$\{ProjName\}/(.*)\}$', r'\1',
                              opt.get('value'))
    return defines, includes, folders, libs, ldscript


def resolve(base, path):
    p = path.replace('${workspace_loc:/${ProjName}}', base)
    return os.path.normpath(os.path.join(base, p))


def build(proj, config, jobs):
    proj_dir = os.path.join(ROOT, 'USB_UCPD', proj)
    defines, includes, folders, libs, ldscript = cproject_options(proj_dir, config)
    assert ldscript, 'no linker script selected for %s/%s' % (proj, config)

    outdir = os.path.join(proj_dir, config)
    os.makedirs(outdir, exist_ok=True)

    sources = local_sources(proj_dir, [f.strip() for f in folders]) + \
              linked_sources(proj_dir)
    sources = sorted(set(os.path.abspath(s) for s in sources))

    # CubeIDE writes include/lib paths relative to the *build* directory
    # (Boot/Debug, Appli/Release, ...), not to the project directory.
    inc_flags = ['-I' + resolve(outdir, i) for i in includes]
    def_flags = ['-D' + d for d in defines] + ['-D' + d for d in EXTRA_DEFINES]
    cflags = MCUFLAGS + CFG_CFLAGS[config] + def_flags + inc_flags

    print('=' * 78)
    print('%s / %s   (%d sources)' % (proj, config, len(sources)))
    print('  defines : %s' % ' '.join(defines))
    print('  linker  : %s' % ldscript)
    print('=' * 78)

    # Many sources are CubeIDE *linked resources* that live outside the project
    # directory (the shared USB_UCPD/Drivers, USB_UCPD/Middlewares and
    # USB_UCPD/Utilities folders are linked into both Boot and Appli).  For
    # those, relpath() starts with '..' and would write the object back into
    # the source tree, so key them by their workspace-relative path instead.
    def obj_for(src):
        if src.startswith(proj_dir + os.sep):
            rel = os.path.relpath(src, proj_dir)
        else:
            rel = os.path.join('ext', os.path.relpath(src, ROOT))
        return os.path.join(outdir, rel + '.o')

    obj_map = {s: obj_for(s) for s in sources}
    for o in obj_map.values():
        assert o.startswith(outdir + os.sep), 'object escapes the build dir: %s' % o

    tasks = []
    for s in sources:
        obj = obj_map[s]
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        dep = obj + '.d'
        if s.lower().endswith('.s'):
            cmd = ['arm-none-eabi-gcc'] + MCUFLAGS + def_flags + inc_flags + \
                  ['-c', s, '-o', obj]
        else:
            cmd = ['arm-none-eabi-gcc'] + cflags + \
                  ['-MMD', '-MP', '-MF', dep, '-c', s, '-o', obj]
        tasks.append((cmd, obj))

    errors, warns = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(subprocess.run, c, capture_output=True, text=True): r
                for c, r in tasks}
        for fut in concurrent.futures.as_completed(futs):
            res = fut.result()
            if res.returncode != 0:
                errors.append((futs[fut], res.stderr))
            elif res.stderr.strip():
                warns.append((futs[fut], res.stderr))
    for obj, w in sorted(warns):
        sys.stderr.write(w if w.startswith('/') else '%s\n%s' % (obj, w))
    for obj, err in sorted(errors):
        sys.stderr.write('FAILED %s\n%s\n' % (obj, err))
    if errors:
        return 1

    objs = [obj_map[s] for s in sources]
    elf = os.path.join(outdir, '%s_%s.elf' % (proj, config))
    ld = os.path.join(proj_dir, ldscript)
    link = ['arm-none-eabi-gcc', '-o', elf] + objs
    for l in libs:
        link.append(resolve(outdir, l))
    link += MCUFLAGS + ['-T' + ld,
                        '-Wl,-Map=%s.map,--gc-sections' % os.path.splitext(elf)[0],
                        '-static', '--specs=nano.specs', '--specs=nosys.specs',
                        '-Wl,--start-group', '-lc', '-lm', '-Wl,--end-group']
    res = subprocess.run(link, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write('LINK FAILED\n%s\n' % res.stderr)
        return 1
    if res.stderr.strip():
        print(res.stderr)

    subprocess.run(['arm-none-eabi-size', elf])
    for ext in ('hex', 'bin'):
        subprocess.run(['arm-none-eabi-objcopy', '-O', 'ihex' if ext == 'hex' else 'binary',
                        elf, os.path.splitext(elf)[0] + '.' + ext], check=True)
    print('OK  %s' % elf)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', choices=['Boot', 'Appli', 'all'], default='all')
    ap.add_argument('--config', choices=['Debug', 'Release', 'all'], default='all')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    ap.add_argument('--root', default=REPO, help='workspace root holding USB_UCPD/')
    ap.add_argument('--extra-defines', nargs='*', default=[],
                    metavar='NAME=VAL',
                    help='extra -D flags, e.g. APP_ENG_CAPTURE=0 to bisect')
    a = ap.parse_args()
    global ROOT, EXTRA_DEFINES
    ROOT = os.path.abspath(a.root)
    EXTRA_DEFINES = list(a.extra_defines)
    projs = ['Boot', 'Appli'] if a.project == 'all' else [a.project]
    cfgs = ['Debug', 'Release'] if a.config == 'all' else [a.config]
    rc = 0
    for p in projs:
        for c in cfgs:
            rc |= build(p, c, a.jobs)
    return rc


if __name__ == '__main__':
    sys.exit(main())
