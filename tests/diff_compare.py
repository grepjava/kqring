#!/usr/bin/env python3
"""Compare a Linux run of tests/kqring_diff with a kqring run.

    python3 tests/diff_compare.py linux.out kqring.out

Lines are compared case by case. A case that differs must be listed in
tests/diff_expected.txt, with its reason; a listed case that no longer
differs is reported too, so the list can't go stale. Both outputs must run
to their end. Exit status 0 means only expected differences.
"""
import collections
import difflib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(path):
    cases = collections.OrderedDict()
    head, ended = [], False
    with open(path, encoding='utf8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line == '# end':
                ended = True
            elif line.startswith('#'):
                head.append(line)
            elif line:
                cases.setdefault(line.split('/')[0], []).append(line)
    if not ended:
        sys.exit('%s: the run did not finish' % path)
    return head, cases


def expected():
    out = {}
    with open(os.path.join(HERE, 'diff_expected.txt'), encoding='utf8') as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                name, why = line.split(None, 1)
                out[name] = why
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    lhead, linux = load(sys.argv[1])
    khead, kqring = load(sys.argv[2])
    want = expected()
    print('\n'.join(['Linux:'] + lhead + ['kqring:'] + khead))
    bad = 0
    for name in sorted(set(linux) | set(kqring)):
        a, b = linux.get(name, []), kqring.get(name, [])
        if a == b:
            if name in want:
                print('STALE    %s: listed, but no longer differs' % name)
                bad += 1
            else:
                print('same     %s' % name)
            continue
        if name in want:
            print('expected %s: %s' % (name, want[name]))
        else:
            print('UNEXPECTED %s' % name)
            bad += 1
        for d in difflib.unified_diff(a, b, 'linux', 'kqring', lineterm='',
                                      n=0):
            if not d.startswith(('---', '+++', '@@')):
                print('    ' + d)
    print('%d cases, %d problems' % (len(set(linux) | set(kqring)), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
