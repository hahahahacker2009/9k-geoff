#include <u.h>
#include <libc.h>
void
abort(void)
{
	notify(0);		/* in case note handler just continues */
	while(*(int*)0)
		;
}
