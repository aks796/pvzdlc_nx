"""x86-32 instruction -> C statements.

Conventions (see runtime/x86rt.h):
- GPRs are C locals inside each function; STORE/LOAD sync them with the CPU
  struct around calls, returns and anything that leaves the function.
- Flags are lazy: each flag-setting instruction records (fl_op, fl_a, fl_b,
  fl_r, fl_c); conditions evaluate them with inline helpers that fold when the
  op is known.
- Guest addresses that the image relocates are emitted as IMG(va) so the image
  can be placed anywhere.
- x87 and XMM state live in the CPU struct.
"""
import re

from capstone import x86

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
# Selector values a 32-bit Windows process sees (only ever stored, e.g. in crash reports)
SEGS = {"cs": 0x23, "ds": 0x2B, "es": 0x2B, "ss": 0x2B, "fs": 0x53, "gs": 0x2B}
WI = {1: 0, 2: 1, 4: 2}

COND = {
    "o": "CC_O", "no": "!CC_O", "b": "CC_B", "c": "CC_B", "nae": "CC_B", "ae": "!CC_B", "nb": "!CC_B", "nc": "!CC_B",
    "e": "CC_E", "z": "CC_E", "ne": "!CC_E", "nz": "!CC_E", "be": "CC_BE", "na": "CC_BE", "a": "!CC_BE", "nbe": "!CC_BE",
    "s": "CC_S", "ns": "!CC_S", "p": "CC_P", "pe": "CC_P", "np": "!CC_P", "po": "!CC_P",
    "l": "CC_L", "nge": "CC_L", "ge": "!CC_L", "nl": "!CC_L", "le": "CC_LE", "ng": "CC_LE", "g": "!CC_LE", "nle": "!CC_LE",
}

# Flag kinds, must match x86rt.h
K = {k: i for i, k in enumerate(
    ["ADD", "SUB", "ADC", "SBB", "LOGIC", "INC", "DEC", "NEG", "SHL", "SHR", "SAR", "MUL", "EXPL", "ROL", "ROR", "BT"])}

ALU = {"add": ("+", "ADD"), "sub": ("-", "SUB"), "and": ("&", "LOGIC"), "or": ("|", "LOGIC"), "xor": ("^", "LOGIC")}


# Port fixes run inside the game's own code: before the instruction at each address,
# call the named host function (host/game_patches.c) with the CPU state synced. A
# (name, target) pair makes it a skip: when the function returns nonzero, execution
# jumps to target, an instruction of the same guest function. ("name", "ret", n) at a function's entry
# returns from it when the function returns nonzero (eax set by it), popping n bytes of arguments.
HOOKS = {
    0x912c70: "game_hook_bank_begin",   # SeedBank::Draw starts (the DLC stages' moved board: the bank where it is)
    0x91319d: "game_hook_bank_drawn",   # SeedBank::Draw: packets drawn, clip about to be cleared
    0x913347: "game_hook_bank_redraw",  # ... redraws every packet and the keyboard cursors' packets
    0x9135b0: "game_hook_bank_cursor",  # ... cleared the clip of its copy for a keyboard cursor's packet
    0x913881: "game_hook_bank_done",    # ... returns
    0x756f70: "game_hook_board_update",     # Board::Update (keyboard players, the game, 2x speed loop)
    0x75900c: "game_hook_kb_move",          # ... a keyboard cursor moves (unit direction in xmm2 / xmm1)
    0x759ddc: "game_hook_speed_pass",       # ... end of a pass: a second one follows at 2x speed
    0x759169: "game_hook_kb_plant_count",  # keyboard player plants: count of seeds on the conveyor
    0x7527e0: "game_hook_kb_zombie_ok",    # keyboard plant, a zombie: al = affordable and ready (verbose log)
    0x7592e9: "game_hook_kb_can_plant",    # keyboard player plants: CanPlantAt's answer in eax (0 = yes)
    0x7525b2: "game_hook_kb_plant_ok",     # keyboard plant (0x752520): al = it may plant (BOMB All Together's limit)
    0x75271c: ("game_hook_kb_belt_step", 0x752727),  # ... planted from a conveyor: its index steps left (too early)
    0x752913: "game_hook_kb_planted",      # ... the packet marked as planted (the belt's step goes here)
    0x7371d3: "game_hook_light_y",         # the flashlight (7-5, mode 108): esi = the mouse's y it is drawn at
    0x7371f9: "game_hook_light_x",         # ... edi = the mouse's x
    0x73def9: ("game_hook_p2_cursor_draw", 0x73df05),  # Board::Draw: player 2's cursor (versus on one touch screen)
    0x9134c0: ("game_hook_p2_bank_cursor", 0x91386c),  # SeedBank::Draw: a keyboard cursor's packet and bracket
    0x73711b: "game_hook_light_pan",       # ... the opening pan: its Graphics back on the screen (widescreen pan too)
    0x82c735: ("game_hook_title_bg", 0x82c799),  # TitleScreen::Draw's background (widescreen: zoomed + light)
    0x821120: "game_hook_menu_draw",        # GameSelector::Draw (widescreen: the menu's matrix draws are its pieces)
    0x82bc9f: "game_hook_page2_bg",         # SideScreen::Draw drew its background (esi = Graphics)
    0x9ed400: "game_hook_blt_matrix",       # DDImage::BltMatrix (widescreen: the menu's pieces grow)
    0xa04f90: "game_hook_draw_image",       # Graphics::DrawImage(Image *, x, y) (widescreen: 4:3 backgrounds swapped)
    0xa05c60: "game_hook_draw_image_f",     # Graphics::DrawImageF(Image *, x, y)
    0xa050f0: "game_hook_draw_image_src",   # Graphics::DrawImage(Image *, x, y, const Rect &src)
    0x7fd350: "game_hook_window_screen",    # ChallengeScreen::Draw (widescreen: the menu stays in view behind it)
    0x801a33: "game_hook_credits_port",     # ChallengeScreen::Draw, the credits page: its playtesters drawn (edi = g)
    0x80f604: "game_hook_credits_max",      # ChallengeScreen::Update: a page's scroll limit chosen ([esp+8], eax = page)
    0x7ddd60: "game_hook_window_screen",    # AwardScreen::Draw
    0x7ddef1: "game_hook_help_table",       # ... the HELP pages' table (widescreen: from the screen's left edge)
    0x7ddf2b: "game_hook_help_table",       # ... the letters' table
    0x7de15f: "game_hook_help_table",       # ... the letters' leaves overlay
    0x7d7c80: "game_hook_window_screen",    # AlmanacDialog::Draw
    0x92221f: "game_hook_reanim_clip",      # a reanim track's draw, before its "clip is the whole 800x600" fast path
    0x73a560: "game_hook_draw_objects_item",  # Board::DrawGameObjects draws its next render item (widescreen bushes)
    0x73a681: "game_hook_draw_objects_done",  # ... has drawn them all
    0xbab120: ("game_hook_mtxlock", "ret", 0),    # std::_Mtxlock (cdecl): EnterCriticalSection, in the host
    0xbab140: ("game_hook_mtxunlock", "ret", 0),  # std::_Mtxunlock: LeaveCriticalSection
    0x9c97f0: ("game_hook_draw_string_cull", "ret", 0),  # TodDrawString: lines off the screen skipped
    0x7615d0: ("game_hook_iter_adopt", "ret", 4),   # std::_Iterator_base12::_Adopt_unlocked: no list
    0x761590: ("game_hook_iter_adopt", "ret", 4),   # ... _Adopt_locked
    0x761e90: ("game_hook_iter_orphan", "ret", 0),  # ... _Orphan_me_unlocked
    0x761e60: ("game_hook_iter_orphan", "ret", 0),  # ... _Orphan_me_locked
    0x824d90: ("game_hook_seedmap_get", "ret", 4),  # std::map<int, ...>::operator[] (debug STL): cached
    0x824db5: "game_hook_seedmap_got",      # ... its value's address in eax
    0x764406: ("game_hook_lt_checked", 0x76441d),   # std::_Debug_lt_pred: skip the second (validation) compare
    0x730c80: "game_hook_zombie_added",     # Board::AddZombieInRow(type, row, wave) (widescreen bush rustle)
    0x78ce54: "game_hook_street_zombie",    # CutScene::PlaceAZombie set the zombie's place (esi = zombie)
    0x9e2022: "game_hook_graphics_made",    # Graphics::Graphics(Image *) stored its clip (edi = this)
    0xa00ea0: "game_hook_cliprect_in",      # Graphics::ClipRect(x, y, w, h) (widescreen)
    0xa00ef1: "game_hook_cliprect_out",     # ... its intersection stored (esi = &mClipRect)
    0xa0b4b0: "game_hook_fillrect",         # Graphics::FillRect(x, y, w, h) (widescreen)
    0x792990: "game_hook_kbcursor_draw",    # the keyboard player's cursor is drawn (g at [esp+4])
    0x793483: "game_hook_kbcursor_drawn",   # ... done (g at [ebp+0xd0])
    0x9d3d18: "game_hook_string_found",     # TodStringListFind returned (two callers)
    0x9d3e14: "game_hook_string_found",
    0xa08260: "game_hook_font_draw",        # ImageFont::DrawString starts
    0x8d6c0a: "game_hook_dave_prompt",      # LawnApp::DrawCrazyDave, its "click to continue"
    0x8d6c33: "game_hook_dave_prompt_done", # ... drawn
    0x75f8c4: ("game_hook_kb_slot_empty", 0x75f9cb),  # P1 cursor's seed tooltip: always hidden
    0x7359f0: "game_hook_board_draw_begin", # Board::Draw: drawn within the picture's ends (widescreen pan)
    0x735ac4: "game_hook_board_draw_end",   # ... its returns
    0x735adc: "game_hook_board_draw_end",
    0x735afa: "game_hook_board_draw_end",
    0x793108: "game_hook_kb_ghost_pos",     # ... its DrawSeedType: Zombies vs. Zombies' zombies placed right
    0x79307c: ("game_hook_kb_ghost_empty", 0x793110),  # cursor's ghost plant: skip it
    0x8250a7: "game_hook_imit_drawn",       # ImitaterDialog::Draw: cards drawn, its tooltip next (ebp = Graphics)
    0x90a67a: "game_hook_chooser_drawn",    # SeedChooserScreen::Draw: all drawn (esi = Graphics)
    0x7d7db4: "game_hook_almanac_drawn",    # AlmanacDialog::Draw: before its Close button (ebx = Graphics)
    0x73f02c: "game_hook_nextgarden_draw",  # Board::DrawZenButtons: the next-garden arrow drawn (ecx = edi Graphics)
    0x73f031: "game_hook_nextgarden_drawn", # ... drawn (also its skip path)
    0x8218ce: ("game_hook_lvlnum_cel", 0x8218d3),  # GameSelector overlay: Adventure's level digits (TodDrawImageCelF,
    0x82195b: ("game_hook_lvlnum_cel", 0x821960),  #  cdecl, args at esp): drawn popping with the lit sign instead
    0x8219a8: ("game_hook_lvlnum_cel", 0x8219ad),
    0x909c18: "game_hook_chmenu_draw",      # SeedChooserScreen::Draw: its Menu button drawn (ecx)
    0x909c1d: "game_hook_chmenu_drawn",     # ... drawn
    0x886240: "game_hook_btn_draw",         # DialogButton::Draw: the focused main-menu sign's pop (scale)
    0x8865e0: "game_hook_btn_draw_end",     # ... its one exit: the scale put back
    0x72be42: ("game_hook_ai_kb0", 0x72be62),  # AI versus, P1 a keyboard player: Board init: one keyboard player
    0x73dec6: ("game_hook_ai_kb1", 0x73decf),  # AI versus, P1 a keyboard player: Board::Draw: P1's cursor
    0x758cad: ("game_hook_ai_kb2", 0x758cba),  # AI versus, P1 a keyboard player: Board::Update: the keyboard players
    0x75a20c: ("game_hook_ai_kb3", 0x75a215),  # AI versus, P1 a keyboard player: Board::Update: P1's cursor
    0x75e773: ("game_hook_ai_kb4", 0x75f385),  # AI versus, P1 a keyboard player: Board::KeyDown: P1's keys
    0x788222: ("game_hook_ai_kb5", 0x78822f),  # AI versus, P1 a keyboard player: keyboard players (not co-op)
    0x9083de: ("game_hook_ai_kb6", 0x9083e7),  # AI versus, P1 a keyboard player: versus chooser: P1's cursor
    0x909cd1: ("game_hook_ai_kb7", 0x909cde),  # AI versus, P1 a keyboard player: versus chooser Draw: the cursors
    0x90a5db: ("game_hook_ai_kb8", 0x90a5e8),  # AI versus, P1 a keyboard player: versus chooser Draw: keyboard players
    0x90c008: ("game_hook_ai_kb9", 0x90c015),  # AI versus, P1 a keyboard player: versus chooser KeyDown
    0x90fefc: ("game_hook_ai_kb10", 0x90ff05),  # AI versus, P1 a keyboard player: versus chooser: P1 keyboard
    0x910897: ("game_hook_ai_kb11", 0x9108a4),  # AI versus, P1 a keyboard player: versus chooser: cursor
    0x9110b7: ("game_hook_ai_kb12", 0x9110c4),  # AI versus, P1 a keyboard player: versus chooser: cursor
    0x913329: ("game_hook_ai_kb13", 0x913347),  # AI versus, P1 a keyboard player: SeedBank::Draw: the cursors' packets
    0x9160d8: ("game_hook_ai_kb14", 0x9161a2),  # AI versus, P1 a keyboard player: SeedPacket: versus keyboard players
}

