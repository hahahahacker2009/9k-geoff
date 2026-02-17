#include <u.h>
#include <libc.h>

typedef struct {
	vlong	q;
	vlong	r;
} Lldiv;

Lldiv
lldiv(vlong a, vlong b)
{
	vlong q, r;

	q = a / b;
	r = a % b;
	return (Lldiv){ q, r };
}
