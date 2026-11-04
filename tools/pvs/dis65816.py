"""Minimal 65816 disassembler for reading the packs by hand.

Tracks the m/x widths through REP/SEP in straight-line code. It does not follow
control flow, so give it a real entry point.
"""
_TABLE = (
    "BRK s1,ORA dxi,COP s1,ORA sr,TSB d,ORA d,ASL d,ORA dil,PHP i,ORA imm,ASL i,PHD i,TSB a,ORA a,ASL a,ORA al,"
    "BPL r,ORA diy,ORA di,ORA siy,TRB d,ORA dx,ASL dx,ORA dily,CLC i,ORA ay,INC i,TCS i,TRB a,ORA ax,ASL ax,ORA alx,"
    "JSR a,AND dxi,JSL al,AND sr,BIT d,AND d,ROL d,AND dil,PLP i,AND imm,ROL i,PLD i,BIT a,AND a,ROL a,AND al,"
    "BMI r,AND diy,AND di,AND siy,BIT dx,AND dx,ROL dx,AND dily,SEC i,AND ay,DEC i,TSC i,BIT ax,AND ax,ROL ax,AND alx,"
    "RTI i,EOR dxi,WDM s1,EOR sr,MVP bm,EOR d,LSR d,EOR dil,PHA i,EOR imm,LSR i,PHK i,JMP a,EOR a,LSR a,EOR al,"
    "BVC r,EOR diy,EOR di,EOR siy,MVN bm,EOR dx,LSR dx,EOR dily,CLI i,EOR ay,PHY i,TCD i,JML al,EOR ax,LSR ax,EOR alx,"
    "RTS i,ADC dxi,PER rl,ADC sr,STZ d,ADC d,ROR d,ADC dil,PLA i,ADC imm,ROR i,RTL i,JMP ai,ADC a,ROR a,ADC al,"
    "BVS r,ADC diy,ADC di,ADC siy,STZ dx,ADC dx,ROR dx,ADC dily,SEI i,ADC ay,PLY i,TDC i,JMP axi,ADC ax,ROR ax,ADC alx,"
    "BRA r,STA dxi,BRL rl,STA sr,STY d,STA d,STX d,STA dil,DEY i,BIT imm,TXA i,PHB i,STY a,STA a,STX a,STA al,"
    "BCC r,STA diy,STA di,STA siy,STY dx,STA dx,STX dy,STA dily,TYA i,STA ay,TXS i,TXY i,STZ a,STA ax,STZ ax,STA alx,"
    "LDY immx,LDA dxi,LDX immx,LDA sr,LDY d,LDA d,LDX d,LDA dil,TAY i,LDA imm,TAX i,PLB i,LDY a,LDA a,LDX a,LDA al,"
    "BCS r,LDA diy,LDA di,LDA siy,LDY dx,LDA dx,LDX dy,LDA dily,CLV i,LDA ay,TSX i,TYX i,LDY ax,LDA ax,LDX ay,LDA alx,"
    "CPY immx,CMP dxi,REP s1,CMP sr,CPY d,CMP d,DEC d,CMP dil,INY i,CMP imm,DEX i,WAI i,CPY a,CMP a,DEC a,CMP al,"
    "BNE r,CMP diy,CMP di,CMP siy,PEI d,CMP dx,DEC dx,CMP dily,CLD i,CMP ay,PHX i,STP i,JML ail,CMP ax,DEC ax,CMP alx,"
    "CPX immx,SBC dxi,SEP s1,SBC sr,CPX d,SBC d,INC d,SBC dil,INX i,SBC imm,NOP i,XBA i,CPX a,SBC a,INC a,SBC al,"
    "BEQ r,SBC diy,SBC di,SBC siy,PEA a,SBC dx,INC dx,SBC dily,SED i,SBC ay,PLX i,XCE i,JSR axi,SBC ax,INC ax,SBC alx"
)
OPS = [tuple(entry.split()) for entry in _TABLE.split(",")]
_SIZES = {"i": 1, "s1": 2, "d": 2, "dx": 2, "dy": 2, "dxi": 2, "diy": 2, "di": 2, "dil": 2,
          "dily": 2, "sr": 2, "siy": 2, "r": 2, "a": 3, "ax": 3, "ay": 3, "ai": 3, "axi": 3,
          "ail": 3, "rl": 3, "bm": 3, "al": 4, "alx": 4}
_FORMATS = {"d": "{}", "dx": "{},x", "dy": "{},y", "dxi": "({},x)", "diy": "({}),y", "di": "({})",
            "dil": "[{}]", "dily": "[{}],y", "sr": "{},s", "siy": "({},s),y", "a": "{}", "ax": "{},x",
            "ay": "{},y", "ai": "({})", "axi": "({},x)", "ail": "[{}]", "al": "{}", "alx": "{},x",
            "bm": "{}"}


def decode(data, off, m=0, x=0):
    """Return (mnemonic, mode, length, operand, new_m, new_x) at file offset off."""
    op = data[off]
    mn, mode = OPS[op]
    if mode == "imm":
        n = 2 if m else 3
    elif mode == "immx":
        n = 2 if x else 3
    else:
        n = _SIZES[mode]
    arg = int.from_bytes(data[off + 1:off + n], "little")
    if op == 0xC2:
        m = 0 if arg & 0x20 else m
        x = 0 if arg & 0x10 else x
    elif op == 0xE2:
        m = 1 if arg & 0x20 else m
        x = 1 if arg & 0x10 else x
    return mn, mode, n, arg, m, x


def format_operand(mode, n, arg, pc):
    if mode == "i":
        return ""
    if mode == "r":
        return "$%04X" % ((pc + 2 + ((arg ^ 0x80) - 0x80)) & 0xFFFF)
    if mode == "rl":
        return "$%04X" % ((pc + 3 + ((arg ^ 0x8000) - 0x8000)) & 0xFFFF)
    if mode in ("imm", "immx", "s1"):
        return "#$%0*X" % ((n - 1) * 2, arg)
    return _FORMATS[mode].format("$%0*X" % ((n - 1) * 2, arg))


def disassemble(data, off, count=64, m=0, x=0, stop_at_return=True):
    """Yield text lines. off is a HiROM file offset; bank is shown as $C0+."""
    for _ in range(count):
        if off >= len(data):
            return
        mn, mode, n, arg, m, x = decode(data, off, m, x)
        pc = off & 0xFFFF
        raw = data[off:off + n].hex().upper()
        yield "C%X:%04X  %-8s  %s %s" % (off >> 16, pc, raw, mn, format_operand(mode, n, arg, pc))
        off += n
        if stop_at_return and mn in ("RTS", "RTL", "RTI", "STP"):
            return
