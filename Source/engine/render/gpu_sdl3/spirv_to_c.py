#!/usr/bin/env python3
"""Convert two SPIRV binaries to a C header + source pair.

Usage: spirv_to_c.py <vert.spv> <frag.spv> <output_base> <vert_symbol> <frag_symbol>
Emits `<output_base>.h` declaring both arrays + lengths, and
`<output_base>.cpp` defining them.
"""

import sys
from pathlib import Path


def _c_array(data: bytes, name: str) -> str:
    hex_bytes = ", ".join(f"0x{b:02x}" for b in data)
    return f"extern const unsigned char {name}[] = {{ {hex_bytes} }};\nextern const unsigned int {name}Len = {len(data)};\n"


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__, file=sys.stderr)
        return 1
    vert_path = Path(sys.argv[1])
    frag_path = Path(sys.argv[2])
    out_base = Path(sys.argv[3])
    vert_symbol = sys.argv[4]
    frag_symbol = sys.argv[5]
    vert_data = vert_path.read_bytes()
    frag_data = frag_path.read_bytes()
    header = (
        "// Auto-generated. Do not edit.\n"
        "#pragma once\n"
        "#include <cstdint>\n\n"
        + _c_array(vert_data, vert_symbol)
        + _c_array(frag_data, frag_symbol)
    )
    source = (
        "// Auto-generated. Do not edit.\n"
        f'#include "{out_base.name}.h"\n'
    )
    out_base.with_suffix(".h").write_text(header)
    out_base.with_suffix(".cpp").write_text(source)
    return 0


if __name__ == "__main__":
    sys.exit(main())