/* count leading zeroes - Zbb extension, as found in sifive u74 */
/* changing 0x13 to 0x1b produces CLZW */
#define CLZ(rs1, rd) \
	WORD $(0x30<<25 | 0<<20 | (rs1)<<15 | 1<<12 | (rd)<<7 | 0x13)

ARG=8

TEXT _clzzbb(SB), 1, $-4			/* int _clzzbb(uvlong) */
	CLZ(ARG, ARG)
	RET
