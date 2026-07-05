#!/usr/bin/env python3
"""V11-AL — fix off-by-one TAC5212_MAX_REG 0x7E -> 0x7F.

Le BQ12 (ADC et DAC) s'étend jusqu'au registre 0x7F de sa page ; avec
max_register=0x7E le regmap rejette l'écriture du dernier octet -> EIO
(constaté board : cset 'TAC0 DAC BQ12 Coefs' = Input/output error).
Les pages TAC5212 vont de 0x00 à 0x7F (datasheet). Idempotent.
"""
import sys

OLD = "#define TAC5212_MAX_REG\t\t\t0x7E"
NEW = "#define TAC5212_MAX_REG\t\t\t0x7F"


def main(src_path):
    path = src_path + "/sound/soc/codecs/tac5212.h"
    with open(path) as f:
        src = f.read()
    if NEW in src:
        print("tac5212-bq12-maxreg: deja applique")
        return
    if src.count(OLD) != 1:
        sys.exit("tac5212-bq12-maxreg: ancre introuvable ou multiple")
    with open(path, "w") as f:
        f.write(src.replace(OLD, NEW))
    print("tac5212-bq12-maxreg: MAX_REG 0x7E -> 0x7F")


if __name__ == "__main__":
    main(sys.argv[1])
