#!/usr/bin/env python3
"""Matches the functions of two builds from ExportFeatures.java output and carries names over.

Usage: match_functions.py named.jsonl stripped.jsonl out.tsv [--check] [--cross]

--cross: the builds come from different compilers (e.g. iOS -> the Vita's SNC): instruction
similarity counts for little; agreeing neighbours, call / block counts, constants, strings and
imports carry the score, and anchors don't need alike code.

--check: withdraws every other name / string anchor (near-certain pairs) before the rest runs, then
reports how many come back with the same partner: an estimate of the other steps' accuracy.

Anchors are features unique on both sides (an existing shared name, the set of strings referenced,
the instruction sequence, the import calls, the large constants); then matches spread through the
call graph: the callee lists of a matched pair are aligned, and their unmatched entries paired when
their code agrees. out.tsv: stripped entry, named entry, name, mangled name, how, score (0..1).
"""
import difflib
import json
import sys
from collections import Counter, defaultdict


def norm_import(c):
    """EXT:___memset_chk -> EXT:memset: the two SDKs differ in fortified and underscored names."""
    n = c[4:].lstrip('_')
    if n.endswith('_chk'):
        n = n[:-4]
    return 'EXT:' + n


CONDS = ('eq', 'ne', 'cs', 'cc', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al')
BASES = {'mov', 'mvn', 'ldr', 'str', 'ldrb', 'strb', 'ldrh', 'strh', 'ldrsb', 'ldrsh', 'ldrd', 'strd', 'add', 'sub',
         'rsb', 'adc', 'sbc', 'and', 'orr', 'eor', 'bic', 'cmp', 'cmn', 'tst', 'teq', 'mul', 'mla', 'b', 'bl', 'bx',
         'blx', 'push', 'pop', 'ldm', 'stm', 'lsl', 'lsr', 'asr', 'ror', 'vldr', 'vstr', 'vmov', 'vadd', 'vsub',
         'vmul', 'vdiv', 'vcmp', 'vcmpe', 'vcvt', 'vneg', 'vabs', 'vmrs', 'smull', 'umull', 'clz', 'uxtb', 'uxth',
         'sxtb', 'sxth', 'ubfx', 'sbfx', 'bfi', 'movw', 'movt'}
ALIAS = {'cpy': 'mov', 'stmdb': 'push', 'stmfd': 'push', 'ldmia': 'pop', 'ldmfd': 'pop', 'cbz': 'b', 'cbnz': 'b',
         'blx': 'bl', 'ldm': 'pop', 'stm': 'push', 'stmia': 'push'}


def norm_mnemonic(m):
    """One spelling for ARM and Thumb-2: no condition, flag or width suffix (the builds mix the two)."""
    m = m.split('.')[0]
    if m.startswith('it') and len(m) <= 5 and set(m[2:]) <= {'t', 'e'}:
        return None
    if m[0] == 'b' and m[1:] in CONDS:  # bls is b + ls, not bl + s
        return 'b'
    for _ in range(2):
        if m in BASES or m in ALIAS:
            break
        if m.endswith('s') and m[:-1] in BASES:
            m = m[:-1]
        elif m[-2:] in CONDS and (m[:-2] in BASES or m[:-2] in ALIAS or m[:-2].rstrip('s') in BASES):
            m = m[:-2]
    m = ALIAS.get(m, m)
    return m


def load(path):
    funcs = {}
    for line in open(path, encoding='utf-8'):
        f = json.loads(line)
        if f['external'] or f['thunk']:
            continue
        f['mnl'] = [n for n in map(norm_mnemonic, f['mn'].split()) if n]
        f['ninstr'] = len(f['mnl'])
        f['calls'] = [norm_import(c) if c.startswith('EXT:') else c[5:] if c.startswith('ADDR:') else c for c in f['calls']]
        funcs[f['entry']] = f
    # a call to a small wrapper of one import (_hiMalloc -> malloc) counts as that import: one build
    # has them inlined
    wrapper = {e: f['calls'][0] for e, f in funcs.items()
               if f['ninstr'] <= 8 and len(f['calls']) == 1 and f['calls'][0].startswith('EXT:')}
    for f in funcs.values():
        f['ext'] = [wrapper.get(c, c) for c in f['calls'] if c.startswith('EXT:') or c in wrapper]
    for f in funcs.values():
        f['callees'] = [c for c in f['calls'] if c in funcs]
        f['callers'] = []
    for e, f in funcs.items():
        for c in set(f['callees']):
            funcs[c]['callers'].append(e)
    return funcs


def generic(name):
    return name.startswith(('FUN_', 'thunk_FUN_', 'LAB_', 'entry'))


A = load(sys.argv[1])  # named (1.0.1)
B = load(sys.argv[2])  # stripped (2.2.0)
a2b, b2a, how = {}, {}, {}


def similarity(a, b):
    """0..1: how alike the two functions' instruction sequences are."""
    x, y = A[a]['mnl'], B[b]['mnl']
    if not x or not y:
        return 0.0
    if min(len(x), len(y)) / max(len(x), len(y)) < 0.5:
        return 0.0
    if len(x) * len(y) > 4_000_000:  # too long to align: compare the mnemonic counts
        cx, cy = Counter(x), Counter(y)
        return sum((cx & cy).values()) / max(len(x), len(y))
    return difflib.SequenceMatcher(None, x, y, autojunk=False).ratio()


def accept(a, b, why):
    if a in a2b or b in b2a:
        return False
    a2b[a], b2a[b], how[b] = b, a, why
    return True


# ---- anchors: a key unique on both sides
def unique_keys(funcs, keyf):
    seen = defaultdict(list)
    for e, f in funcs.items():
        k = keyf(f)
        if k is not None:
            seen[k].append(e)
    return {k: v[0] for k, v in seen.items() if len(v) == 1}


def size_ratio(a, b):
    return min(A[a]['ninstr'], B[b]['ninstr']) / max(A[a]['ninstr'], B[b]['ninstr'], 1)


def anchor(keyf, why, min_sim=0.0):
    ka, kb = unique_keys(A, keyf), unique_keys(B, keyf)
    n = 0
    for k, a in ka.items():
        b = kb.get(k)
        if not b or a in a2b or b in b2a:
            continue
        if '--cross' in sys.argv:
            ok = size_ratio(a, b) >= 0.4
        else:
            ok = min_sim == 0 or similarity(a, b) >= min_sim
        if ok:
            n += accept(a, b, why)
    return n


def shared_names(f):
    return None if generic(f['names'][0]) else f['names'][0]


ext = lambda f: tuple(f['ext'])
report = []
report.append(('name', anchor(shared_names, 'name')))
check = '--check' in sys.argv
cross = '--cross' in sys.argv
held = {}
if check:
    for i, (b, a) in enumerate(sorted(b2a.items())):
        if i % 2 == 0 and how[b] in ('name', 'strings'):
            held[b] = a
            del a2b[a], b2a[b], how[b]
report.append(('instructions', anchor(lambda f: f['mn'] if f['ninstr'] >= 12 else None, 'instructions')))
report.append(('strings', anchor(lambda f: tuple(sorted(set(f['strings']))) or None, 'strings', 0.3)))
report.append(('imports', anchor((lambda f: ext(f) if len(ext(f)) >= 3 else None) if cross else
                                 (lambda f: (ext(f), f['blocks']) if len(ext(f)) >= 2 else None), 'imports', 0.4)))
report.append(('constants', anchor(lambda f: tuple(sorted(set(f['consts']))) if len(set(f['consts'])) >= 2 else None,
                                   'constants', 0.4)))
report.append(('instr+calls', anchor(lambda f: (f['mn'], len(f['callees'])) if f['ninstr'] >= 6 else None,
                                     'instructions', 0.0)))


# ---- scoring: instructions, constants, strings, imports, and the matched neighbours
def jaccard(x, y):
    if not x and not y:
        return None
    cx, cy = Counter(x), Counter(y)
    return sum((cx & cy).values()) / sum((cx | cy).values())


sim_cache = {}


def sim(a, b):
    k = (a, b)
    if k not in sim_cache:
        sim_cache[k] = similarity(a, b)
    return sim_cache[k]


def neighbours(a, b):
    """(agreeing, total) matched callers and callees of a and b."""
    agree = total = 0
    for rel in ('callees', 'callers'):
        na = {x for x in A[a][rel] if x in a2b}
        nb = {y for y in B[b][rel] if y in b2a}
        total += len(na | {b2a[y] for y in nb})
        agree += len(na & {b2a[y] for y in nb})
    return agree, total


def score(a, b):
    fa, fb = A[a], B[b]
    ra = min(fa['ninstr'], fb['ninstr']) / max(fa['ninstr'], fb['ninstr'], 1)
    if ra < (0.4 if cross else 0.5):
        return 0.0
    parts = [(sim(a, b), 0.5 if cross else 2.0)]
    if cross:  # the shape: how many calls and blocks
        na, nb = len(fa['calls']), len(fb['calls'])
        parts.append((min(na, nb) / max(na, nb) if max(na, nb) else 1.0, 1.0))
    for x, y, w in ((fa['consts'], fb['consts'], 1.5), (fa['strings'], fb['strings'], 2.0),
                    (fa['ext'], fb['ext'], 1.0)):
        j = jaccard(x, y)
        if j is not None:
            parts.append((j, w))
    ag, tot = neighbours(a, b)
    if tot:
        parts.append((ag / tot, (3.0 if cross else 2.0) if tot >= 2 else (1.5 if cross else 1.0)))
    parts.append((min(fa['blocks'], fb['blocks']) / max(fa['blocks'], fb['blocks'], 1), 1.0 if cross else 0.5))
    return sum(v * w for v, w in parts) / sum(w for _, w in parts)


def pick(proposals, thr, margin):
    """proposals: {b: {a: score}} -> mutual best pairs, clear of the runner-up on both sides."""
    best_for_a = defaultdict(list)
    for b, cs in proposals.items():
        for a, s in cs.items():
            best_for_a[a].append((s, b))
    n = 0
    for b, cs in proposals.items():
        if not cs or b in b2a:
            continue
        ranked = sorted(((s, a) for a, s in cs.items()), reverse=True)
        s, a = ranked[0]
        # a lower score will do for a bigger function far ahead of the runner-up
        big = min(A[a]['ninstr'], B[b]['ninstr']) >= 40 and s >= 0.35
        m = 0.2 if big and s < thr else margin
        if a in a2b or (s < thr and not big) or (len(ranked) > 1 and ranked[1][0] > s - m):
            continue
        ra = sorted(best_for_a[a], reverse=True)
        if ra[0][1] != b or (len(ra) > 1 and ra[1][0] > s - m):
            continue
        n += accept(a, b, 'callgraph' if neighbours(a, b)[0] else 'similar')
    return n


def neighbour_round(thr, margin):
    proposals = {}
    for b in B:
        if b in b2a:
            continue
        cands = set()
        for y in B[b]['callers']:
            if y in b2a:
                cands.update(x for x in A[b2a[y]]['callees'] if x not in a2b)
        for y in B[b]['callees']:
            if y in b2a:
                cands.update(x for x in A[b2a[y]]['callers'] if x not in a2b)
        if cands:
            proposals[b] = {a: score(a, b) for a in cands}
    return pick(proposals, thr, margin)


def hist(funcs, e):
    f = funcs[e]
    if 'hist' not in f:
        f['hist'] = Counter(f['mnl'])
    return f['hist']


def global_round(thr, margin, min_instr):
    by_len = defaultdict(list)
    for a in A:
        if a not in a2b:
            by_len[A[a]['ninstr'].bit_length()].append(a)
    proposals = {}
    for b in B:
        if b in b2a or B[b]['ninstr'] < min_instr:
            continue
        k = B[b]['ninstr'].bit_length()
        cands = [a for kk in (k - 1, k, k + 1) for a in by_len.get(kk, [])]
        if cross:  # alike in size and sharing a constant, string or import
            fb = B[b]
            kb = set(fb['consts']) | set(fb['strings']) | set(fb['ext'])
            cands = [a for a in cands if min(A[a]['blocks'], fb['blocks']) / max(A[a]['blocks'], fb['blocks'], 1) >= 0.5
                     and kb & (set(A[a]['consts']) | set(A[a]['strings']) | set(A[a]['ext']))]
        else:
            hb = hist(B, b)
            cands = [a for a in cands if sum((hist(A, a) & hb).values()) / max(A[a]['ninstr'], B[b]['ninstr']) >= 0.6]
        proposals[b] = {a: s for a in cands if (s := score(a, b)) >= 0.3}
    return pick(proposals, thr, margin)


def vote_round(min_votes=3):
    """Pairs lined up in the callee lists of several matched callers, whatever their code looks like
    (a function rewritten between the builds, e.g. __effgo becoming a dispatcher)."""
    votes = Counter()
    for a, b in a2b.items():
        ca = [c for c in A[a]['calls'] if c in A or c.startswith('EXT:')]
        cb = [c for c in B[b]['calls'] if c in B or c.startswith('EXT:')]
        la = [('M', c) if c in a2b else c if c.startswith('EXT:') else '?' for c in ca]
        lb = [('M', b2a[c]) if c in b2a else c if c.startswith('EXT:') else '?' for c in cb]
        seen = set()
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, la, lb, autojunk=False).get_opcodes():
            if op in ('equal', 'replace') and i2 - i1 == j2 - j1:
                for x, y in zip(ca[i1:i2], cb[j1:j2]):
                    if x in A and y in B and x not in a2b and y not in b2a and (x, y) not in seen:
                        seen.add((x, y))
                        votes[(x, y)] += 1  # one vote per matched caller
    best_a, best_b = defaultdict(list), defaultdict(list)
    for (x, y), v in votes.items():
        best_a[x].append(v)
        best_b[y].append(v)
    n = 0
    for (x, y), v in votes.most_common():
        if v < min_votes or x in a2b or y in b2a:
            continue
        ra, rb = sorted(best_a[x], reverse=True), sorted(best_b[y], reverse=True)
        if ra[0] != v or rb[0] != v or (len(ra) > 1 and ra[1] * 2 > v) or (len(rb) > 1 and rb[1] * 2 > v):
            continue
        n += accept(x, y, 'call position')
    return n


