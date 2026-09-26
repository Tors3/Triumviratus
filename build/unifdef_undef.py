"""Toglie dal sorgente i blocchi condizionali di una macro, trattandola come NON definita.

Uso: python unifdef_undef.py MACRO file1 [file2 ...] [--dry]

Riconosce solo le forme semplici, che sono quelle usate nel progetto:
  #ifdef M / #ifndef M / #if defined(M) / #if !defined(M)  (con #else, senza #elif)
I condizionali su altre macro restano intatti, anche annidati. Se trova la macro in una forma
che non sa risolvere (#elif, espressioni composte) si ferma e non scrive nulla.
Serve per la pulizia 7.1 (25/09/2026): le biforcazioni chiuse spariscono, l'albero resta identico
(si verifica poi con bench e node_identity.py).
"""
import re
import sys

DIRECTIVE = re.compile(r'^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$')


def classify(kind, rest, macro):
    """True/False = condizione nota con MACRO non definita; None = non riguarda la macro."""
    rest = rest.split('//')[0].strip()
    if kind == 'ifdef' and rest == macro:
        return False
    if kind == 'ifndef' and rest == macro:
        return True
    if kind == 'if':
        if re.fullmatch(r'defined\s*\(\s*%s\s*\)' % macro, rest) or re.fullmatch(r'defined\s+%s' % macro, rest):
            return False
        if re.fullmatch(r'!\s*defined\s*\(\s*%s\s*\)' % macro, rest):
            return True
    if re.search(r'\b%s\b' % macro, rest):
        raise SystemExit(f"forma non gestita: #{kind} {rest}")
    return None


def process(text, macro):
    out = []
    # ogni frame: (ours, keep_current_branch). ours=False => condizionale di altri, si copia.
    stack = []

    def emitting():
        return all(k for ours, k in stack if ours)

    for line in text.splitlines(keepends=True):
        m = DIRECTIVE.match(line)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind in ('ifdef', 'ifndef', 'if'):
                val = classify(kind, rest, macro)
                if val is None:
                    stack.append((False, True))
                    if emitting():
                        out.append(line)
                else:
                    stack.append((True, val))
                continue
            if kind == 'elif':
                if stack and stack[-1][0]:
                    raise SystemExit("#elif dentro un blocco della macro: non gestito")
                classify('if', rest, macro)
                if emitting():
                    out.append(line)
                continue
            if kind == 'else':
                ours, keep = stack[-1]
                if ours:
                    stack[-1] = (True, not keep)
                    continue
                if emitting():
                    out.append(line)
                continue
            if kind == 'endif':
                ours, _ = stack.pop()
                if not ours and emitting():
                    out.append(line)
                continue
        if emitting():
            out.append(line)
    if stack:
        raise SystemExit("condizionali non bilanciati")
    return ''.join(out)


def main():
    args = [a for a in sys.argv[1:] if a != '--dry']
    dry = '--dry' in sys.argv
    macro, files = args[0], args[1:]
    for f in files:
        with open(f, encoding='utf-8', errors='surrogateescape', newline='') as fh:
            src = fh.read()
        new = process(src, macro)
        if new != src:
            removed = src.count('\n') - new.count('\n')
            print(f"{f}: -{removed} righe")
            if not dry:
                with open(f, 'w', encoding='utf-8', errors='surrogateescape', newline='') as fh:
                    fh.write(new)


if __name__ == '__main__':
    main()
