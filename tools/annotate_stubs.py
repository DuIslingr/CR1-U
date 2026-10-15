"""Rewrite 'dyld_stub_binder+0xADDR' references in disasm.txt with the imported symbol name."""
import re, sys
stubs_path, disasm_path = sys.argv[1], sys.argv[2]
names = {int(a, 16): n for a, n in (l.split() for l in open(stubs_path) if l.strip())}
pat = re.compile(r"<dyld_stub_binder\+0x([0-9a-f]+)>")
def sub(m):
    return f"<{names.get(int(m.group(1), 16), m.group(0)[1:-1])}>"
lines = [pat.sub(sub, l) for l in open(disasm_path)]
open(disasm_path, "w").writelines(lines)