class Unsupported(Exception):
    pass


def h(v):
    return f"0x{v & 0xFFFFFFFF:x}u"


class Lifter:
    def __init__(self, prog, func, names):
        self.prog = prog
        self.img = prog.img
        self.f = func
        self.names = names  # function va -> C name
        self.out = []

    # ---- operands -------------------------------------------------------
    def rname(self, insn, r):
        return insn.reg_name(r)

    def disp_expr(self, insn, mem):
        d = mem.disp & 0xFFFFFFFF
        if insn.disp_size == 4 and insn.address + insn.disp_offset in self.img.relocs:
            return f"IMG({h(d)})"
        return h(d) if d else None

    def ea(self, insn, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg32(insn, m.base))
        if m.index:
            idx = self.reg32(insn, m.index)
            parts.append(idx if m.scale == 1 else f"{idx}*{m.scale}u")
        d = self.disp_expr(insn, m)
        if d:
            parts.append(d)
        if m.segment == x86.X86_REG_FS:
            parts.append("c->fs_base")
        elif m.segment == x86.X86_REG_GS:
            raise Unsupported("gs segment")
        return "(uint32_t)(" + (" + ".join(parts) if parts else "0u") + ")"

    def reg32(self, insn, r):
        n = self.rname(insn, r)
        if n in R32:
            return n
        raise Unsupported("address reg " + n)

    def imm(self, insn, op, size=None):
        v = op.imm & MASK[size or op.size or 4]
        if insn.imm_size == 4 and insn.address + insn.imm_offset in self.img.relocs:
            return f"IMG({h(v)})"
        return h(v)

    def rd(self, insn, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            n = self.rname(insn, op.reg)
            if n in R32:
                return n
            if n in R16:
                return f"(uint16_t){R16[n]}"
            if n in R8L:
                return f"(uint8_t){R8L[n]}"
            if n in R8H:
                return f"(uint8_t)({R8H[n]} >> 8)"
            if n in SEGS:
                return h(SEGS[n])
            raise Unsupported("read reg " + n)
        if op.type == x86.X86_OP_IMM:
            return self.imm(insn, op, size)
        if op.type == x86.X86_OP_MEM:
            ea = self.ea(insn, op)
            return {1: f"RD8({ea})", 2: f"RD16({ea})", 4: f"RD32({ea})", 8: f"RD64({ea})"}[size]
        raise Unsupported("operand")

    def wr(self, insn, op, val, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            n = self.rname(insn, op.reg)
            if n in R32:
                return f"{n} = (uint32_t)({val});"
            if n in R16:
                r = R16[n]
                return f"{r} = ({r} & 0xffff0000u) | (uint16_t)({val});"
            if n in R8L:
                r = R8L[n]
                return f"{r} = ({r} & 0xffffff00u) | (uint8_t)({val});"
            if n in R8H:
                r = R8H[n]
                return f"{r} = ({r} & 0xffff00ffu) | ((uint32_t)(uint8_t)({val}) << 8);"
            raise Unsupported("write reg " + n)
        if op.type == x86.X86_OP_MEM:
            ea = self.ea(insn, op)
            return {1: f"WR8({ea}, {val});", 2: f"WR16({ea}, {val});", 4: f"WR32({ea}, {val});",
                    8: f"WR64({ea}, {val});"}[size]
        raise Unsupported("write operand")

    def is_xmm(self, insn, op):
        return op.type == x86.X86_OP_REG and self.rname(insn, op.reg).startswith("xmm")

    def x(self, insn, op):
        return f"c->x[{self.rname(insn, op.reg)[3:]}]"

    # ---- flags ----------------------------------------------------------
    @staticmethod
    def fl(kind, size, a, b, r, cin="fl_c"):
        op = K[kind] * 4 + WI[size]
        s = f"fl_op = {op}; fl_a = {a}; fl_b = {b};" + (f" fl_r = {r};" if r != "fl_r" else "")
        if cin != "fl_c":
            s += f" fl_c = {cin};"
        return s

    # ---- control flow helpers ------------------------------------------
    def fname(self, va):
        return self.names.get(va)

    def tail(self, va):
        n = self.fname(va)
        if n:
            return f"STORE; {n}(c); return;"
        return f"STORE; rt_call(c, IMG({h(va)})); return;"

    def goto(self, va):
        if va in self.f.labels and va in self.f.insns:
            return f"goto L_{va:08x};"
        return self.tail(va)

    def call_direct(self, insn, target):
        ret = insn.address + insn.size
        target = follow_jmp(self.prog, target)
        if target in INLINE_C:
            return INLINE_C[target]
        n = self.fname(target)
        push = f"esp -= 4; WR32(esp, IMG({h(ret)}));"
        body = inline_body(self.prog, target, self.names) if target != self.f.entry and not isinstance(self, InlineLifter) else None
        if body is not None:
            # a small leaf function's body in place of the call: the caller's registers stay in its locals
            self.n_inline = getattr(self, "n_inline", 0) + 1
            return f"{push} {{ {body.replace('@P@', f'I{self.n_inline}_')} }}"
        if n:
            return f"{push} STORE; {n}(c); LOAD;"
        return f"{push} STORE; rt_call(c, IMG({h(target)})); LOAD;"

    # ---- main -----------------------------------------------------------
    def emit(self, s):
        self.out.append("    " + s)

    def lift_function(self):
        f = self.f
        name = self.names[f.entry]
        o = self.out
        o.append(f"void {name}(CPU *c)\n{{\n    LOCALS;\n    RT_ENTER({h(f.entry)});")
        order = sorted(f.insns)
        # Fallthroughs that land somewhere other than the next emitted insn need a label too
        for k, va in enumerate(order):
            insn = f.insns[va]
            if insn is not None and self.falls_through(insn):
                nxt = va + insn.size
                if nxt in f.insns and (k + 1 >= len(order) or order[k + 1] != nxt):
                    f.labels.add(nxt)
        for va, hk in HOOKS.items():
            if isinstance(hk, tuple) and hk[1] == "ret":
                continue
            if isinstance(hk, tuple) and va in f.insns:
                assert hk[1] in f.insns, f"hook skip target {hk[1]:#x} is outside {name}"
                f.labels.add(hk[1])
        resume = getattr(f, "resume", [])
        if resume:
            # Exceptions resume mid-function (after a catch / __except): keep a
            # resume point on the CPU's chain while this function is live.
            cases = " ".join(f"case {h(x)}: goto L_{x:08x};" for x in resume)
            self.emit(f"RtJmp rj_; rj_.prev = c->rj_top; rj_.func = {h(f.entry)}; rj_.esp0 = c->esp; c->rj_top = &rj_;")
            self.emit(f"if (__builtin_setjmp(rj_.buf)) {{ c->rj_top = &rj_; LOAD; switch (c->resume_va) {{ {cases} "
                      f"default: break; }} rt_trap(c, c->resume_va, \"bad resume\"); }}")
        if order and order[0] != f.entry:
            self.emit(f"goto L_{f.entry:08x};")
        for k, va in enumerate(order):
            insn = f.insns[va]
            if va in f.labels or k == 0:
                o.append(f"L_{va:08x}:;")
            if va in HOOKS:
                hk = HOOKS[va]
                if isinstance(hk, tuple) and hk[1] == "ret":
                    # ("name", "ret", n): at a function's entry, when the host returns nonzero (having set
                    # eax), the function returns at once, popping its n argument bytes
                    self.emit(f"{{ int {hk[0]}(CPU *); STORE; int r_ = {hk[0]}(c); LOAD; "
                              f"if (r_) {{ esp += {4 + hk[2]}; STORE; return; }} }}")
                elif isinstance(hk, tuple):
                    self.emit(f"{{ int {hk[0]}(CPU *); STORE; int r_ = {hk[0]}(c); LOAD; "
                              f"if (r_) goto L_{hk[1]:08x}; }}")
                else:
                    self.emit(f"{{ void {hk}(CPU *); STORE; {hk}(c); LOAD; }}")
            if insn is None:
                self.emit(f"STORE; rt_trap(c, {h(va)}, \"undecodable\"); return;")
                continue
            try:
                self.lift(insn)
            except (Unsupported, KeyError) as e:
                txt = f"{insn.mnemonic} {insn.op_str}".replace('"', "'")
                self.emit(f"STORE; rt_unimpl(c, {h(va)}, \"{txt}\"); return; /* {e} */")
            # Fallthrough into an address that isn't emitted next
            nxt = va + insn.size
            if self.falls_through(insn):
                if k + 1 >= len(order) or order[k + 1] != nxt:
                    self.emit(self.goto(nxt) if nxt in f.insns else self.tail(nxt))
        o.append("}\n")
        text = "\n".join(o)
        if resume:
            # leave the resume chain before any tail transfer or return
            text = re.sub(r"STORE; ((?:F_[0-9a-f]+|rt_call)\(c[^;]*\)); return;", r"STORE; RJ_POP; \1; return;", text)
            text = text.replace("return;", "{ RJ_POP; return; }")
        return text

    @staticmethod
    def base_mn(insn):
        m = insn.mnemonic
        for p in ("rep ", "repe ", "repne ", "lock ", "bnd ", "notrack "):
            if m.startswith(p):
                return p.strip(), m[len(p):]
        return "", m

    def falls_through(self, insn):
        _, m = self.base_mn(insn)
        if m in ("ret", "retf", "jmp", "int3", "hlt", "ud2", "iretd", "sysret", "into", "int1"):
            return False
        return True

    def cond_jump(self, insn, cc):
        t = self.prog.resolve(insn.operands[0].imm & 0xFFFFFFFF)
        self.emit(f"if ({COND[cc]}) {{ {self.goto(t)} }}")

    def lift(self, insn):
        pfx, m = self.base_mn(insn)
        ops = insn.operands
        va = insn.address
        E = self.emit

        if pfx in ("rep", "repe", "repne"):
            return self.lift_string(insn, pfx, m)
        if m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsd", "scasb", "scasd",
                 "cmpsb", "cmpsd") and (not ops or all(o.type == x86.X86_OP_MEM for o in ops)):
            return self.lift_string(insn, "", m)

        if m in ("nop", "lfence", "mfence", "sfence", "pause", "wait", "fwait", "prefetchnta", "prefetcht0",
                 "prefetcht1", "prefetcht2", "prefetchw", "prefetchwt1", "vzeroupper", "fnclex", "fninit"):
            if m == "fninit":
                E("c->ftop = 0; c->fsw = 0; c->fcw = 0x37f;")
            return
        if m in ("int3", "hlt", "ud2", "int", "int1", "into", "iretd", "sysret", "syscall", "in", "out", "insb",
                 "insd", "outsb", "outsd", "outsw", "cli", "sti", "les", "lds", "bound", "arpl", "ljmp", "lcall",
                 "retf", "daa", "das", "aaa", "aas", "aam", "aad", "salc"):
            E(f"STORE; rt_trap(c, {h(va)}, \"{m}\"); return;")
            return

        if m == "mov":
            E(self.wr(insn, ops[0], self.rd(insn, ops[1], ops[0].size)))
            return
        if m == "movzx":
            E(self.wr(insn, ops[0], self.rd(insn, ops[1])))
            return
        if m == "movsx":
            t = {1: "int8_t", 2: "int16_t"}[ops[1].size]
            E(self.wr(insn, ops[0], f"(uint32_t)(int32_t)({t})({self.rd(insn, ops[1])})"))
            return
        if m == "lea":
            E(self.wr(insn, ops[0], self.ea_nofs(insn, ops[1])))
            return
        if m == "push":
            if ops[0].size == 2:
                E(f"{{ uint32_t v_ = {self.rd(insn, ops[0])}; esp -= 2; WR16(esp, v_); }}")
            else:
                E(f"{{ uint32_t v_ = {self.rd(insn, ops[0], 4)}; esp -= 4; WR32(esp, v_); }}")
            return
        if m == "pop":
            if ops[0].size == 2:
                E(f"{{ uint32_t v_ = RD16(esp); esp += 2; {self.wr(insn, ops[0], 'v_')} }}")
            else:
                E(f"{{ uint32_t v_ = RD32(esp); esp += 4; {self.wr(insn, ops[0], 'v_')} }}")
            return
        if m == "pushal":
            E("{ uint32_t s_ = esp; esp -= 32; WR32(esp+28, eax); WR32(esp+24, ecx); WR32(esp+20, edx); "
              "WR32(esp+16, ebx); WR32(esp+12, s_); WR32(esp+8, ebp); WR32(esp+4, esi); WR32(esp, edi); }")
            return
        if m == "popal":
            E("edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16); edx = RD32(esp+20); "
              "ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32;")
            return
        if m == "pushfd":
            E("{ uint32_t v_ = EFLAGS | (c->df << 10); esp -= 4; WR32(esp, v_); }")
            return
        if m == "popfd":
            E("{ uint32_t v_ = RD32(esp); esp += 4; c->df = (v_ >> 10) & 1; "
              + self.fl("EXPL", 4, "0", "0", "v_") + " }")
            return
        if m == "lahf":
            E("eax = (eax & 0xffff00ffu) | ((EFLAGS & 0xd5u) << 8) | 0x200u;")
            return
        if m == "sahf":
            E("{ uint32_t v_ = ((eax >> 8) & 0xd5u) | (EFLAGS & 0x800u); " + self.fl("EXPL", 4, "0", "0", "v_") + " }")
            return
        if m in ("stc", "clc", "cmc"):
            v = {"stc": "(EFLAGS | 1u)", "clc": "(EFLAGS & ~1u)", "cmc": "(EFLAGS ^ 1u)"}[m]
            E("{ uint32_t v_ = " + v + "; " + self.fl("EXPL", 4, "0", "0", "v_") + " }")
            return
        if m in ("cld", "std"):
            E(f"c->df = {1 if m == 'std' else 0};")
            return
        if m == "cdq":
            E("edx = (uint32_t)((int32_t)eax >> 31);")
            return
        if m == "cwde":
            E("eax = (uint32_t)(int32_t)(int16_t)eax;")
            return
        if m == "cbw":
            E("eax = (eax & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)eax;")
            return
        if m == "cwd":
            E("edx = (edx & 0xffff0000u) | (uint16_t)((int16_t)eax >> 15);")
            return
        if m == "bswap":
            r = self.rname(insn, ops[0].reg)
            E(f"{r} = __builtin_bswap32({r});")
            return
        if m == "xchg":
            a, b = ops
            E(f"{{ uint32_t t_ = {self.rd(insn, a)}; {self.wr(insn, a, self.rd(insn, b))} {self.wr(insn, b, 't_')} }}")
            return
        if m == "xadd":
            a, b = ops
            s = a.size
            E(f"{{ uint32_t a_ = {self.rd(insn, a)}, b_ = {self.rd(insn, b)}, r_ = (a_ + b_) & {h(MASK[s])}; "
              f"{self.wr(insn, b, 'a_')} {self.wr(insn, a, 'r_')} {self.fl('ADD', s, 'a_', 'b_', 'r_')} }}")
            return
        if m == "cmpxchg":
            a, b = ops
            s = a.size
            acc = {4: "eax", 2: "(uint16_t)eax", 1: "(uint8_t)eax"}[s]
            accw = {4: "eax = d_;", 2: "eax = (eax & 0xffff0000u) | (uint16_t)d_;",
                    1: "eax = (eax & 0xffffff00u) | (uint8_t)d_;"}[s]
            E(f"{{ uint32_t d_ = {self.rd(insn, a)}, a_ = {acc}; {self.fl('SUB', s, 'a_', 'd_', f'(a_ - d_) & {h(MASK[s])}')} "
              f"if (a_ == d_) {{ {self.wr(insn, a, self.rd(insn, b))} }} else {{ {accw} }} }}")
            return
        if m == "leave":
            E("esp = ebp; ebp = RD32(esp); esp += 4;")
            return
        if m == "xlatb":
            E("eax = (eax & 0xffffff00u) | RD8(ebx + (uint8_t)eax);")
            return

        if m in ALU:
            sym, kind = ALU[m]
            a, b = ops
            s = a.size
            bv = self.rd(insn, b, s)
            if m == "xor" and a.type == b.type == x86.X86_OP_REG and a.reg == b.reg:
                E(f"{self.wr(insn, a, '0')} {self.fl('LOGIC', s, '0', '0', '0')}")
                return
            E(f"{{ uint32_t a_ = {self.rd(insn, a)}, b_ = {bv}, r_ = (a_ {sym} b_) & {h(MASK[s])}; "
              f"{self.wr(insn, a, 'r_')} {self.fl(kind, s, 'a_', 'b_', 'r_')} }}")
            return
        if m in ("cmp", "test"):
            a, b = ops
            s = a.size
            r = "(a_ - b_)" if m == "cmp" else "(a_ & b_)"
            E(f"{{ uint32_t a_ = {self.rd(insn, a)}, b_ = {self.rd(insn, b, s)}; "
              f"{self.fl('SUB' if m == 'cmp' else 'LOGIC', s, 'a_', 'b_', r + ' & ' + h(MASK[s]))} }}")
            return
        if m in ("adc", "sbb"):
            a, b = ops
            s = a.size
            sym = "+" if m == "adc" else "-"
            E(f"{{ uint32_t ci_ = CC_B, a_ = {self.rd(insn, a)}, b_ = {self.rd(insn, b, s)}, "
              f"r_ = (a_ {sym} b_ {sym} ci_) & {h(MASK[s])}; {self.wr(insn, a, 'r_')} "
              f"{self.fl(m.upper(), s, 'a_', 'b_', 'r_', 'ci_')} }}")
            return
        if m in ("inc", "dec"):
            a = ops[0]
            s = a.size
            sym = "+" if m == "inc" else "-"
            E(f"{{ uint32_t ci_ = CC_B, a_ = {self.rd(insn, a)}, r_ = (a_ {sym} 1) & {h(MASK[s])}; "
              f"{self.wr(insn, a, 'r_')} {self.fl(m.upper(), s, 'a_', '1', 'r_', 'ci_')} }}")
            return
        if m == "neg":
            a = ops[0]
            s = a.size
            E(f"{{ uint32_t a_ = {self.rd(insn, a)}, r_ = (0u - a_) & {h(MASK[s])}; "
              f"{self.wr(insn, a, 'r_')} {self.fl('NEG', s, 'a_', '0', 'r_')} }}")
            return
        if m == "not":
            a = ops[0]
            E(self.wr(insn, a, f"~{self.rd(insn, a)}"))
            return
        if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            return self.lift_shift(insn, m)
        if m in ("shld", "shrd"):
            a, b, cnt = ops
            if a.size != 4:
                raise Unsupported("16-bit shld")
            n = self.rd(insn, cnt, 1)
            if m == "shrd":
                r = "(a_ >> n_) | (b_ << (32 - n_))"
                kind = "SHR"
            else:
                r = "(a_ << n_) | (b_ >> (32 - n_))"
                kind = "SHL"
            E(f"{{ uint32_t n_ = {n} & 31; if (n_) {{ uint32_t a_ = {self.rd(insn, a)}, b_ = {self.rd(insn, b)}, "
              f"r_ = {r}; {self.wr(insn, a, 'r_')} {self.fl(kind, 4, 'a_', 'n_', 'r_')} }} }}")
            return
        if m in ("rcr", "rcl"):
            a, cnt = ops
            if a.size != 4:
                raise Unsupported("narrow rcr")
            n = self.rd(insn, cnt, 1)
            if m == "rcr":
                step = "{ uint32_t o_ = a_ & 1; a_ = (a_ >> 1) | (cf_ << 31); cf_ = o_; }"
            else:
                step = "{ uint32_t o_ = a_ >> 31; a_ = (a_ << 1) | cf_; cf_ = o_; }"
            E(f"{{ uint32_t n_ = {n} & 31; if (n_) {{ uint32_t cf_ = CC_B, a_ = {self.rd(insn, a)}; "
              f"while (n_--) {step} {self.wr(insn, a, 'a_')} "
              f"{{ uint32_t v_ = (EFLAGS & ~1u) | cf_; {self.fl('EXPL', 4, '0', '0', 'v_')} }} }} }}")
            return
        if m in ("bt", "bts", "btr", "btc"):
            return self.lift_bt(insn, m)
        if m in ("bsf", "bsr", "tzcnt", "lzcnt", "popcnt"):
            a, b = ops
            if a.size != 4:
                raise Unsupported("narrow bitscan")
            src = self.rd(insn, b)
            if m == "bsf":
                E(f"{{ uint32_t s_ = {src}; if (s_) {{ {self.wr(insn, a, '__builtin_ctz(s_)')} }} "
                  f"{{ uint32_t v_ = s_ ? 0u : 0x40u; {self.fl('EXPL', 4, '0', '0', 'v_')} }} }}")
            elif m == "bsr":
                E(f"{{ uint32_t s_ = {src}; if (s_) {{ {self.wr(insn, a, '31 - __builtin_clz(s_)')} }} "
                  f"{{ uint32_t v_ = s_ ? 0u : 0x40u; {self.fl('EXPL', 4, '0', '0', 'v_')} }} }}")
            elif m == "tzcnt":
                E(f"{{ uint32_t s_ = {src}, r_ = s_ ? (uint32_t)__builtin_ctz(s_) : 32u; {self.wr(insn, a, 'r_')} "
                  f"{{ uint32_t v_ = (s_ ? 0u : 1u) | (r_ ? 0u : 0x40u); {self.fl('EXPL', 4, '0', '0', 'v_')} }} }}")
            elif m == "lzcnt":
                E(f"{{ uint32_t s_ = {src}, r_ = s_ ? (uint32_t)__builtin_clz(s_) : 32u; {self.wr(insn, a, 'r_')} "
                  f"{{ uint32_t v_ = (s_ ? 0u : 1u) | (r_ ? 0u : 0x40u); {self.fl('EXPL', 4, '0', '0', 'v_')} }} }}")
            else:
                E(f"{{ uint32_t s_ = {src}; {self.wr(insn, a, '__builtin_popcount(s_)')} "
                  f"{{ uint32_t v_ = s_ ? 0u : 0x40u; {self.fl('EXPL', 4, '0', '0', 'v_')} }} }}")
            return
        if m in ("imul", "mul", "div", "idiv"):
            return self.lift_muldiv(insn, m)
        if m.startswith("set") and m[3:] in COND:
            E(self.wr(insn, ops[0], f"({COND[m[3:]]}) ? 1u : 0u"))
            return
        if m.startswith("cmov") and m[4:] in COND:
            E(f"if ({COND[m[4:]]}) {{ {self.wr(insn, ops[0], self.rd(insn, ops[1]))} }}")
            return

        # ---- control flow ----
        if m == "jmp":
            o = ops[0]
            if o.type == x86.X86_OP_IMM:
                E(self.goto(self.prog.resolve(o.imm & 0xFFFFFFFF)))
                return
            t = self.rd(insn, o, 4)
            tbl = self.f.tables.get(va)
            if tbl:
                cases = " ".join(f"case {h(x)}: goto L_{x:08x};" for x in sorted(set(tbl)) if x in self.f.insns)
                E(f"switch ({t} - IMG(0)) {{ {cases} default: {{ uint32_t t_ = {t}; STORE; rt_call(c, t_); return; }} }}")
            else:
                E(f"{{ uint32_t t_ = {t}; STORE; rt_call(c, t_); return; }}")
            return
        if m.startswith("j") and m[1:] in COND:
            return self.cond_jump(insn, m[1:])
        if m == "jecxz":
            t = self.prog.resolve(ops[0].imm & 0xFFFFFFFF)
            E(f"if (ecx == 0) {{ {self.goto(t)} }}")
            return
        if m in ("loop", "loope", "loopne"):
            t = self.prog.resolve(ops[0].imm & 0xFFFFFFFF)
            extra = {"loop": "", "loope": " && CC_E", "loopne": " && !CC_E"}[m]
            E(f"ecx--; if (ecx != 0{extra}) {{ {self.goto(t)} }}")
            return
        if m == "call":
            o = ops[0]
            if o.type == x86.X86_OP_IMM:
                E(self.call_direct(insn, self.prog.resolve(o.imm & 0xFFFFFFFF)))
            else:
                ret = va + insn.size
                E(f"{{ uint32_t t_ = {self.rd(insn, o, 4)}; esp -= 4; WR32(esp, IMG({h(ret)})); "
                  f"STORE; rt_call(c, t_); LOAD; }}")
            return
        if m == "ret":
            n = ops[0].imm if ops else 0
            E(f"esp += {4 + n}; STORE; return;")
            return

        if m == "cpuid":
            E("STORE; rt_cpuid(c); LOAD;")
            return
        if m == "rdtsc":
            E("STORE; rt_rdtsc(c); LOAD;")
            return
        if m == "xgetbv":
            E("STORE; rt_xgetbv(c); LOAD;")
            return

        if insn.opcode[0] in range(0xD8, 0xE0) and insn.mnemonic.startswith("f"):
            return self.lift_x87(insn)
        if m in ("fnstsw", "fstsw") and ops and ops[0].type == x86.X86_OP_REG:
            E("eax = (eax & 0xffff0000u) | FSW;")
            return
        return self.lift_sse(insn, m)

    def ea_nofs(self, insn, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg32(insn, m.base))
        if m.index:
            idx = self.reg32(insn, m.index)
            parts.append(idx if m.scale == 1 else f"{idx}*{m.scale}u")
        d = self.disp_expr(insn, m)
        if d:
            parts.append(d)
        return "(uint32_t)(" + (" + ".join(parts) if parts else "0u") + ")"

    # ---- groups ---------------------------------------------------------
    def lift_shift(self, insn, m):
        a = insn.operands[0]
        s = a.size
        bits = s * 8
        if len(insn.operands) > 1:
            n = self.rd(insn, insn.operands[1], 1)
        else:
            n = "1u"
        mask = h(MASK[s])
        if m in ("shl", "sal"):
            r, kind = f"(a_ << n_) & {mask}", "SHL"
        elif m == "shr":
            r, kind = "a_ >> n_", "SHR"
        elif m == "sar":
            st = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[s]
            r, kind = f"(uint32_t)((int32_t)({st})a_ >> (n_ < {bits} ? n_ : {bits - 1})) & {mask}", "SAR"
        elif m == "rol":
            r, kind = f"((a_ << (n_ % {bits})) | (a_ >> (({bits} - n_ % {bits}) % {bits}))) & {mask}", "ROL"
        else:
            r, kind = f"((a_ >> (n_ % {bits})) | (a_ << (({bits} - n_ % {bits}) % {bits}))) & {mask}", "ROR"
        if m in ("shl", "sal", "shr") and s < 4:
            r = f"(n_ < 32 ? ({r}) : 0u) & {mask}"
        self.emit(f"{{ uint32_t n_ = {n} & 31; if (n_) {{ uint32_t a_ = {self.rd(insn, a)}, r_ = {r}; "
                  f"{self.wr(insn, a, 'r_')} {self.fl(kind, s, 'a_', 'n_', 'r_')} }} }}")

    def lift_bt(self, insn, m):
        a, b = insn.operands
        if a.size != 4:
            raise Unsupported("16-bit bt")
        if a.type == x86.X86_OP_MEM and b.type == x86.X86_OP_REG:
            base = self.ea(insn, a)
            addr = f"({base} + (uint32_t)(((int32_t){self.rd(insn, b)} >> 5) * 4))"
            val, wr = f"RD32(p_)", "WR32(p_, v_);"
            pre = f"uint32_t p_ = {addr}, n_ = {self.rd(insn, b)} & 31;"
        else:
            val = self.rd(insn, a)
            wr = self.wr(insn, a, "v_")
            pre = f"uint32_t n_ = {self.rd(insn, b, 1)} & 31;"
        upd = {"bt": None, "bts": "v_ = o_ | (1u << n_);", "btr": "v_ = o_ & ~(1u << n_);",
               "btc": "v_ = o_ ^ (1u << n_);"}[m]
        s = f"{{ {pre} uint32_t o_ = {val}, bit_ = (o_ >> n_) & 1; "
        if upd:
            s += f"uint32_t v_; {upd} {wr} "
        s += self.fl("BT", 4, "o_", "n_", "fl_r", "bit_") + " }"
        self.emit(s)

    def lift_muldiv(self, insn, m):
        ops = insn.operands
        E = self.emit
        if m == "imul" and len(ops) >= 2:
            d = ops[0]
            s = d.size
            if len(ops) == 2:
                a, b = self.rd(insn, d), self.rd(insn, ops[1])
            else:
                a, b = self.rd(insn, ops[1]), self.rd(insn, ops[2], s)
            st = {2: "int16_t", 4: "int32_t"}[s]
            E(f"{{ int64_t p_ = (int64_t)({st}){a} * (int64_t)({st}){b}; uint32_t r_ = (uint32_t)p_ & {h(MASK[s])}; "
              f"{self.wr(insn, d, 'r_')} {self.fl('MUL', s, '0', f'(p_ != ({st})p_)', 'r_')} }}")
            return
        src = ops[0]
        s = src.size
        v = self.rd(insn, src)
        if s == 4:
            if m == "mul":
                E(f"{{ uint64_t p_ = (uint64_t)eax * (uint64_t)({v}); eax = (uint32_t)p_; edx = (uint32_t)(p_ >> 32); "
                  f"{self.fl('MUL', 4, '0', 'edx != 0', 'eax')} }}")
            elif m == "imul":
                E(f"{{ int64_t p_ = (int64_t)(int32_t)eax * (int64_t)(int32_t)({v}); eax = (uint32_t)p_; "
                  f"edx = (uint32_t)((uint64_t)p_ >> 32); {self.fl('MUL', 4, '0', '(p_ != (int32_t)p_)', 'eax')} }}")
            elif m == "div":
                E(f"{{ uint64_t n_ = ((uint64_t)edx << 32) | eax; uint32_t d_ = {v}; if (!d_) {{ STORE; rt_trap(c, {h(insn.address)}, \"div0\"); }} "
                  f"eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ % d_); }}")
            else:
                E(f"{{ int64_t n_ = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d_ = (int32_t)({v}); "
                  f"if (!d_) {{ STORE; rt_trap(c, {h(insn.address)}, \"div0\"); }} "
                  f"eax = (uint32_t)(int32_t)(n_ / d_); edx = (uint32_t)(int32_t)(n_ % d_); }}")
            return
        if s == 1:
            if m == "mul":
                E(f"{{ uint32_t p_ = (uint8_t)eax * (uint32_t)({v}); eax = (eax & 0xffff0000u) | (p_ & 0xffffu); "
                  f"{self.fl('MUL', 1, '0', '(p_ >> 8) != 0', 'p_')} }}")
            elif m == "imul":
                E(f"{{ int32_t p_ = (int8_t)eax * (int32_t)(int8_t)({v}); eax = (eax & 0xffff0000u) | ((uint32_t)p_ & 0xffffu); "
                  f"{self.fl('MUL', 1, '0', '(p_ != (int8_t)p_)', '(uint32_t)p_')} }}")
            elif m == "div":
                E(f"{{ uint32_t n_ = (uint16_t)eax, d_ = {v}; eax = (eax & 0xffff0000u) | ((n_ % d_) << 8) | ((n_ / d_) & 0xffu); }}")
            else:
                E(f"{{ int32_t n_ = (int16_t)eax, d_ = (int8_t)({v}); eax = (eax & 0xffff0000u) | (((uint32_t)(n_ % d_) & 0xffu) << 8) | ((uint32_t)(n_ / d_) & 0xffu); }}")
            return
        if s == 2:
            if m in ("mul", "imul"):
                t = "uint32_t" if m == "mul" else "int32_t"
                c16 = "uint16_t" if m == "mul" else "int16_t"
                E(f"{{ {t} p_ = ({t})({c16})eax * ({t})({c16})({v}); eax = (eax & 0xffff0000u) | ((uint32_t)p_ & 0xffffu); "
                  f"edx = (edx & 0xffff0000u) | (((uint32_t)p_ >> 16) & 0xffffu); "
                  f"{self.fl('MUL', 2, '0', '(edx & 0xffffu) != 0', '(uint32_t)p_')} }}")
            else:
                t, c16 = ("uint32_t", "uint16_t") if m == "div" else ("int32_t", "int16_t")
                E(f"{{ {t} n_ = ({t})((((uint32_t)(uint16_t)edx) << 16) | (uint16_t)eax), d_ = ({c16})({v}); "
                  f"eax = (eax & 0xffff0000u) | ((uint32_t)(n_ / d_) & 0xffffu); "
                  f"edx = (edx & 0xffff0000u) | ((uint32_t)(n_ % d_) & 0xffffu); }}")
            return
        raise Unsupported("muldiv size")

    def lift_string(self, insn, pfx, m):
        E = self.emit
        step = {"b": 1, "w": 2, "d": 4}[m[-1]]
        rd = {1: "RD8", 2: "RD16", 4: "RD32"}[step]
        wr = {1: "WR8", 2: "WR16", 4: "WR32"}[step]
        acc = {1: "(uint8_t)eax", 2: "(uint16_t)eax", 4: "eax"}[step]
        accw = {1: "eax = (eax & 0xffffff00u) | v_;", 2: "eax = (eax & 0xffff0000u) | v_;", 4: "eax = v_;"}[step]
        d = f"(c->df ? -{step} : {step})"
        base = m[:-1]
        if base == "movs":
            body = f"{wr}(edi, {rd}(esi)); esi += {d}; edi += {d};"
        elif base == "stos":
            body = f"{wr}(edi, {acc}); edi += {d};"
        elif base == "lods":
            body = f"{{ uint32_t v_ = {rd}(esi); {accw} }} esi += {d};"
        elif base == "scas":
            body = (f"{{ uint32_t a_ = {acc}, b_ = {rd}(edi); {self.fl('SUB', step, 'a_', 'b_', f'(a_ - b_) & {h(MASK[step])}')} }} "
                    f"edi += {d};")
        elif base == "cmps":
            body = (f"{{ uint32_t a_ = {rd}(esi), b_ = {rd}(edi); {self.fl('SUB', step, 'a_', 'b_', f'(a_ - b_) & {h(MASK[step])}')} }} "
                    f"esi += {d}; edi += {d};")
        else:
            raise Unsupported("string " + m)
        if not pfx:
            E(body)
            return
        if base in ("movs", "stos", "lods"):
            E(f"while (ecx) {{ {body} ecx--; }}")
        else:
            stop = "!CC_E" if pfx in ("rep", "repe") else "CC_E"
            E(f"while (ecx) {{ {body} ecx--; if ({stop}) break; }}")

    # ---- x87 ------------------------------------------------------------
    def lift_x87(self, insn):
        E = self.emit
        op = insn.opcode[0]
        modrm = insn.modrm
        reg = (modrm >> 3) & 7
        mod3 = (modrm >> 6) == 3
        i = modrm & 7
        mem = None
        for o in insn.operands:
            if o.type == x86.X86_OP_MEM:
                mem = self.ea(insn, o)
        arith = ["+", "*", "cmp", "cmpp", "-", "r-", "/", "r/"]

        def binop(kind, a, b):
            return {"+": f"{a} + {b}", "*": f"{a} * {b}", "-": f"{a} - {b}", "r-": f"{b} - {a}",
                    "/": f"{a} / {b}", "r/": f"{b} / {a}"}[kind]

        if op in (0xD8, 0xDC, 0xDA, 0xDE) and not mod3:
            src = {0xD8: f"(double)RDF32({mem})", 0xDC: f"RDF64({mem})",
                   0xDA: f"(double)(int32_t)RD32({mem})", 0xDE: f"(double)(int16_t)RD16({mem})"}[op]
            k = arith[reg]
            if k in ("cmp", "cmpp"):
                E(f"FCOM(ST(0), {src});" + (" FPOP;" if k == "cmpp" else ""))
            else:
                E(f"ST(0) = {binop(k, 'ST(0)', src)};")
            return
        if op == 0xD8 and mod3:
            k = arith[reg]
            if k in ("cmp", "cmpp"):
                E(f"FCOM(ST(0), ST({i}));" + (" FPOP;" if k == "cmpp" else ""))
            else:
                E(f"ST(0) = {binop(k, 'ST(0)', f'ST({i})')};")
            return
        if op in (0xDC, 0xDE) and mod3:
            pop = op == 0xDE
            if op == 0xDE and reg == 3 and i == 1:
                E("FCOM(ST(0), ST(1)); FPOP; FPOP;")
                return
            # Intel: DC/DE E0+i = FSUBR(P) st(i) = st0 - st(i); E8+i = FSUB(P) st(i) = st(i) - st0;
            # F0+i = FDIVR(P) st(i) = st0 / st(i); F8+i = FDIV(P) st(i) = st(i) / st0
            k = {0: "+", 1: "*", 4: "r-", 5: "-", 6: "r/", 7: "/"}.get(reg)
            if k is None:
                if reg in (2, 3):
                    E(f"FCOM(ST(0), ST({i}));" + (" FPOP;" if reg == 3 or pop else ""))
                    return
                raise Unsupported("x87 DC/DE")
            E(f"ST({i}) = {binop(k, f'ST({i})', 'ST(0)')};" + (" FPOP;" if pop else ""))
            return
        if op == 0xD9 and not mod3:
            if reg == 0:
                E(f"FPUSH((double)RDF32({mem}));")
            elif reg == 2:
                E(f"WRF32({mem}, (float)ST(0));")
            elif reg == 3:
                E(f"WRF32({mem}, (float)ST(0)); FPOP;")
            elif reg == 5:
                E(f"c->fcw = RD16({mem});")
            elif reg == 7:
                E(f"WR16({mem}, c->fcw);")
            elif reg == 4:
                E(f"c->fcw = RD16({mem}); c->fsw = RD16({mem} + 4);")
            elif reg == 6:
                E(f"WR32({mem}, c->fcw); WR32({mem} + 4, FSW); WR32({mem} + 8, 0xffffu);")
            else:
                raise Unsupported("x87 D9 mem")
            return
        if op == 0xD9 and mod3:
            b = modrm
            if 0xC0 <= b <= 0xC7:
                E(f"{{ double v_ = ST({i}); FPUSH(v_); }}")
            elif 0xC8 <= b <= 0xCF:
                E(f"{{ double v_ = ST(0); ST(0) = ST({i}); ST({i}) = v_; }}")
            elif b == 0xD0:
                pass
            else:
                tbl = {0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = __builtin_fabs(ST(0));", 0xE4: "FCOM(ST(0), 0.0);",
                       0xE5: "FXAM;", 0xE8: "FPUSH(1.0);", 0xE9: "FPUSH(3.321928094887362);",
                       0xEA: "FPUSH(1.4426950408889634);", 0xEB: "FPUSH(3.141592653589793);",
                       0xEC: "FPUSH(0.30102999566398120);", 0xED: "FPUSH(0.6931471805599453);", 0xEE: "FPUSH(0.0);",
                       0xF0: "ST(0) = rt_exp2(ST(0)) - 1.0;", 0xF1: "ST(1) = ST(1) * rt_log2(ST(0)); FPOP;",
                       0xF3: "ST(1) = rt_atan2(ST(1), ST(0)); FPOP;", 0xF5: "ST(0) = rt_remainder(ST(0), ST(1)); c->fsw &= ~0x4700u;",
                       0xF8: "ST(0) = rt_fmod(ST(0), ST(1)); c->fsw &= ~0x4700u;", 0xFA: "ST(0) = rt_sqrt(ST(0));",
                       0xFC: "ST(0) = FROUND(ST(0));", 0xFD: "ST(0) = rt_ldexp(ST(0), (int)rt_trunc(ST(1)));",
                       0xFE: "ST(0) = rt_sin(ST(0));", 0xFF: "ST(0) = rt_cos(ST(0));",
                       0xF6: "c->ftop = (c->ftop - 1) & 7;", 0xF7: "c->ftop = (c->ftop + 1) & 7;"}
                if b not in tbl:
                    raise Unsupported("x87 D9 reg")
                E(tbl[b])
            return
        if op == 0xDB and not mod3:
            t = {0: f"FPUSH((double)(int32_t)RD32({mem}));",
                 1: f"WR32({mem}, (uint32_t)FTOI32(rt_trunc(ST(0)))); FPOP;",
                 2: f"WR32({mem}, (uint32_t)FTOI32(FROUND(ST(0))));",
                 3: f"WR32({mem}, (uint32_t)FTOI32(FROUND(ST(0)))); FPOP;",
                 5: f"FPUSH(RDF80({mem}));", 7: f"WRF80({mem}, ST(0)); FPOP;"}.get(reg)
            if t is None:
                raise Unsupported("x87 DB mem")
            E(t)
            return
        if op == 0xDB and mod3:
            if modrm == 0xE2:
                E("c->fsw &= 0x3800u;")
            elif modrm == 0xE3:
                E("c->ftop = 0; c->fsw = 0; c->fcw = 0x37f;")
            elif 0xE8 <= modrm <= 0xF7:
                E(f"FCOMI(ST(0), ST({i}));")
            else:
                raise Unsupported("fcmov")
            return
        if op == 0xDD and not mod3:
            t = {0: f"FPUSH(RDF64({mem}));", 1: f"WR64({mem}, (uint64_t)FTOI64(rt_trunc(ST(0)))); FPOP;",
                 2: f"WRF64({mem}, ST(0));", 3: f"WRF64({mem}, ST(0)); FPOP;", 7: f"WR16({mem}, FSW);",
                 4: f"STORE; rt_frstor(c, {mem}); LOAD;", 6: f"STORE; rt_fnsave(c, {mem}); LOAD;"}.get(reg)
            if t is None:
                raise Unsupported("x87 DD mem")
            E(t)
            return
        if op == 0xDD and mod3:
            if reg == 0:
                return  # ffree
            if reg == 2:
                E(f"ST({i}) = ST(0);")
            elif reg == 3:
                E(f"ST({i}) = ST(0); FPOP;")
            elif reg == 4:
                E(f"FCOM(ST(0), ST({i}));")
            elif reg == 5:
                E(f"FCOM(ST(0), ST({i})); FPOP;")
            else:
                raise Unsupported("x87 DD reg")
            return
        if op == 0xDA and mod3:
            if modrm == 0xE9:
                E("FCOM(ST(0), ST(1)); FPOP; FPOP;")
                return
            raise Unsupported("fcmov")
        if op == 0xDF and not mod3:
            t = {0: f"FPUSH((double)(int16_t)RD16({mem}));",
                 1: f"WR16({mem}, (uint16_t)FTOI16(rt_trunc(ST(0)))); FPOP;",
                 2: f"WR16({mem}, (uint16_t)FTOI16(FROUND(ST(0))));",
                 3: f"WR16({mem}, (uint16_t)FTOI16(FROUND(ST(0)))); FPOP;",
                 5: f"FPUSH((double)(int64_t)RD64({mem}));",
                 7: f"WR64({mem}, (uint64_t)FTOI64(FROUND(ST(0)))); FPOP;"}.get(reg)
            if t is None:
                raise Unsupported("x87 DF mem")
            E(t)
            return
        if op == 0xDF and mod3:
            if modrm == 0xE0:
                E("eax = (eax & 0xffff0000u) | FSW;")
            elif 0xE8 <= modrm <= 0xF7:
                E(f"FCOMI(ST(0), ST({i})); FPOP;")
            else:
                raise Unsupported("x87 DF reg")
            return
        raise Unsupported("x87")

    # ---- SSE ------------------------------------------------------------
    def xsrc(self, insn, op, size):
        """C expression for an XMM-or-memory source as an X128 (memory loads `size` bytes, rest zero)."""
        if self.is_xmm(insn, op):
            return self.x(insn, op)
        if op.type == x86.X86_OP_MEM:
            ea = self.ea(insn, op)
            return {16: f"RDX128({ea})", 8: f"RDX64({ea})", 4: f"RDX32({ea})"}[size]
        raise Unsupported("xmm src")

    def lift_sse(self, insn, m):
        ops = insn.operands
        E = self.emit
        if m.startswith("v"):
            raise Unsupported("avx")
        X = lambda o: self.x(insn, o)

        if m in ("movaps", "movups", "movapd", "movupd", "movdqa", "movdqu"):
            d, s = ops
            if self.is_xmm(insn, d):
                E(f"{X(d)} = {self.xsrc(insn, s, 16)};")
            else:
                E(f"WRX128({self.ea(insn, d)}, {X(s)});")
            return
        if m in ("movss", "movsd"):
            n, fld = (4, "f[0]") if m == "movss" else (8, "d[0]")
            d, s = ops
            if self.is_xmm(insn, d) and self.is_xmm(insn, s):
                E(f"{X(d)}.{fld} = {X(s)}.{fld};")
            elif self.is_xmm(insn, d):
                E(f"{X(d)} = {self.xsrc(insn, s, n)};")
            else:
                E(f"WR{'F32' if n == 4 else 'F64'}({self.ea(insn, d)}, {X(s)}.{fld});")
            return
        if m == "movd":
            d, s = ops
            if self.is_xmm(insn, d):
                E(f"{X(d)} = X128_U32({self.rd(insn, s, 4)});")
            else:
                E(self.wr(insn, d, f"{X(s)}.u[0]", 4))
            return
        if m == "movq":
            d, s = ops
            if self.is_xmm(insn, d):
                if self.is_xmm(insn, s):
                    E(f"{{ uint64_t q_ = {X(s)}.q[0]; {X(d)}.q[0] = q_; {X(d)}.q[1] = 0; }}")
                else:
                    E(f"{X(d)} = RDX64({self.ea(insn, s)});")
            else:
                E(f"WR64({self.ea(insn, d)}, {X(s)}.q[0]);")
            return
        if m in ("movlpd", "movlps", "movhps", "movhpd"):
            lane = 0 if m.startswith("movl") else 1
            d, s = ops
            if self.is_xmm(insn, d):
                E(f"{X(d)}.q[{lane}] = RD64({self.ea(insn, s)});")
            else:
                E(f"WR64({self.ea(insn, d)}, {X(s)}.q[{lane}]);")
            return
        if m == "movlhps":
            E(f"{X(ops[0])}.q[1] = {X(ops[1])}.q[0];")
            return
        if m == "movhlps":
            E(f"{X(ops[0])}.q[0] = {X(ops[1])}.q[1];")
            return

        scalar = {"addss": ("f", "+"), "subss": ("f", "-"), "mulss": ("f", "*"), "divss": ("f", "/"),
                  "addsd": ("d", "+"), "subsd": ("d", "-"), "mulsd": ("d", "*"), "divsd": ("d", "/")}
        if m in scalar:
            t, sym = scalar[m]
            d, s = ops
            src = self.xsrc(insn, s, 4 if t == "f" else 8)
            E(f"{X(d)}.{t}[0] = {X(d)}.{t}[0] {sym} ({src}).{t}[0];")
            return
        if m in ("minss", "maxss", "minsd", "maxsd"):
            t = "f" if m.endswith("ss") else "d"
            d, s = ops
            src = self.xsrc(insn, s, 4 if t == "f" else 8)
            cmp = "<" if m.startswith("min") else ">"
            E(f"{{ X128 s_ = {src}; {X(d)}.{t}[0] = ({X(d)}.{t}[0] {cmp} s_.{t}[0]) ? {X(d)}.{t}[0] : s_.{t}[0]; }}")
            return
        if m in ("sqrtss", "sqrtsd"):
            t = "f" if m.endswith("ss") else "d"
            d, s = ops
            src = self.xsrc(insn, s, 4 if t == "f" else 8)
            fn = "rt_sqrtf" if t == "f" else "rt_sqrt"
            E(f"{X(d)}.{t}[0] = {fn}(({src}).{t}[0]);")
            return
        packed = {"addps": ("f", 4, "+"), "subps": ("f", 4, "-"), "mulps": ("f", 4, "*"), "divps": ("f", 4, "/"),
                  "addpd": ("d", 2, "+"), "subpd": ("d", 2, "-"), "mulpd": ("d", 2, "*"), "divpd": ("d", 2, "/"),
                  "paddd": ("u", 4, "+"), "psubd": ("u", 4, "-"), "paddq": ("q", 2, "+"), "psubq": ("q", 2, "-"),
                  "paddw": ("w", 8, "+"), "psubw": ("w", 8, "-"), "paddb": ("b", 16, "+"), "psubb": ("b", 16, "-"),
                  "pmulld": ("u", 4, "*")}
        if m in packed:
            t, n, sym = packed[m]
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < {n}; i_++) "
              f"{X(d)}.{t}[i_] = {X(d)}.{t}[i_] {sym} s_.{t}[i_]; }}")
            return
        if m in ("minps", "maxps", "minpd", "maxpd"):
            t, n = ("f", 4) if m.endswith("ps") else ("d", 2)
            cmp = "<" if m.startswith("min") else ">"
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < {n}; i_++) "
              f"{X(d)}.{t}[i_] = ({X(d)}.{t}[i_] {cmp} s_.{t}[i_]) ? {X(d)}.{t}[i_] : s_.{t}[i_]; }}")
            return
        bitw = {"andps": "&", "andpd": "&", "pand": "&", "orps": "|", "orpd": "|", "por": "|",
                "xorps": "^", "xorpd": "^", "pxor": "^"}
        if m in bitw:
            d, s = ops
            if self.is_xmm(insn, s) and s.reg == d.reg and bitw[m] == "^":
                E(f"{X(d)}.q[0] = 0; {X(d)}.q[1] = 0;")
                return
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; {X(d)}.q[0] {bitw[m]}= s_.q[0]; {X(d)}.q[1] {bitw[m]}= s_.q[1]; }}")
            return
        if m in ("andnps", "andnpd", "pandn"):
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; {X(d)}.q[0] = ~{X(d)}.q[0] & s_.q[0]; "
              f"{X(d)}.q[1] = ~{X(d)}.q[1] & s_.q[1]; }}")
            return
        if m in ("cvtsi2ss", "cvtsi2sd"):
            d, s = ops
            t, ct = ("f", "float") if m.endswith("ss") else ("d", "double")
            E(f"{X(d)}.{t}[0] = ({ct})(int32_t){self.rd(insn, s, 4)};")
            return
        if m in ("cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si"):
            d, s = ops
            t = "f" if "ss2" in m else "d"
            src = f"(double)({self.xsrc(insn, s, 4 if t == 'f' else 8)}).{t}[0]"
            if not m.startswith("cvtt"):
                src = f"rt_rint({src})"
            E(self.wr(insn, d, f"(uint32_t)FTOI32({src})", 4))
            return
        if m == "cvtss2sd":
            d, s = ops
            E(f"{X(d)}.d[0] = (double)({self.xsrc(insn, s, 4)}).f[0];")
            return
        if m == "cvtsd2ss":
            d, s = ops
            E(f"{X(d)}.f[0] = (float)({self.xsrc(insn, s, 8)}).d[0];")
            return
        if m == "cvtdq2ps":
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < 4; i_++) {X(d)}.f[i_] = (float)(int32_t)s_.u[i_]; }}")
            return
        if m == "cvtdq2pd":
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 8)}; {X(d)}.d[0] = (double)(int32_t)s_.u[0]; {X(d)}.d[1] = (double)(int32_t)s_.u[1]; }}")
            return
        if m == "cvtps2pd":
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 8)}; {X(d)}.d[0] = s_.f[0]; {X(d)}.d[1] = s_.f[1]; }}")
            return
        if m == "cvtpd2ps":
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; {X(d)}.f[0] = (float)s_.d[0]; {X(d)}.f[1] = (float)s_.d[1]; {X(d)}.q[1] = 0; }}")
            return
        if m in ("cvttps2dq", "cvtps2dq"):
            d, s = ops
            f = "rt_trunc" if m == "cvttps2dq" else "rt_rint"
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < 4; i_++) {X(d)}.u[i_] = (uint32_t)FTOI32({f}(s_.f[i_])); }}")
            return
        if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
            d, s = ops
            t = "f" if m.endswith("ss") else "d"
            E(f"FCOMI_SSE({X(d)}.{t}[0], ({self.xsrc(insn, s, 4 if t == 'f' else 8)}).{t}[0]);")
            return
        if m.startswith("cmp") and m[-2:] in ("ss", "sd", "ps", "pd"):
            pred = m[3:-2]
            preds = {"eq": "a_ == b_", "lt": "a_ < b_", "le": "a_ <= b_", "unord": "a_ != a_ || b_ != b_",
                     "neq": "!(a_ == b_)", "nlt": "!(a_ < b_)", "nle": "!(a_ <= b_)", "ord": "a_ == a_ && b_ == b_"}
            if pred not in preds:
                raise Unsupported("cmp predicate")
            t = "f" if m[-1] == "s" else "d"
            n = 1 if m[-2] == "s" else (4 if t == "f" else 2)
            q = "u" if t == "f" else "q"
            ty = "float" if t == "f" else "double"
            d, s = ops
            size = (4 if t == "f" else 8) if n == 1 else 16
            all1 = "0xffffffffu" if t == "f" else "~0ull"
            E(f"{{ X128 s_ = {self.xsrc(insn, s, size)}; for (int i_ = 0; i_ < {n}; i_++) {{ {ty} a_ = {X(d)}.{t}[i_], b_ = s_.{t}[i_]; "
              f"{X(d)}.{q}[i_] = ({preds[pred]}) ? {all1} : 0; }} }}")
            return
        if m == "pshufd":
            d, s, im = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; uint32_t k_ = {im.imm & 0xff}; for (int i_ = 0; i_ < 4; i_++) "
              f"{X(d)}.u[i_] = s_.u[(k_ >> (2 * i_)) & 3]; }}")
            return
        if m in ("pshuflw", "pshufhw"):
            d, s, im = ops
            base = 0 if m == "pshuflw" else 4
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; uint32_t k_ = {im.imm & 0xff}; {X(d)} = s_; for (int i_ = 0; i_ < 4; i_++) "
              f"{X(d)}.w[{base} + i_] = s_.w[{base} + ((k_ >> (2 * i_)) & 3)]; }}")
            return
        if m == "shufps":
            d, s, im = ops
            k = im.imm & 0xff
            E(f"{{ X128 a_ = {X(d)}, s_ = {self.xsrc(insn, s, 16)}; {X(d)}.u[0] = a_.u[{k & 3}]; {X(d)}.u[1] = a_.u[{(k >> 2) & 3}]; "
              f"{X(d)}.u[2] = s_.u[{(k >> 4) & 3}]; {X(d)}.u[3] = s_.u[{(k >> 6) & 3}]; }}")
            return
        if m == "shufpd":
            d, s, im = ops
            k = im.imm & 3
            E(f"{{ X128 a_ = {X(d)}, s_ = {self.xsrc(insn, s, 16)}; {X(d)}.q[0] = a_.q[{k & 1}]; {X(d)}.q[1] = s_.q[{k >> 1}]; }}")
            return
        unpck = {"unpcklps": ("u", 4, 0), "punpckldq": ("u", 4, 0), "unpcklpd": ("q", 2, 0), "punpcklqdq": ("q", 2, 0),
                 "unpckhpd": ("q", 2, 1), "punpckhqdq": ("q", 2, 1), "punpckhdq": ("u", 4, 1), "unpckhps": ("u", 4, 1),
                 "punpcklwd": ("w", 8, 0), "punpckhwd": ("w", 8, 1), "punpcklbw": ("b", 16, 0), "punpckhbw": ("b", 16, 1)}
        if m in unpck:
            t, n, hi = unpck[m]
            d, s = ops
            off = n // 2 if hi else 0
            E(f"{{ X128 a_ = {X(d)}, s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < {n // 2}; i_++) "
              f"{{ {X(d)}.{t}[2 * i_] = a_.{t}[{off} + i_]; {X(d)}.{t}[2 * i_ + 1] = s_.{t}[{off} + i_]; }} }}")
            return
        shifts = {"pslld": ("u", 4, 32, "<<"), "psrld": ("u", 4, 32, ">>"), "psllq": ("q", 2, 64, "<<"),
                  "psrlq": ("q", 2, 64, ">>"), "psllw": ("w", 8, 16, "<<"), "psrlw": ("w", 8, 16, ">>")}
        if m in shifts or m in ("psrad", "psraw"):
            d, s = ops
            if s.type == x86.X86_OP_IMM:
                cnt = f"{s.imm & 0xff}u"
            else:
                cnt = f"(uint32_t)({self.xsrc(insn, s, 16)}).q[0]"
            if m in shifts:
                t, n, bits, sym = shifts[m]
                E(f"{{ uint32_t n_ = {cnt}; for (int i_ = 0; i_ < {n}; i_++) {X(d)}.{t}[i_] = n_ >= {bits} ? 0 : "
                  f"{X(d)}.{t}[i_] {sym} n_; }}")
            else:
                t, n, st, bits = ("u", 4, "int32_t", 32) if m == "psrad" else ("w", 8, "int16_t", 16)
                E(f"{{ uint32_t n_ = {cnt}; if (n_ > {bits - 1}) n_ = {bits - 1}; for (int i_ = 0; i_ < {n}; i_++) "
                  f"{X(d)}.{t}[i_] = ({st}){X(d)}.{t}[i_] >> n_; }}")
            return
        if m == "palignr":
            d, s, im = ops
            k = im.imm & 0xff
            E(f"{{ X128 a_ = {X(d)}, s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < 16; i_++) {{ int j_ = i_ + {k}; "
              f"{X(d)}.b[i_] = j_ < 16 ? s_.b[j_] : j_ < 32 ? a_.b[j_ - 16] : 0; }} }}")
            return
        if m in ("psrldq", "pslldq"):
            d, s = ops
            k = s.imm & 0xff
            if m == "psrldq":
                E(f"{{ X128 a_ = {X(d)}; for (int i_ = 0; i_ < 16; i_++) {X(d)}.b[i_] = i_ + {k} < 16 ? a_.b[i_ + {k}] : 0; }}")
            else:
                E(f"{{ X128 a_ = {X(d)}; for (int i_ = 0; i_ < 16; i_++) {X(d)}.b[i_] = i_ >= {k} ? a_.b[i_ - {k}] : 0; }}")
            return
        pcmp = {"pcmpeqb": ("b", 16, "uint8_t", "=="), "pcmpeqw": ("w", 8, "uint16_t", "=="),
                "pcmpeqd": ("u", 4, "uint32_t", "=="), "pcmpgtb": ("b", 16, "int8_t", ">"),
                "pcmpgtw": ("w", 8, "int16_t", ">"), "pcmpgtd": ("u", 4, "int32_t", ">"),
                "pminsd": ("u", 4, "int32_t", "min"), "pmaxsd": ("u", 4, "int32_t", "max"),
                "pminsw": ("w", 8, "int16_t", "min"), "pmaxsw": ("w", 8, "int16_t", "max"),
                "pminub": ("b", 16, "uint8_t", "min"), "pmaxub": ("b", 16, "uint8_t", "max")}
        if m in pcmp:
            t, n, ty, sym = pcmp[m]
            d, s = ops
            if sym in ("min", "max"):
                c = "<" if sym == "min" else ">"
                body = f"{{ {ty} a_ = ({ty}){X(d)}.{t}[i_], b_ = ({ty})s_.{t}[i_]; {X(d)}.{t}[i_] = a_ {c} b_ ? a_ : b_; }}"
            else:
                body = f"{X(d)}.{t}[i_] = (({ty}){X(d)}.{t}[i_] {sym} ({ty})s_.{t}[i_]) ? ({ty})-1 : 0;"
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; for (int i_ = 0; i_ < {n}; i_++) {body} }}")
            return
        if m == "pmovmskb":
            d, s = ops
            E(self.wr(insn, d, f"rt_pmovmskb({X(s)})", 4))
            return
        if m in ("movmskps", "movmskpd"):
            d, s = ops
            if m == "movmskps":
                v = f"(({X(s)}.u[0] >> 31) | (({X(s)}.u[1] >> 31) << 1) | (({X(s)}.u[2] >> 31) << 2) | (({X(s)}.u[3] >> 31) << 3))"
            else:
                v = f"(uint32_t)(({X(s)}.q[0] >> 63) | (({X(s)}.q[1] >> 63) << 1))"
            E(self.wr(insn, d, v, 4))
            return
        if m == "pmuludq":
            d, s = ops
            E(f"{{ X128 s_ = {self.xsrc(insn, s, 16)}; {X(d)}.q[0] = (uint64_t){X(d)}.u[0] * s_.u[0]; "
              f"{X(d)}.q[1] = (uint64_t){X(d)}.u[2] * s_.u[2]; }}")
            return
        if m in ("packuswb", "packsswb"):
            d, s = ops
            fn = "rt_packuswb" if m == "packuswb" else "rt_packsswb"
            E(f"{X(d)} = {fn}({X(d)}, {self.xsrc(insn, s, 16)});")
            return
        if m == "pextrw":
            d, s, im = ops
            E(self.wr(insn, d, f"{X(s)}.w[{im.imm & 7}]", 4))
            return
        if m == "pinsrw":
            d, s, im = ops
            E(f"{X(d)}.w[{im.imm & 7}] = (uint16_t){self.rd(insn, s, 2 if s.type == x86.X86_OP_MEM else 4)};")
            return
        if m == "stmxcsr":
            E(f"WR32({self.ea(insn, ops[0])}, c->mxcsr);")
            return
        if m == "ldmxcsr":
            E(f"c->mxcsr = RD32({self.ea(insn, ops[0])});")
            return
        if m == "movnti":
            E(self.wr(insn, ops[0], self.rd(insn, ops[1])))
            return
        raise Unsupported("insn " + m)


