"""PE image facts the recompiler needs: sections, relocations, imports and
the incremental-link thunk table (ILT)."""
import bisect
import struct

import pefile


class Image:
    def __init__(self, path):
        self.pe = pe = pefile.PE(path)
        oh = pe.OPTIONAL_HEADER
        self.base = oh.ImageBase
        self.entry = self.base + oh.AddressOfEntryPoint
        self.size = oh.SizeOfImage
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode(errors="replace")
            start = self.base + s.VirtualAddress
            self.sections.append((name, start, start + max(s.Misc_VirtualSize, s.SizeOfRawData), s))
        self.mem = bytearray(self.size)
        self.mem[0:len(pe.header)] = pe.header
        for _, start, _, s in self.sections:
            raw = s.get_data()[: s.Misc_VirtualSize or s.SizeOfRawData]
            off = start - self.base
            self.mem[off:off + len(raw)] = raw
        text = self.section(".text")
        self.text_start, self.text_end = text[1], text[2]

        # Every 32-bit absolute address in the image (IMAGE_REL_BASED_HIGHLOW)
        self.relocs = set()
        for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in block.entries:
                if e.type == 3:
                    self.relocs.add(e.rva + self.base)
        self.reloc_list = sorted(self.relocs)

        # IAT slot VA -> (dll, name)
        self.imports = {}
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            dll = d.dll.decode().lower()
            for i in d.imports:
                name = i.name.decode() if i.name else f"#{i.ordinal}"
                self.imports[i.address] = (dll, name)

        self.ilt = self._parse_ilt()

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise KeyError(name)

    def section_of(self, va):
        for s in self.sections:
            if s[1] <= va < s[2]:
                return s[0]
        return None

    def in_text(self, va):
        return self.text_start <= va < self.text_end

    def u8(self, va):
        return self.mem[va - self.base]

    def u32(self, va):
        return struct.unpack_from("<I", self.mem, va - self.base)[0]

    def bytes_at(self, va, n):
        o = va - self.base
        return bytes(self.mem[o:o + n])

    def relocs_in(self, lo, hi):
        i = bisect.bisect_left(self.reloc_list, lo)
        out = []
        while i < len(self.reloc_list) and self.reloc_list[i] < hi:
            out.append(self.reloc_list[i])
            i += 1
        return out

    def _parse_ilt(self):
        """/INCREMENTAL puts a table of `jmp rel32` thunks at the start of
        .text; every function address the program takes points at one."""
        thunks = {}
        va = self.text_start
        while va < self.text_end:
            b = self.u8(va)
            if b == 0xCC:
                va += 1
                continue
            if b != 0xE9:
                break
            rel = struct.unpack_from("<i", self.mem, va + 1 - self.base)[0]
            thunks[va] = (va + 5 + rel) & 0xFFFFFFFF
            va += 5
        self.ilt_end = va
        return thunks
