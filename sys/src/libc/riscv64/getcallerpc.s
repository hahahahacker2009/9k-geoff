#define RARG	R8

TEXT	getcallerpc(SB), 1, $0
	MOV	R1, RARG
	RET
