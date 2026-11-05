#!/usr/bin/env python3
"""Runs a built gpugen suite in Xenia or on a console and compares runs.

    python3 tools/gpugen/run.py xenia OUTDIR
    python3 tools/gpugen/run.py console OUTDIR
    python3 tools/gpugen/run.py diff A.json B.json

Xenia and console runs work like xamgen's (tools/xamgen/run.py, whose helpers
this uses): the executor leaves gpugen.bin beside the XEX or on the console's
Hdd:\\oracle, and both decode to OUTDIR/<where>_results.json, a raw suite's
results to OUTDIR/<where>_<test>.bin. A console run needs a build with the
command processor microcode.
"""

import argparse
import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gpugen  # noqa: E402

# xamgen's run.py, by path since this file shares its name.
_spec = importlib.util.spec_from_file_location(
    'xamgen_run', os.path.join(os.path.dirname(HERE), 'xamgen', 'run.py'))
xamgen_run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(xamgen_run)

XENIA = xamgen_run.XENIA


def decode_into(outdir, where, results_path):
    with open(os.path.join(outdir, 'gpugen.json')) as f:
        meta = json.load(f)
    with open(results_path, 'rb') as f:
        decoded = gpugen.decode(meta, f.read())
    gpugen.save_raw(decoded, outdir, where)
    out = os.path.join(outdir, f'{where}_results.json')
    with open(out, 'w') as f:
        json.dump(decoded, f, indent=1)
    if 'failed' in decoded:
        stopped = f', {decoded["failed"]}'
    elif decoded['segments_done'] == 0:
        stopped = ', stopped in the setup'
    elif decoded['not_run']:
        stopped = f', stopped in {decoded["not_run"][0]!r}'
    else:
        stopped = ''
    print(f'{where}: {decoded["segments_done"]} of {decoded["segments"]} '
          f'segments, {len(decoded["tests"])} tests{stopped} -> {out}')
    return decoded


