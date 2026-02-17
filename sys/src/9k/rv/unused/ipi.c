/*
 * process messages to other cpus with notifications by ipi or clock
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "riscv64.h"

typedef struct Ipireq Ipireq;
struct Ipireq {
	Ipireq	*next;
	void	(*dowork)(void *);
	void	*arg;
	short	cpusent;
	short	cpusleft;
};

static Ipireq *ipimsgs;
static Lock ipimsglock;

void
qipimsg(void *workfn, void *arg)
{
	int machno;
	Ipireq *req;
	Mach *mp;

	ilock(&ipimsglock);
	req = malloc(sizeof *req);
	req->dowork = workfn;
	req->arg = arg;
	req->cpusent = m->machno;
	req->cpusleft = sys->nonline - 1;
	req->next = ipimsgs;	/* insert at head, order shouldn't matter */
	ipimsgs = req;
	iunlock(&ipimsglock);

	for (machno = 0; machno < sys->nonline; machno++)
		if (machno != m->machno) {
			mp = sys->machptr[machno];
			if (mp)
				ipitohart(mp->hartid);
		}
}

void
procipimsgs(void)
{
	Ipireq *req, *next;
	Ipireq **prevp;

	if (ipimsgs == nil)
		return;
	ilock(&ipimsglock);
	prevp = &ipimsgs;
	for (req = ipimsgs; req != nil; req = next) {
		next = req->next;
		if (req->cpusent == m->machno)
			continue;
		(*req->dowork)(req->arg);
		if (--req->cpusleft <= 0) {  /* all cpus have done the work? */
			*prevp = next;
			free(req);
		} else
			prevp = &req->next;
	}
	iunlock(&ipimsglock);
}