# ---- inlining ----------------------------------------------------------
# Every call costs a full register save and reload through the CPU struct and a C call; the debug STL's
# accessors (one to ten instructions) are called millions of times a second. A call to a small function (no
# jumps out, no jump tables, no hooks, no exception resume points; its own calls inlined again one level down
# when they can be, else ordinary calls) gets the callee's body in
# its place, lifted with the caller's register locals: the return address is still pushed (the callee may
# read its stack), "ret n" pops it and its arguments and jumps to the end, labels get a per-call prefix.
INLINE_MAX = 24
# hot functions (call counts on the credits page and the plant select) allowed up to 40 instructions: the
# debug STL's checked iterator operations, most of whose length is their never-taken error report
INLINE_HOT = {0x7fb3c0, 0x7fbdd0, 0x7fbbc0, 0x7b87c0, 0x7fc570, 0x7f5d90, 0xa2cde0}
INLINE_HOT_MAX = 40
# hooked functions whose whole effect is a few lines of C (host/game_patches.c has the same as hooks, for
# calls that come through rt_call)
_ADOPT = ("{ uint32_t p_ = RD32(esp); WR32(ecx, p_ ? RD32(p_) : 0u); WR32(ecx + 4u, 0u); esp += 4; }")
_ORPHAN = "{ WR32(ecx, 0u); }"
INLINE_C = {0x7615d0: _ADOPT, 0x761590: _ADOPT, 0x761e90: _ORPHAN, 0x761e60: _ORPHAN}
INLINE_DEPTH = 2
_inline_cache = {}


