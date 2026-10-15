#!/usr/bin/env python3
"""Merges name tables from match_functions.py made from different source builds for one target.

Usage: combine_names.py out.tsv label1=table1.tsv label2=table2.tsv ...

Each target function takes the name its sources agree on; with only one source, that one's; when
they disagree, the higher-scored one, marked as disputed (ApplyNames calls it low confidence).
A mangled name is kept from any source naming it the same. out.tsv has match_functions.py's
columns plus "source" (which builds and their entries), as ApplyNames.java reads it.
"""
import csv
import sys

out_path = sys.argv[1]
tables = []
for arg in sys.argv[2:]:
    label, _, path = arg.partition('=')
    tables.append((label, {r['entry_220']: r for r in csv.DictReader(open(path, encoding='utf-8'), delimiter='\t')}))

entries = sorted({e for _, t in tables for e in t}, key=lambda e: int(e, 16))
counts = {'agreed': 0, 'single': 0, 'disputed': 0}
with open(out_path, 'w', encoding='utf-8') as out:
    out.write('entry_220\tentry_101\tname\tmangled\thow\tscore\tsource\n')
    for e in entries:
        have = [(label, t[e]) for label, t in tables if e in t]
        names = {r['name'] for _, r in have}
        best_label, best = max(have, key=lambda lr: float(lr[1]['score']))
        if len(have) > 1 and len(names) == 1:
            how, kind = 'agreeing builds', 'agreed'
            score = max(float(r['score']) for _, r in have)
        elif len(have) == 1:
            how, kind = best['how'], 'single'
            score = float(best['score'])
        else:
            how, kind = 'disputed: ' + ', '.join(f"{l} {r['name']}" for l, r in have), 'disputed'
            score = float(best['score'])
        counts[kind] += 1
        mangled = next((r['mangled'] for _, r in have if r['name'] == best['name'] and r['mangled']), '')
        source = ' and '.join(f"{l} @ 0x{r['entry_101'].lstrip('0')}" for l, r in have if r['name'] == best['name'])
        out.write(f"{e}\t{best['entry_101']}\t{best['name']}\t{mangled}\t{how}\t{score:.2f}\t{source}\n")
print(f"{len(entries)} functions: {counts['agreed']} named alike by several builds, {counts['single']} by one, "
      f"{counts['disputed']} disputed (the higher score taken) -> {out_path}")
