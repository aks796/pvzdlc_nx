"""Function discovery and per-function control flow (recursive descent)."""
import capstone
from capstone import x86

STOP = {"ret", "retf", "int3", "hlt", "ud2", "iretd", "sysret", "into", "int1"}
COND_JUMPS = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp",
              "jl", "jge", "jle", "jg", "jecxz", "loop", "loope", "loopne"}
MAX_TABLE = 4096


def make_cs():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    return md


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}        # va -> CsInsn
        self.labels = {entry}  # branch targets inside the function
        self.tails = set()     # other functions reached by jump or fallthrough
        self.tables = {}       # indirect-jmp va -> [targets]
        self.calls = set()
        self.resume = []       # continuations (after catch / __except) inside this function


class Program:
    def __init__(self, img):
        self.img = img
        self.md = make_cs()
        self.cache = {}
        self.entries = set()
        self.funcs = {}

    def decode(self, va):
        insn = self.cache.get(va)
        if insn is None:
            if not self.img.in_text(va):
                return None
            for insn in self.md.disasm(self.img.bytes_at(va, 16), va, 1):
                break
            else:
                insn = False
            self.cache[va] = insn
        return insn or None

    def resolve(self, va):
        """Follow ILT thunks to the function body."""
        seen = 0
        while va in self.img.ilt and seen < 4:
            va = self.img.ilt[va]
            seen += 1
        return va

    def code_pointer(self, va):
        """A taken address in .text that names a function, or None."""
        if not self.img.in_text(va):
            return None
        return self.resolve(va)

    def find_continuations(self):
        """Targets of `mov eax, offset L; ret` (a C++ catch funclet returning where the parent resumes).
        They are labels inside the function whose frame resumes there, never functions of their own: as
        functions, the CRT's jump to them would be lifted as a call (see rt_resume)."""
        img = self.img
        conts = set()
        text = img.bytes_at(img.text_start, img.text_end - img.text_start)
        i = text.find(b"\xb8")
        while i >= 0:
            va = img.text_start + i
            if i + 5 < len(text) and text[i + 5] == 0xC3 and va + 1 in img.relocs:
                t = img.u32(va + 1)
                if img.in_text(t):
                    conts.add(t)
            i = text.find(b"\xb8", i + 1)
        self.cont_targets = conts

    def seed(self):
        img = self.img
        self.find_continuations()
        self.entries.add(self.resolve(img.entry))
        self.entries.update(self.resolve(t) for t in img.ilt.values())
        # Function pointers stored in data (vtables, CRT init tables, ...)
        for r in img.reloc_list:
            if img.in_text(r):
                continue
            v = img.u32(r)
            if (img.in_text(v) and v >= img.ilt_end or v in img.ilt) and v not in self.cont_targets:
                self.entries.add(self.resolve(v))

    def jump_table(self, insn):
        op = insn.operands[0]
        if op.type != x86.X86_OP_MEM or op.mem.base != 0 or op.mem.index == 0 or op.mem.scale != 4:
            return None
        tbl = op.mem.disp & 0xFFFFFFFF
        if insn.address + insn.disp_offset not in self.img.relocs:
            return None
        out = []
        for k in range(MAX_TABLE):
            slot = tbl + 4 * k
            if slot not in self.img.relocs:
                break
            t = self.img.u32(slot)
            if not self.img.in_text(t):
                break
            out.append(t)
        return out or None

    def walk(self, entry, final, extra=()):
        f = Function(entry)
        work = [entry] + list(extra)
        f.labels.update(extra)
        seen = set()
        while work:
            va = work.pop()
            while va not in seen:
                if va != entry and va in self.entries:
                    f.tails.add(va)
                    break
                insn = self.decode(va)
                if insn is None:
                    f.insns[va] = None
                    seen.add(va)
                    break
                seen.add(va)
                f.insns[va] = insn
                m = insn.mnemonic
                nxt = va + insn.size
                if m.startswith("rep") or m.startswith("lock") or m.startswith("bnd "):
                    m = m.split(" ", 1)[1] if " " in m else m
                if m in STOP:
                    break
                ops = insn.operands
                if m == "jmp":
                    if ops[0].type == x86.X86_OP_IMM:
                        t = self.resolve(ops[0].imm & 0xFFFFFFFF)
                        if t in self.entries and t != entry:
                            f.tails.add(t)
                        else:
                            f.labels.add(t)
                            work.append(t)
                    else:
                        tbl = self.jump_table(insn)
                        if tbl:
                            f.tables[va] = tbl
                            for t in tbl:
                                f.labels.add(t)
                                work.append(t)
                    break
                if m in COND_JUMPS:
                    t = self.resolve(ops[0].imm & 0xFFFFFFFF)
                    if t in self.entries and t != entry:
                        f.tails.add(t)
                    else:
                        f.labels.add(t)
                        work.append(t)
                    va = nxt
                    continue
                if m == "call":
                    if ops[0].type == x86.X86_OP_IMM:
                        t = self.resolve(ops[0].imm & 0xFFFFFFFF)
                        f.calls.add(t)
                        if not final and self.img.in_text(t) and t not in self.cont_targets:
                            self.entries.add(t)
                # Taken code addresses in immediates (push offset fn, mov [x], offset fn)
                if not final and insn.imm_size == 4 and va + insn.imm_offset in self.img.relocs:
                    for o in ops:
                        if o.type == x86.X86_OP_IMM:
                            t = self.code_pointer(o.imm & 0xFFFFFFFF)
                            if t is not None and t != entry and t not in self.cont_targets:
                                self.entries.add(t)
                va = nxt
            else:
                continue
        return f

    def discover(self, log=print):
        self.seed()
        done = set()
        rnd = 0
        while True:
            todo = sorted(self.entries - done)
            if not todo:
                break
            rnd += 1
            for e in todo:
                self.walk(e, final=False)
                done.add(e)
            log(f"discovery round {rnd}: {len(self.entries)} functions")
        self.extra = self.place_continuations(log)
        for e in sorted(self.entries):
            self.funcs[e] = self.walk(e, final=True, extra=self.extra.get(e, ()))
        self.find_resume_points(log)
        return self.funcs

    def place_continuations(self, log=print):
        """A continuation the normal flow of some function reaches belongs to it. One only reached
        through a catch belongs to the nearest function before it with a frame prologue (catch funclets
        have none: they run on the parent's ebp) and to every funclet between the two: after a try/catch
        inside a catch, the inner catch resumes in the outer funclet's code. rt_resume takes the innermost
        of them still live."""
        import bisect
        reached = set()
        last = {}
        for e in self.entries:
            f = self.walk(e, final=True)
            reached.update(c for c in self.cont_targets if c in f.insns)
            last[e] = max((va + n.size for va, n in f.insns.items() if n), default=e)
        starts = sorted(self.entries)
        extra = {}
        for c in sorted(self.cont_targets - reached):
            i = bisect.bisect_left(starts, c) - 1
            while i >= 0:
                b = self.img.bytes_at(starts[i], 6)
                if b[:2] in (b"\x8b\xff", b"\x66\x90"):  # /hotpatch pad: the DLC's own functions all have one
                    b = b[2:]
                # push ebp; mov ebp, esp / push ebx; mov ebx, esp / push ebp; lea ebp, [esp+N] (big frames)
                if b.startswith((b"\x55\x8b\xec", b"\x53\x8b\xdc", b"\x55\x8d\x6c\x24", b"\x55\x8d\xac\x24")):
                    break
                i -= 1
            # Funclets sit right after (or inside) the parent's try body, so the parent's code runs up to
            # the next function. A framed function that stops short of it isn't the parent: that one was
            # never discovered (dead code in the DLC exe: 0x72d630, 0x8d1a70), so nothing gets c
            nxt = min(starts[i + 1], c) if i >= 0 and i + 1 < len(starts) else c
            if i >= 0 and last[starts[i]] >= nxt and c - starts[i] < 0x20000:  # F_00866d20 spans 0x11600
                for e in starts[i:bisect.bisect_left(starts, c)][:8]:
                    extra.setdefault(e, []).append(c)
        log(f"continuations: {len(self.cont_targets)}, {len(reached)} reached by normal flow, "
            f"{sum(len(v) for v in extra.values())} placed by frame")
        return extra

    def find_resume_points(self, log=print):
        """Addresses execution can resume at after an exception: C++ catch
        funclets return their continuation (`mov eax, offset L; ret`), and
        SEH4 scope tables name __except blocks. Each lands in the middle of
        a parent function, which then needs a resume point (see lift.py)."""
        img = self.img
        conts = set()
        text = img.bytes_at(img.text_start, img.text_end - img.text_start)
        i = text.find(b"\xb8")
        while i >= 0:
            va = img.text_start + i
            if i + 5 < len(text) and text[i + 5] == 0xC3 and va + 1 in img.relocs:
                t = img.u32(va + 1)
                if img.in_text(t):
                    conts.add(t)
            i = text.find(b"\xb8", i + 1)
        # SEH4: push -2; push offset scopetable; push offset _except_handler4
        for f in self.funcs.values():
            for va, insn in f.insns.items():
                if insn is None or insn.mnemonic != "push" or insn.imm_size != 4:
                    continue
                if va + insn.imm_offset not in img.relocs:
                    continue
                tbl = insn.operands[0].imm & 0xFFFFFFFF
                if img.section_of(tbl) != ".rdata":
                    continue
                nxt = f.insns.get(va + insn.size)
                if not nxt or nxt.mnemonic != "push" or nxt.imm_size != 4:
                    continue
                for k in range(32):
                    e = tbl + 16 + 12 * k
                    if e + 8 not in img.relocs:
                        break
                    h = img.u32(e + 8)
                    if not img.in_text(h):
                        break
                    conts.add(h)
        owners = {}
        for fva, f in self.funcs.items():
            cs = {c for c in conts & f.insns.keys() if f.insns[c] is not None}
            if cs:
                owners[fva] = cs
        for fva, cs in owners.items():
            self.funcs[fva].resume = sorted(cs)
            self.funcs[fva].labels.update(cs)
        self.conts = {c: sorted(f for f, cs in owners.items() if c in cs) for c in conts}
        log(f"resume points: {len(conts)} continuations in {len(owners)} functions")
