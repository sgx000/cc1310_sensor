#!/usr/bin/env python3
"""
bin2hex.py - konwersja ELF (.out) CC1310 do Intel HEX z rekordami type-04
(Extended Linear Address).

Dlaczego: arm-none-eabi-objcopy -O ihex generuje rekordy type-02 (Extended
Segment Address) dla adresow > 64 KB. UniFlash dla CC1310 poprawnie
interpretuje TYLKO type-04, przez co CCFG na 0x1FFA8 nie zostaje zapisany
i chip nie startuje. Ten skrypt obchodzi problem: robi objcopy -O binary
(pelny obraz z gap-fill 0xFF), a potem sam sklada Intel HEX z type-04.

Uzycie:
    python3 bin2hex.py <objcopy> <plik.out> <wynik.hex>

Przyklad:
    python3 bin2hex.py \\
        ~/arm-gnu-toolchain-12.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-objcopy \\
        rfEasyLinkRx.out gateway.hex
"""
import sys, struct, subprocess, tempfile, os

def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    objcopy, elf, out_hex = sys.argv[1], sys.argv[2], sys.argv[3]
    readelf = objcopy.replace("objcopy", "readelf")

    # 1. Pelny obraz binarny (0x00000..0x1FFFF), dziury wypelnione 0xFF.
    tmpbin = tempfile.mktemp(suffix=".bin")
    subprocess.check_call([objcopy, "-O", "binary",
                           "--gap-fill=0xff", "--pad-to=0x20000", elf, tmpbin])
    data = open(tmpbin, "rb").read()
    os.unlink(tmpbin)

    # 2. Entry point z ELF (do rekordu type-05).
    txt = subprocess.check_output([readelf, "-h", elf]).decode()
    ep = int(txt.split("Entry point address:")[1].split()[0], 16)

    def rec(payload, addr_lo, rectype):
        n = len(payload)
        chk = n + ((addr_lo >> 8) & 0xFF) + (addr_lo & 0xFF) + rectype + sum(payload)
        chk = (-chk) & 0xFF
        return ":%02X%04X%02X%s%02X" % (
            n, addr_lo, rectype, "".join("%02X" % b for b in payload), chk)

    with open(out_hex, "w") as f:
        cur_upper = -1
        addr = 0
        while addr < len(data):
            chunk = data[addr:addr+16]
            # pomijamy wyrownane do 16B bloki samych 0xFF (puste sektory)
            if all(b == 0xFF for b in chunk):
                addr += 16
                continue
            upper = (addr >> 16) & 0xFFFF
            if upper != cur_upper:
                f.write(rec(list(struct.pack(">H", upper)), 0, 4) + "\n")  # type-04
                cur_upper = upper
            f.write(rec(list(chunk), addr & 0xFFFF, 0) + "\n")             # type-00
            addr += 16
        f.write(rec(list(struct.pack(">I", ep)), 0, 5) + "\n")            # type-05 start addr
        f.write(":00000001FF\n")                                          # EOF
    print("OK ->", out_hex, "(entry=0x%X)" % ep)

if __name__ == "__main__":
    main()
