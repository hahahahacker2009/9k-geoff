/*
 * Macros for accessing page table entries; change the
 * C-style array-index macros into a page table byte offset
 */
#define PML4O(v)	((PTLX((v), 3))<<3)
#define PDPO(v)		((PTLX((v), 2))<<3)
#define PDO(v)		((PTLX((v), 1))<<3)
#define PTO(v)		((PTLX((v), 0))<<3)

/* trashes CX */
#define BUSY(n, l)	MOVL $(n), CX; l: LOOP l

/* trashes AX, DX and CX */
// #define DBGPUT(c, l) MOVL $0x3F8, DX; MOVL $(c), AX; OUTB; BUSY(33000, l)
#define DBGPUT(c, l)