def run_xenia(outdir, timeout, extra_args):
    outdir = os.path.abspath(outdir)
    if not os.path.exists(XENIA):
        sys.exit(f'xenia: no {XENIA}, set XENIA_EXE to the build to run')
    xex = os.path.join(outdir, 'gpugen.xex')
    log = os.path.join(outdir, 'xenia.log')
    results = os.path.join(outdir, 'gpugen.bin')
    for path in (results, log):
        if os.path.exists(path):
            os.remove(path)
    from_wsl = xamgen_run.from_wsl(XENIA)
    before = xamgen_run.windows_pids(XENIA) if from_wsl else set()
    started = time.time()
    process = subprocess.Popen(
        [XENIA, *xamgen_run.XENIA_ARGS, *extra_args,
         f'--log_file={xamgen_run.host_path(log, XENIA)}',
         xamgen_run.host_path(xex, XENIA)],
        cwd=os.path.dirname(XENIA), stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    deadline = started + timeout
    summary = None
    try:
        while time.time() < deadline and process.poll() is None:
            time.sleep(0.5)
            if not os.path.exists(log):
                continue
            with open(log, errors='replace') as f:
                for line in f:
                    if '(DbgPrint) gpugen:' in line:
                        summary = line.strip()
            if summary and os.path.exists(results):
                time.sleep(0.5)
                break
    finally:
        exit_code = process.poll()
        if from_wsl:
            for pid in xamgen_run.windows_pids(XENIA) - before:
                subprocess.run(['taskkill.exe', '/PID', str(pid), '/F'],
                               capture_output=True)
        elif exit_code is None:
            process.kill()
        process.wait(timeout=10)
    if not summary:
        folder, dumps = xamgen_run.crash_dumps(started)
        if dumps:
            sys.exit(f'xenia crashed, dump {dumps[0]} in {folder}')
        if exit_code is not None:
            sys.exit(f'xenia exited with {exit_code} before the summary, '
                     f'see {log}')
        sys.exit(f'xenia: no summary within {timeout} s, see {log}')
    written = re.search(r'results file ([0-9A-Fa-f]{8})', summary)
    if not written or int(written.group(1), 16):
        sys.exit(f'xenia: no results from this run, summary {summary!r}')
    print(f'xenia: {summary}')
    copy = os.path.join(outdir, 'xenia_results.bin')
    os.replace(results, copy)
    return decode_into(outdir, 'xenia', copy)


def run_console(outdir, timeout):
    outdir = os.path.abspath(outdir)
    with open(os.path.join(outdir, 'gpugen.json')) as f:
        meta = json.load(f)
    if not meta.get('console_ready'):
        sys.exit('console: this build has no command processor microcode')
    rgh = xamgen_run.rgh
    rgh_exe = xamgen_run.rgh_exe()
    xex = f'{xamgen_run.CONSOLE_DIR}\\gpugen.xex'
    results = f'{xamgen_run.CONSOLE_DIR}\\gpugen.bin'
    code, out = rgh('title')
    if 'Aurora' not in out:
        sys.exit('console: not at Aurora, refusing to launch')
    code, out = rgh('fs', 'put', '--path', xex, '--in',
                    xamgen_run.host_path(os.path.join(outdir, 'gpugen.xex'),
                                         rgh_exe), timeout=180)
    if code:
        sys.exit(f'console: upload failed\n{out}')
    rgh('fs', 'rm', '--path', results)
    code, out = rgh('launch', '--xex', xex)
    if code:
        sys.exit(f'console: launch failed\n{out}')
    watch_log = os.path.join(outdir, 'console_watch.jsonl')
    if os.path.exists(watch_log):
        os.remove(watch_log)
    watcher = subprocess.Popen(
        [rgh_exe, 'debug', 'watch', '--duration', str(timeout), '--raw',
         '--out', xamgen_run.host_path(watch_log, rgh_exe)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    fault = summary = None
    try:
        while time.time() < deadline and watcher.poll() is None:
            time.sleep(0.5)
            events = xamgen_run.watch_events(watch_log)
            for event in events:
                raw = event.get('Raw', '')
                if event.get('Type') == 'debugstr' and 'gpugen:' in raw:
                    summary = raw
                if event.get('Type') == 'exception':
                    fault = raw
            if fault or any(e.get('Type') == 'modunload' and
                            'gpugen.xex' in e.get('Raw', '') for e in events):
                break
    finally:
        watcher.terminate()
        watcher.wait()
    if fault or not summary:
        code, out = rgh('mem', 'peek', '--addr',
                        f'{meta["control"] + gpugen.C_PROGRESS:#x}',
                        '--type', 'u32')
        stages = ', '.join(f'{i} {name}'
                           for i, name in enumerate(gpugen.PROGRESS, 1))
        if fault:
            rgh('launch', '--xex', xamgen_run.AURORA)
        sys.exit(f'console: {fault or "no summary"}\nprogress: {out.strip()}'
                 f'\n({stages})')
    written = re.search(r'results file ([0-9A-Fa-f]{8})', summary)
    if not written or int(written.group(1), 16):
        sys.exit(f'console: no results from this run, summary {summary!r}')
    print(f'console: {summary}')
    for _ in range(30):
        code, out = rgh('title')
        if 'Aurora' in out:
            break
        time.sleep(1)
    copy = os.path.join(outdir, 'console_results.bin')
    code, out = rgh('fs', 'get', '--path', results, '--out',
                    xamgen_run.host_path(copy, rgh_exe))
    if code:
        sys.exit(f'console: no results file\n{out}')
    return decode_into(outdir, 'console', copy)


def load_words(path):
    """{test: hex words} from a results JSON, reading a raw suite's .bin
    files beside it."""
    with open(path) as f:
        tests = json.load(f)['tests']
    for name, words in tests.items():
        if isinstance(words, str):
            with open(os.path.join(os.path.dirname(path), words), 'rb') as f:
                data = f.read()
            tests[name] = [f'{w:08X}' for w in
                           struct.unpack(f'>{len(data) // 4}I', data)]
    return tests


def diff(a_path, b_path):
    a, b = load_words(a_path), load_words(b_path)
    same = differ = 0
    for name, want in a.items():
        got = b.get(name)
        if got is None:
            print(f'{name}: not run in {b_path}')
            continue
        bad = [i for i, (x, y) in enumerate(zip(want, got)) if x != y]
        if bad:
            differ += 1
            print(f'{name}: {len(bad)} of {len(want)} words differ, first '
                  f'{[(i, want[i], got[i]) for i in bad[:4]]}')
        else:
            same += 1
            print(f'{name}: all {len(want)} words same')
    print(f'{same} tests same, {differ} different')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('xenia')
    p.add_argument('outdir')
    p.add_argument('--timeout', type=int, default=30)
    p.add_argument('--xenia-arg', action='append', default=[],
                   help='extra Xenia argument, e.g. --xenia-arg=--gpu=vulkan')
    p = sub.add_parser('console')
    p.add_argument('outdir')
    p.add_argument('--timeout', type=int, default=30)
    p = sub.add_parser('diff')
    p.add_argument('a')
    p.add_argument('b')
    args = parser.parse_args()
    if args.command == 'xenia':
        run_xenia(args.outdir, args.timeout, args.xenia_arg)
    elif args.command == 'console':
        run_console(args.outdir, args.timeout)
    else:
        diff(args.a, args.b)


if __name__ == '__main__':
    main()
