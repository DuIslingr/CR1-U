#!/usr/bin/env python3
"""The port's own interface text (src/platform/ui_text.hpp): every tr("..."), trf("...", ...) and N_("...")
string in src/ listed in lang/template.pot, and each lang/<code>.po brought up to date with it - its
translations kept, new strings added untranslated, strings no longer in the code dropped.

    tools/ui_strings.py            update lang/template.pot and lang/*.po
    tools/ui_strings.py --check    exit 1 if they are out of date (nothing written)
    tools/ui_strings.py --new de "Deutsch"   start lang/de.po

The catalogs are gettext's: Poedit, Weblate, a text editor. A translation keeps the {0}, {1} ... of its
English, in any order. Leave a string untranslated (msgstr "") to show the English.
"""
import os
import re
import sys

ENGINE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ENGINE, "src")
LANG = os.path.join(ENGINE, "lang")

CALL = re.compile(r'\b(tr|trf|N_)\s*\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)')
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "\0"}


def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = s[i + 1]
        if n in "uU":
            k = 4 if n == "u" else 8
            out.append(chr(int(s[i + 2:i + 2 + k], 16)))
            i += 2 + k
        elif n == "x":
            m = re.match(r"[0-9a-fA-F]+", s[i + 2:])
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        else:
            out.append(ESCAPES.get(n, n))
            i += 2
    return "".join(out)


def po_quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    if "\n" not in s.rstrip("\n") and len(s) < 70:
        return '"' + s.replace("\n", "\\n") + '"'
    parts = s.split("\n")
    lines = ['""']
    for i, p in enumerate(parts):
        last = i == len(parts) - 1
        if last and not p:
            break
        lines.append('"' + p + ("" if last else "\\n") + '"')
    return "\n".join(lines)


def strip_comments(text):
    # comments out, strings and line numbers kept
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text[i] == '"':
            m = re.compile(r'"(?:[^"\\\n]|\\.)*"').match(text, i)
            j = m.end() if m else i + 1
            out.append(text[i:j])
            i = j
        elif text[i] == "'":
            m = re.compile(r"'(?:[^'\\\n]|\\.)*'").match(text, i)
            j = m.end() if m else i + 1
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def collect():
    found = {}  # msgid -> [file:line]
    for root, dirs, files in os.walk(SRC):
        dirs.sort()  # (the same order on every machine: the references and the strings' order follow it)
        for f in sorted(files):
            if not f.endswith((".cpp", ".hpp", ".mm", ".h")) or f.startswith("ui_text."):
                continue
            path = os.path.join(root, f)
            with open(path, encoding="utf-8") as fh:
                text = strip_comments(fh.read())
            for m in CALL.finditer(text):
                msgid = "".join(c_unescape(x) for x in LITERAL.findall(m.group(2)))
                if not msgid.strip():
                    continue
                # (the file alone, as xgettext --add-location=file: an edit that moves lines leaves the catalogs as they are)
                ref = os.path.relpath(path, ENGINE).replace(os.sep, "/")
                refs = found.setdefault(msgid, [])
                if ref not in refs:
                    refs.append(ref)
    return found


def parse_po(text):
    """[(comments, msgid, msgstr, fuzzy)] and the header (msgstr of msgid "")"""
    entries, header = [], ""
    cur = {"c": [], "id": None, "str": None, "fuzzy": False, "f": None}

    def flush():
        nonlocal header
        if cur["id"] is not None:
            if cur["id"] == "":
                header = cur["str"] or ""
            else:
                entries.append((cur["c"], cur["id"], cur["str"] or "", cur["fuzzy"]))
        cur.update({"c": [], "id": None, "str": None, "fuzzy": False, "f": None})

    for raw in text.split("\n"):
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith("#~"):
            continue  # obsolete
        if line.startswith("#"):
            if cur["f"] == "str":
                flush()
            if line.startswith("#,") and "fuzzy" in line:
                cur["fuzzy"] = True
            elif line.startswith("# ") or line == "#" or line.startswith("#."):
                cur["c"].append(line)
            continue
        m = re.match(r'(msgid|msgstr)\s+"(.*)"$', line)
        if m:
            if m.group(1) == "msgid":
                if cur["f"] == "str":
                    flush()
                cur["id"], cur["f"] = c_unescape(m.group(2)), "id"
            else:
                cur["str"], cur["f"] = c_unescape(m.group(2)), "str"
            continue
        m = re.match(r'"(.*)"$', line)
        if m and cur["f"]:
            key = "id" if cur["f"] == "id" else "str"
            cur[key] = (cur[key] or "") + c_unescape(m.group(1))
    flush()
    return entries, header


def render(header, items):
    """items: [(comments, refs, msgid, msgstr, fuzzy)]"""
    out = ['msgid ""', "msgstr " + po_quote(header), ""]
    for comments, refs, msgid, msgstr, fuzzy in items:
        out += comments
        for i in range(0, len(refs), 4):
            out.append("#: " + " ".join(refs[i:i + 4]))
        if fuzzy:
            out.append("#, fuzzy")
        out.append("msgid " + po_quote(msgid))
        out.append("msgstr " + po_quote(msgstr))
        out.append("")
    return "\n".join(out)


TEMPLATE_HEADER = ("Project-Id-Version: the port's interface text\n"
                   "MIME-Version: 1.0\n"
                   "Content-Type: text/plain; charset=UTF-8\n"
                   "Content-Transfer-Encoding: 8bit\n"
                   "Language: \n"
                   "X-Language-Name: \n")


def main():
    args = sys.argv[1:]
    check = "--check" in args
    found = collect()
    order = sorted(found, key=lambda k: found[k][0])  # in the order of their first place in the code
    wanted = {}
    wanted["template.pot"] = render(TEMPLATE_HEADER, [([], found[k], k, "", False) for k in order])
    if "--new" in args:
        i = args.index("--new")
        code, name = args[i + 1], args[i + 2]
        path = os.path.join(LANG, code + ".po")
        if os.path.exists(path):
            sys.exit("%s exists" % path)
        header = TEMPLATE_HEADER.replace("Language: \n", "Language: %s\n" % code).replace("X-Language-Name: \n", "X-Language-Name: %s\n" % name)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render(header, [([], found[k], k, "", False) for k in order]))
    for f in sorted(os.listdir(LANG)) if os.path.isdir(LANG) else []:
        if not f.endswith(".po"):
            continue
        with open(os.path.join(LANG, f), encoding="utf-8") as fh:
            entries, header = parse_po(fh.read())
        have = {e[1]: e for e in entries}
        items = []
        for k in order:
            c, _, s, fz = have.get(k, ([], k, "", False))
            items.append((c, found[k], k, s, fz))
        wanted[f] = render(header, items)
    stale = []
    for f, text in wanted.items():
        path = os.path.join(LANG, f)
        old = open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else None
        if old is not None:
            old = old.replace("\r\n", "\n")  # (a Windows checkout's line ends)
        if old != text:
            stale.append(f)
            if not check:
                os.makedirs(LANG, exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text)
    for f in sorted(wanted):
        if f.endswith(".po"):
            entries, _ = parse_po(wanted[f])
            done = sum(1 for e in entries if e[2] and not e[3])
            print("%-12s %d of %d translated" % (f, done, len(entries)))
    if check and stale:
        print("out of date: " + ", ".join(stale) + " (run tools/ui_strings.py)")
        sys.exit(1)
    print("%d strings%s" % (len(order), "" if check or not stale else "; written: " + ", ".join(stale)))


if __name__ == "__main__":
    main()