class _NoInline(Exception):
    pass


class InlineLifter(Lifter):
    def goto(self, va):
        if va in self.f.insns:
            return f"goto @P@{va:08x};"
        raise _NoInline()

    def tail(self, va):
        raise _NoInline()

    def call_direct(self, insn, target):
        # one level down: inlined again if it can be, else an ordinary call (it saves and reloads the
        # registers as the caller's own calls do)
        body = inline_body(self.prog, target, self.names, self.depth + 1) if self.depth < INLINE_DEPTH else None
        if body is None:
            return Lifter.call_direct(self, insn, target)
        self.n_sub = getattr(self, "n_sub", 0) + 1
        ret = insn.address + insn.size
        return f"esp -= 4; WR32(esp, IMG({h(ret)})); {{ {body.replace('@P@', f'@P@{self.n_sub}_')} }}"

    def lift(self, insn):
        pfx, m = self.base_mn(insn)
        if m == "ret":
            n = insn.operands[0].imm if insn.operands else 0
            self.emit(f"esp += {4 + n}; goto @P@end;")
            return
        if m == "int3":  # (the debug STL's break after a failed check: rt_trap doesn't return)
            self.emit(f"STORE; rt_trap(c, {h(insn.address)}, \"int3\");")
            return
        if m == "jmp" and insn.operands[0].type != x86.X86_OP_IMM:
            raise _NoInline()
        before = len(self.out)
        super().lift(insn)
        for line in self.out[before:]:
            if "return;" in line or "rt_unimpl" in line:
                raise _NoInline()

    def body(self):
        f = self.f
        order = sorted(f.insns)
        if order[0] != f.entry:
            self.emit(f"goto @P@{f.entry:08x};")
        for k, va in enumerate(order):
            insn = f.insns[va]
            if insn is None:
                raise _NoInline()
            self.out.append(f"@P@{va:08x}:;")
            self.lift(insn)
            nxt = va + insn.size
            if self.falls_through(insn) and (k + 1 >= len(order) or order[k + 1] != nxt):
                self.emit(self.goto(nxt))
        self.out.append("@P@end:;")
        return " ".join(line.strip() for line in self.out)


