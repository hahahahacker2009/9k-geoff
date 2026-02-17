/* muldiv using only integers; shouldn't trap */
#include <u.h>
#include <libc.h>

ulong
umuldiv(ulong a, ulong b, ulong c)
{
	return (uvlong)a * (uvlong)b / c;
}

long
muldiv(long a, long b, long c)
{
	return (vlong)a * (vlong)b / c;
}
