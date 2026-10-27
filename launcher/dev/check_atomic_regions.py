"""Static check behind the lock-free guest atomics patch: every guest region between mtmsrd r13 (REX_ENTER_GLOBAL_LOCK) and the
next mtmsrd (REX_LEAVE_GLOBAL_LOCK) in the generated code contains only loads, ALU, compares, branches and ONE kind of write: the
store-conditional (stwcx./stdcx., emitted as __sync_bool_compare_and_swap), no other store, no call, no mfmsr/mtmsrd of its own.

  python -I check_atomic_regions.py [generated dir]   (reads only the files that mention REX_ENTER_GLOBAL_LOCK; exit 1 on a violation)
"""
import glob
import os
import re
import sys

gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "generated", "default")
files = [f for f in glob.glob(os.path.join(gen, "mcla_recomp.*.cpp"))]
ALLOWED = re.compile(r"^(lwarx|ldarx|stwcx\.|stdcx\.|lwz|ld|lbz|lhz|lwzx|lbzx|lhzx|ldx|li|lis|mr|mr\.|addi|addis|add|add\.|addic|addic\.|subf|subf\.|subfic|"
                     r"neg|and|and\.|andc|andc\.|or|or\.|ori|oris|andi\.|andis\.|xor|xori|xoris|nor|extsb|extsh|extsw|clrlwi|clrlwi\.|rlwinm|rlwinm\.|"
                     r"rlwimi|rldicl|rldicr|slwi|srwi|slw|srw|sraw|srawi|cmpwi|cmplwi|cmpw|cmplw|cmpdi|cmpldi|cmpd|cmpld|b|bne|beq|blt|bgt|ble|bge|"
                     r"bne\+|beq\+|bne-|beq-|bdnz|bc|crclr|crset|nop|mfmsr|mtmsrd|sync|lwsync|eieio|isync|cntlzw|cntlzd|mulli|mullw|divwu|extrwi|blr)$")
funcs = 0
regions = 0
bad = []
mn_seen = {}
unpaired = []
for f in files:
    txt = open(f, encoding="utf-8", errors="replace").read()
    if "GLOBAL_LOCK" not in txt:
        continue
    for m in re.finditer(r"DEFINE_REX_FUNC\((\w+)\) \{\n(.*?)\n\}\n", txt, re.S):
        name, body = m.group(1), m.group(2)
        if "GLOBAL_LOCK" not in body:
            continue
        funcs += 1
        lines = body.split("\n")
        n_enter = sum("REX_ENTER_GLOBAL_LOCK" in l for l in lines)
        n_leave = sum("REX_LEAVE_GLOBAL_LOCK" in l for l in lines)
        if n_enter != n_leave:
            unpaired.append((name, n_enter, n_leave))
        # a region: from an ENTER to the LAST LEAVE before the next ENTER (a region has several exits: the compare-failed path
        # clears the reservation with its own store-conditional and leaves)
        segs = []
        for l in lines:
            if "REX_ENTER_GLOBAL_LOCK" in l:
                segs.append([])
            if segs:
                segs[-1].append(l)
        for seg in segs:
            regions += 1
            last = max(i for i, l in enumerate(seg) if "REX_LEAVE_GLOBAL_LOCK" in l) if any("REX_LEAVE_GLOBAL_LOCK" in l for l in seg) else len(seg) - 1
            if not any("REX_LEAVE_GLOBAL_LOCK" in l for l in seg):
                bad.append((name, "no LEAVE after ENTER"))
            cas = 0
            for l in seg[1:last]:
                s = l.strip()
                if s.startswith("// "):
                    mn = s[3:].split()[0] if s[3:].split() else ""
                    mn_seen[mn] = mn_seen.get(mn, 0) + 1
                    if not ALLOWED.match(mn):
                        bad.append((name, "instruction " + mn))
                    continue
                if "__sync_bool_compare_and_swap" in s:
                    cas += 1
                if re.search(r"REX_(MM_)?STORE|REX_CHECK_GLOBAL_LOCK|REX_ENTER_GLOBAL_LOCK|sub_[0-9A-F]{8}\(|__imp__|REX_CALL|call", s):
                    bad.append((name, "statement in region: " + s[:90]))
                if re.search(r"__sync_|__atomic|atomic_", s) and "atomic_thread_fence" not in s and "__sync_bool_compare_and_swap" not in s:
                    bad.append((name, "other atomic in region: " + s[:90]))
            if cas == 0:
                bad.append((name, "region without a store-conditional"))
print(f"{funcs} functions with global lock macros, {regions} regions (ENTER..LEAVE)")
print("mnemonics inside regions:", ", ".join(f"{k}:{v}" for k, v in sorted(mn_seen.items())))
print("functions whose ENTER and LEAVE counts differ (several exits per region):", len(unpaired))
print("violations:", len(bad))
for b in bad[:40]:
    print("  ", b)
sys.exit(1 if bad else 0)
