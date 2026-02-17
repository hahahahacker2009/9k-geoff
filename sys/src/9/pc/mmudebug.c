#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"io.h"

#define PAGEIS	"page %#.8lux is "
#define PROC	"proc %d (pid %lud) "
#define OTHERS	" but has other refs!"

void
countpagerefs(ulong *ref, int print)
{
	int i, n;
	Mach *mm;
	Page *pg;
	Proc *p;

	n = 0;
	for(i=0; i<conf.nproc; i++){
		p = proctab(i);
		if(p->mmupdb){
			if(print){
				if(ref[pagenumber(p->mmupdb)])
					iprint(PAGEIS PROC "pdb\n",
						p->mmupdb->pa, i, p->pid);
				continue;
			}
			if(ref[pagenumber(p->mmupdb)]++ == 0)
				n++;
			else
				iprint(PAGEIS PROC "pdb" OTHERS "\n",
					p->mmupdb->pa, i, p->pid);
		}
		if(p->kmaptable){
			if(print){
				if(ref[pagenumber(p->kmaptable)])
					iprint(PAGEIS PROC "kmaptable\n",
						p->kmaptable->pa, i, p->pid);
				continue;
			}
			if(ref[pagenumber(p->kmaptable)]++ == 0)
				n++;
			else
				iprint(PAGEIS PROC "kmaptable" OTHERS "\n",
					p->kmaptable->pa, i, p->pid);
		}
		for(pg=p->mmuused; pg; pg=pg->next){
			if(print){
				if(ref[pagenumber(pg)])
					iprint(PAGEIS "on " PROC "mmuused\n",
						pg->pa, i, p->pid);
				continue;
			}
			if(ref[pagenumber(pg)]++ == 0)
				n++;
			else
				iprint(PAGEIS "on " PROC "mmuused" OTHERS "\n",
					pg->pa, i, p->pid);
		}
		for(pg=p->mmufree; pg; pg=pg->next){
			if(print){
				if(ref[pagenumber(pg)])
					iprint(PAGEIS "on " PROC "mmufree\n",
						pg->pa, i, p->pid);
				continue;
			}
			if(ref[pagenumber(pg)]++ == 0)
				n++;
			else
				iprint(PAGEIS "on " PROC "mmufree" OTHERS "\n",
					pg->pa, i, p->pid);
		}
	}
	if(!print)
		iprint("%d pages in proc mmu\n", n);
	n = 0;
	for(i=0; i<conf.nmach; i++){
		mm = MACHP(i);
		for(pg=mm->pdbpool; pg; pg=pg->next){
			if(print){
				if(ref[pagenumber(pg)])
					iprint(PAGEIS "in cpu%d pdbpool\n",
						pg->pa, i);
				continue;
			}
			if(ref[pagenumber(pg)]++ == 0)
				n++;
			else
				iprint(PAGEIS "in cpu%d pdbpool" OTHERS "\n",
					pg->pa, i);
		}
	}
	if(!print){
		iprint("%d pages in mach pdbpools\n", n);
		for(i=0; i<conf.nmach; i++)
			iprint("cpu%d: %d pdballoc, %d pdbfree\n",
				i, MACHP(i)->pdballoc, MACHP(i)->pdbfree);
	}
}