rounds = 0
steps = ((0.7, 0.1), (0.6, 0.1), (0.55, 0.08), (0.5, 0.08)) + (((0.45, 0.08), (0.4, 0.08)) if cross else ())
for thr, margin in steps:
    while True:
        rounds += 1
        n = neighbour_round(thr, margin)
        if not n:
            n = global_round(max(thr, 0.7), 0.1, 16)
        if not n:
            n = vote_round()
        if not n:
            break
report.append(('callgraph', sum(1 for h in how.values() if h == 'callgraph')))
report.append(('call position', sum(1 for h in how.values() if h == 'call position')))
report.append(('similar code', sum(1 for h in how.values() if h == 'similar')))

# ---- check: how many of a matched function's calls into matched functions agree
agree = total = 0
for b, a in b2a.items():
    for y in set(B[b]['callees']):
        if y in b2a:
            total += 1
            agree += b2a[y] in A[a]['callees']

with open(sys.argv[3], 'w', encoding='utf-8') as out:
    out.write('entry_220\tentry_101\tname\tmangled\thow\tscore\n')
    for b in sorted(b2a, key=lambda e: int(e, 16)):
        a = b2a[b]
        if generic(A[a]['names'][0]):  # matched (it helped the neighbours) but nameless there too
            continue
        mangled = next((n for n in A[a]['names'] if n.startswith('__Z')), '')
        out.write(f"{b}\t{a}\t{A[a]['names'][0]}\t{mangled}\t{how[b]}\t{score(a, b):.2f}\n")

print(f'1.0.1: {len(A)} functions; 2.2.0: {len(B)} functions')
for k, v in report:
    print(f'  {k:14s} {v}')
print(f'matched {len(b2a)} ({100 * len(b2a) / len(B):.1f}% of 2.2.0); propagation rounds {rounds}')
print(f'call edges between matched functions that agree: {agree}/{total} ({100 * agree / max(total, 1):.1f}%)')
if check:
    right = sum(b2a.get(b) == a for b, a in held.items())
    wrong = [(b, a) for b, a in held.items() if b in b2a and b2a[b] != a]
    print(f'check: {len(held)} withdrawn anchors: {right} matched again correctly, {len(wrong)} wrongly, '
          f'{len(held) - right - len(wrong)} not matched')
    for b, a in wrong:
        print(f'  {b}: {A[a]["names"][0]} matched to {A[b2a[b]]["names"][0]} ({how[b]})')
