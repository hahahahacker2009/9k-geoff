/*
 * uncached memory access (null version)
 */

enum {
	Cached	= 0,
	Uncached = 0,
};

#define UNCACHED(p)	(p)
#define CACHED(p)	(p)
#define ISCACHED(p)	1
