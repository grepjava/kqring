#!/usr/bin/env python3
"""Check COMPATIBILITY.MD against the source.

    python3 tests/compat_check.py [--baseline HEADER] [--liburing TESTDIR]
                                  [--results CHECKLOG]

Fails when a capability in <sys/kqring.h> has no row, when a row's status
disagrees with what the module does, or when a row names a function or test
that doesn't exist (a test is a test_ function of tests/kqring_test.c,
liburing/NAME, diff/NAME for a case of tests/kqring_diff.c, or fuzz). With --baseline (Linux's io_uring.h) every opcode, flag
and registration in it must have a row; with --liburing (liburing's test/
directory) the liburing tests named must exist. With --results (the
check.log of `make check` in liburing/) a test named must have run, and one
that failed may only be named on a row that isn't Equivalent.
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATUSES = ('Equivalent', 'Partial', 'Different', 'Unsupported')
# Every symbol of these kinds needs a row.
REQUIRED = re.compile(
    r'^(IORING_OP_|IORING_SETUP_|IORING_ENTER_|IOSQE_(?!.*_BIT$)|'
    r'IORING_REGISTER_|IORING_UNREGISTER_|IORING_FEAT_|IORING_TIMEOUT_|'
    r'SOCKET_URING_OP_)')
NOT_CAPABILITIES = {'IORING_OP_LAST', 'IORING_REGISTER_LAST',
                    'IORING_REGISTER_FILES_SKIP', 'IORING_TIMEOUT_CLOCK_MASK',
                    'IORING_TIMEOUT_UPDATE_MASK'}


def read(path):
    with open(path, encoding='utf8') as f:
        return f.read()


def symbols(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    found = set(re.findall(r'\b([A-Z][A-Z0-9_]+)\b', text))
    return {s for s in found if REQUIRED.match(s)} - NOT_CAPABILITIES


def rows(text):
    """Rows of the tables with a Source column, by section."""
    section, cols = None, None
    for line in text.split('\n'):
        if line.startswith('## '):
            section, cols = line[3:], None
        if not line.startswith('|'):
            cols = None
            continue
        cells = [c.strip() for c in re.split(r'(?<!\\)\|', line)[1:-1]]
        if cols is None:
            cols = cells
        elif 'Source' in cols and not set(cells[0]) <= set('-'):
            yield section, dict(zip(cols, cells))


def mask(text, name):
    m = re.search(r'#define\s+' + name + r'\s*\\?\n(.*?)\n\n', text, re.S)
    return set(re.findall(r'IORING_[A-Z0-9_]+', m.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--baseline')
    ap.add_argument('--liburing')
    ap.add_argument('--results')
    args = ap.parse_args()

    doc = read(os.path.join(ROOT, 'COMPATIBILITY.MD'))
    hdr = read(os.path.join(ROOT, 'include/sys/kqring.h'))
    var = read(os.path.join(ROOT, 'kmod/kqring_var.h'))
    dev = read(os.path.join(ROOT, 'kmod/kqring_dev.c'))
    reg = read(os.path.join(ROOT, 'kmod/kqring_register.c'))
    kmod = '\n'.join(read(f) for f in glob.glob(os.path.join(ROOT, 'kmod/*.[ch]')))
    tests = read(os.path.join(ROOT, 'tests/kqring_test.c'))

    funcs = set(re.findall(r'^(\w+)\(', kmod, re.M))
    funcs |= set(re.findall(r'^#define\s+(\w+)\(', kmod, re.M))
    funcs |= set(re.findall(r'struct fileops (\w+) =', kmod))
    tfuncs = set(re.findall(r'^(test_\w+)\(', tests, re.M))
    dcases = set(re.findall(r'^\t\{ "(\w+)", c_\w+ \},$',
                            read(os.path.join(ROOT, 'tests', 'kqring_diff.c')),
                            re.M))
    probe = re.search(r'supported_ops\[\] = \{(.*?)\};', reg, re.S).group(1)
    probed = set(re.findall(r'IORING_OP_\w+', probe))
    setup = mask(var, 'KQRING_SETUP_FLAGS')
    enter = mask(var, 'KQRING_ENTER_FLAGS')
    feats = mask(var, 'KQRING_FEATURES')

    results = {}
    if args.results:
        for line in read(args.results).split('\n'):
            if len(line.split()) >= 2:
                results[line.split()[1]] = line.split()[0]

    errors = []
    table = {}
    gaps = 0
    for section, r in rows(doc):
        name = r['Capability'] if 'Capability' in r else r['Boundary']
        sym = name.strip('`')
        if 'Capability' in r:
            if sym in table:
                errors.append('%s: two rows' % sym)
            table[sym] = r['Status']
        if r['Status'] not in STATUSES:
            errors.append('%s: bad status %r' % (name, r['Status']))
        if r['Status'] != 'Equivalent' and not r['Contract']:
            errors.append('%s: %s with no contract' % (name, r['Status']))
        for f in re.findall(r'`(\w+)`', r['Source']):
            if f not in funcs:
                errors.append('%s: no function %s' % (name, f))
        if not r['Source']:
            errors.append('%s: no source' % name)
        cells = re.findall(r'`([\w/.-]+)`', r['Tests'])
        if not cells:
            errors.append('%s: no test and no gap' % name)
        for t in cells:
            if t == 'gap':
                gaps += 1
            elif t.startswith('test_'):
                if t not in tfuncs:
                    errors.append('%s: no test %s' % (name, t))
            elif t.startswith('liburing/'):
                if args.liburing and not os.path.exists(
                        os.path.join(args.liburing, t[9:] + '.c')):
                    errors.append('%s: no liburing test %s' % (name, t))
                res = results.get(t[9:], 'PASS' if not results else 'none')
                if res in ('SKIP', 'none'):
                    errors.append('%s: %s did not run' % (name, t))
                elif res != 'PASS' and r['Status'] == 'Equivalent':
                    errors.append('%s: %s fails' % (name, t))
            elif t.startswith('diff/'):
                if t[5:] not in dcases:
                    errors.append('%s: no differential case %s' % (name, t))
            elif t != 'fuzz':
                errors.append('%s: unknown test %s' % (name, t))

    for s in sorted(symbols(hdr) - set(table)):
        errors.append('%s: in kqring.h, no row' % s)
    if args.baseline:
        for s in sorted(symbols(read(args.baseline)) - set(table)):
            errors.append('%s: in the baseline, no row' % s)

    supported = lambda s: table.get(s, 'Unsupported') != 'Unsupported'
    for s, st in table.items():
        if s.startswith('IORING_OP_') and supported(s) != (s in probed):
            errors.append('%s: %s, but probe says %s' %
                          (s, st, 'yes' if s in probed else 'no'))
        if s.startswith('IORING_SETUP_') and supported(s) != (s in setup):
            errors.append('%s: %s, setup mask disagrees' % (s, st))
        if s.startswith('IORING_ENTER_') and supported(s) != (s in enter):
            errors.append('%s: %s, enter mask disagrees' % (s, st))
        if s.startswith('IORING_FEAT_') and supported(s) != (s in feats):
            errors.append('%s: %s, features disagree' % (s, st))

    if gaps and '## Validation gaps' not in doc:
        errors.append('gaps with no Validation gaps section')
    for e in errors:
        print(e)
    counts = {s: sum(v == s for v in table.values()) for s in STATUSES}
    print('%d capabilities: %s; %d gaps; %d errors' % (
        len(table), ', '.join('%d %s' % (counts[s], s) for s in STATUSES),
        gaps, len(errors)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
