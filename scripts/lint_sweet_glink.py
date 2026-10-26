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

# 3) boot-arg knob variables used outside the block that declares them.
#    This is the exact shape of the v63 build failure: `s1_fsrpoll` was parsed
#    inside g_smmu_probe()'s inner block and then referenced from the data-path
#    function, which the compiler rejected after a full CI cycle.  Structure
#    fields, shadowed names and plain locals are ignored on purpose - only
#    variables that are actually read by PE_parse_boot_argn() are checked.
KNOB = re.compile(r'PE_parse_boot_argn\(\s*"[^"]+"\s*,\s*&(\w+)')
TYPE = re.compile(r'^\s*(?:int|unsigned|uint32_t|uint64_t|vm_offset_t|'
                  r'addr64_t|bool|char)\b')
# strip string literals for every match below: "s1 cb%d" is not a use of s1
CODE = [re.sub(r'"[^"]*"', '""', l) for l in lines]
DECL_ANY = re.compile(r'\b(?:int|unsigned|uint32_t|uint64_t|vm_offset_t|'
                      r'addr64_t|bool|char)\s+\**\s*(\w+)')


def declares(line, name):
    return any(n == name for n in DECL_ANY.findall(line))


for m in KNOB.finditer('\n'.join(lines)):
    name = m.group(1)
    uline = '\n'.join(lines)[:m.start()].count('\n')
    dline = None
    for i in range(min(uline, len(CODE) - 1), -1, -1):
        if declares(CODE[i], name):
            dline = i + 1
            break
    if dline is None:
        continue
    # a name declared more than once is shadowing or unrelated: skip
    if sum(1 for l in CODE if declares(l, name)) > 1:
        continue
    depth = 0
    declared_depth = None
    for i, l in enumerate(CODE):
        if i + 1 == dline:
            declared_depth = depth
        depth += l.count('{') - l.count('}')
        if i + 1 > dline and declared_depth is not None and depth < declared_depth:
            for k in range(i + 1, len(CODE)):
                if (re.search(r'\b%s\b' % name, CODE[k])
                        and not CODE[k].lstrip().startswith(('*', '/*', '//'))):
                    bad.append((k + 1,
                                "knob %s used after its declaring block closed "
                                "(declared line %d)" % (name, dline)))
                    break
            break

for ln, why in bad: print("LINT %d: %s" % (ln, why))
print("lint:", "OK" if not bad else "%d problem(s)" % len(bad))
sys.exit(1 if bad else 0)