def inline_body(prog, va, names, depth=0):
    key = (va, depth)
    if key in _inline_cache:
        return _inline_cache[key]
    body = None
    f = prog.funcs.get(va)
    if (f is not None and 0 < len(f.insns) <= (INLINE_HOT_MAX if va in INLINE_HOT else INLINE_MAX) and not f.tables and not getattr(f, "resume", None)
            and not f.tails and not any(a in HOOKS for a in f.insns)):
        try:
            lf = InlineLifter(prog, f, names)
            lf.depth = depth
            body = lf.body()
        except (_NoInline, Unsupported, KeyError):
            body = None
    _inline_cache[key] = body
    return body


def follow_jmp(prog, va):
    """A function that is only "jmp elsewhere" (the exe has many besides its ILT): its target, so the call
    goes straight there (and can be inlined)"""
    for _ in range(4):
        f = prog.funcs.get(va)
        if f is None or len(f.insns) != 1:
            break
        insn = f.insns.get(va)
        if insn is None or insn.mnemonic != "jmp" or insn.operands[0].type != x86.X86_OP_IMM:
            break
        nxt = prog.resolve(insn.operands[0].imm & 0xFFFFFFFF)
        if nxt not in prog.funcs or any(a in HOOKS for a in f.insns):
            break
        va = nxt
    return va
