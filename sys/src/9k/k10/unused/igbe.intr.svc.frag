static ulong
svcintrs(ulong icr, ulong im, ulong intrs, uint *imp, Rendez *rend, uint *intcnt)
{
	ulong imbits;

	imbits = icr & intrs;
	if(imbits){
		*imp = imbits;
		wakeup(rend);
		im &= ~intrs;
		++*intcnt;
	}
	return im;
}

