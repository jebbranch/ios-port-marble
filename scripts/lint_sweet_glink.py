import re, sys
src = sys.argv[1] if len(sys.argv)>1 else 'sweet_glink.c'
lines = open(src).read().split('\n')
bad = []

# 1) a bare "static <type>" line must be followed by a function name, not by
#    another declaration (this is how a variable got wedged between the return
#    type and the name and broke the build)
bare = re.compile(r'^static\s+[A-Za-z_][A-Za-z0-9_ \*]*$')
for i,l in enumerate(lines):
    if bare.match(l):
        j = i+1
        while j < len(lines) and (lines[j].strip()=='' or lines[j].lstrip().startswith(('/*','*','//'))):
            j += 1
        ok = j < len(lines) and (re.match(r'^[A-Za-z_]\w*\s*\(', lines[j])
                                 or 'SYSCTL_HANDLER_ARGS' in lines[j])
        if not ok:
            bad.append((i+1, "bare type line followed by %r" % lines[j][:50]))

# 2) file-scope static variables used before they are declared
decl = {}
for i,l in enumerate(lines):
    m = re.match(r'static\s+[A-Za-z_][A-Za-z0-9_ \*]*?\b(g_\w+)\s*(\[[^\]]*\])?\s*(=[^;]*)?;', l)
    if m: decl.setdefault(m.group(1), i+1)
for name, dline in decl.items():
    for i,l in enumerate(lines):
        if re.search(r'\b%s\b' % name, l):
            if i+1 < dline and not l.lstrip().startswith(('*','/*','//')):
                bad.append((i+1, "use of %s before its declaration at line %d" % (name, dline)))
            break

# 3) locals used outside the block they are declared in: cheap check that any
#    identifier declared inside a "{ ... }" is not referenced after the block
#    closes. Only applied to the identifiers we added, to keep it simple.
for local in ('iova_mode','s1empty','mapa','l3t','va'):
    m = [i+1 for i,l in enumerate(lines) if re.search(r'\b(int|uint64_t|uint64_t \*|unsigned)\b[^;]*\b%s\b[^;]*;' % local, l)]
    if len(m) != 1: continue      # several locals share the name: skip
    dline = m[0]
    depth = 0
    declared_depth = None
    for i,l in enumerate(lines):
        if i+1 == dline:
            declared_depth = depth
        depth += l.count('{') - l.count('}')
        if i+1 > dline and declared_depth is not None and depth < declared_depth:
            # block closed at this line; any later use is out of scope
            for k in range(i+1, len(lines)):
                if re.search(r'\b%s\b' % local, lines[k]) and not lines[k].lstrip().startswith(('*','/*','//')):
                    bad.append((k+1, "%s used after its block closed (declared line %d)" % (local, dline)))
                    break
            break

for ln, why in bad: print("LINT %d: %s" % (ln, why))
print("lint:", "OK" if not bad else "%d problem(s)" % len(bad))
sys.exit(1 if bad else 0)
