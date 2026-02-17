/*
 * cidr ip file - does ip match starts of lines of file using cidr notation?
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ip.h>

/*
 * called if a mask isn't specified.  we build a minimal mask
 * instead of using the default mask for that net.
 * in this case we never allow a class A mask (0xff000000).
 */
static void
v4minclassb(uchar *mask, char *ip)
{
	ulong m;
	char *p;

	m = 0xff000000;
	for(p = strchr(ip, '.'); p && p[1]; p = strchr(p+1, '.'))
		m = (m>>8)|0xff000000;

	/* force at least a class B */
	hnputl(mask, m | 0xffff0000);
}

/* parse a CIDR address (cp) and compare to the peer IP addr */
static int
cidrcheck(uchar peerip[], char *cp)
{
	char *p;
	vlong v;
	uchar addr[IPaddrlen], mask[IPaddrlen], peermasked[IPaddrlen];

	/* parse v4 or v6 address and optional mask */
	v = parseip(addr, cp);
	if (v < 0)
		return 0;
	if (v == 6) {			/* v6 doesn't have a parsecidr() */
		p = strchr(cp, '/');
		if (p) {
			v = parseipmask(mask, p);
			if (v < 0)
				return 0;
		} else
			memset(mask, 0, sizeof mask);
	} else {
		uchar addr4[IPv4addrlen], mask4[IPv4addrlen];

		v4parsecidr(addr4, mask4, cp);
		if(strchr(cp, '/') == nil)	/* no mask? */
			v4minclassb(mask4, cp);	/* make one from addr */
		v4tov6(addr, addr4);
		v4tov6(mask, mask4);
	}
	maskip(peerip, mask, peermasked);
	if (equivip6(peermasked, addr))
		return 1;
	return 0;
}

/*
 * process - parse a list of CIDR addresses in cidrfile,
 * comparing each to the peer IP addr
 */
static void
process(char *peer, char *cidrfile)
{
	int n;
	char *cp;
	uchar peerip[IPaddrlen];
	Biobuf *bp;

	if (parseip(peerip, peer) < 0)
		sysfatal("bad ip %s", peer);

	bp = Bopen(cidrfile, OREAD);
	if (bp == nil)
		sysfatal("can't open %s: %r", cidrfile);
	while ((cp = Brdline(bp, '\n')) != nil) {
		n = Blinelen(bp);
		cp[n-1] = 0;
		if (*cp && cidrcheck(peerip, cp)) {
			print("%s matches %s\n", peer, cp);
			exits(0);
		}
	}
	Bterm(bp);
	exits("no match");
}

static void
usage(void)
{
	fprint(2, "usage: %s ip file...\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	char *peer, *cidrfile;

	ARGBEGIN {
	default:
		usage();
		break;
	} ARGEND

	if (argc != 2)
		usage();

	peer = argv[0];
	cidrfile = argv[1];
	process(peer, cidrfile);
	exits(0);
}
