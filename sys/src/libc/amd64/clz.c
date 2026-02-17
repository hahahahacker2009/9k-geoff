/* count leading zero bits */
#include <u.h>
#include <libc.h>

enum {
	Clzbits = 8 * sizeof(uvlong),
};

int	_bsr(uvlong n);

int
clz(uvlong n)
{
	if (n == 0)
		return Clzbits;
	return Clzbits - 1 - _bsr(n);
}
