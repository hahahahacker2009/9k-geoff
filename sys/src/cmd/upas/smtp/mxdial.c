/*
 * mxdial - find mail exchangers (MXs)
 *
 * makes heavy use of cs and dns.
 */
#include "common.h"
#include <smtp.h>	/* to publish dial_string_parse */

int	alarmscale;			/* from smtp.c */

char	*bustedmxs[Maxbustedmx];

int nmx;
Mx mx[Nmx];			/* list of mxs for current rcvr domain */

static char dnsname[NETPATHLEN + 10];

static int	compar(void*, void*);
static void	expand_meta(DS *ds);
static int	mxlookup1(DS*, char*);

static int
busted(char *mx)
{
	char **bmp;

	for (bmp = bustedmxs; *bmp != nil; bmp++)
		if (strcmp(mx, *bmp) == 0)
			return 1;
	return 0;
}

static int
mxtimeout(void*, char *msg)
{
	if(strstr(msg, "alarm"))
		return 1;
	return 0;
}

long
timedwrite(int fd, void *buf, long len, long ms)
{
	long n, oalarm;

	atnotify(mxtimeout, 1);
	oalarm = alarm(ms);
	n = write(fd, buf, len);
	alarm(oalarm);
	atnotify(mxtimeout, 0);
	return n;
}

static int
isloopback(char *ip)
{
	return strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0;
}

/* Giveup overrides all others */
void
setmxsts(Mx *mxp, char *sts)
{
	if (sts == nil || sts[0] == '\0' || mxp->sts == Giveup)
		return;
	if (mxp->sts == Retry && sts == Giveup)
		mxp->sts = Giveup;
	else if (mxp->sts == nil)
		mxp->sts = sts;
}

/*
 * refuse to honor loopback addresses given by dns.
 */
char *
ismxok(Mx *mxp)
{
	if(isloopback(mxp->host)){
		if(debug)
			fprint(2, "mxlookup returned loopback\n");
		werrstr("domain lists %s as mail server", mxp->host);
		return Giveup;
	} else if (busted(mxp->host)) {
		if (debug)
			fprint(2, "mxdial skipping busted mx %s\n", mxp->host);
		return Retry;
	} else
		return nil;
}

/*
 *  call the dns process and have it try to resolve the mx request
 *
 *  this routine knows about the firewall and tries inside and outside
 *  dns's seperately.
 */
int
mxlookup(DS *ds, char *domain)
{
	int n, na;

	/* just in case we find no domain name */
	strcpy(domain, ds->host);	/* assumes domain is Maxdomain bytes */

	if(ds->netdir)
		n = mxlookup1(ds, domain);
	else {
		ds->netdir = "/net";
		n = mxlookup1(ds, domain);
		if(n <= 0) {
			ds->netdir = "/net.alt";
			na = mxlookup1(ds, domain);
			if (na >= 0)
				n = na;
		}
	}
	return n;
}

static int
askdnsmx(int dns, char *host, char *domain)
{
	int n;
	char buf[Maxdomain];
	char *fields[4];
	Mx *mxp;

	snprint(buf, sizeof buf, "%s mx", host);
	if(debug)
		fprint(2, "sending %s '%s'\n", dnsname, buf);
	/*
	 * don't hang indefinitely in the write to /net/dns.
	 */
	seek(dns, 0, 0);
	n = timedwrite(dns, buf, strlen(buf), 6*alarmscale);
	if(n < 0){
		rerrstr(buf, sizeof buf);
		if(debug)
			fprint(2, "dns: %s\n", buf);
		if(strstr(buf, "dns failure"))
			/* if dns fails for the mx lookup, we have to stop */
			return -1;
		return 0;
	}

	/*
	 *  parse any mx entries
	 *  assumes one record per read from dns
	 *  resulting mx list will be only host names, not proto nor netdir.
	 */
	seek(dns, 0, 0);
	nmx = 0;
	while(nmx < Nmx && (n = read(dns, buf, sizeof buf-1)) > 0){
		mxp = &mx[nmx];
		buf[n] = 0;
		if(debug)
			fprint(2, "dns mx: %s\n", buf);
		n = getfields(buf, fields, 4, 1, " \t");
		if(n < 4)
			continue;
		/*
		 * reject obvious attempts at loopback.  more devious ones will
		 * be caught when we connect.
		 */
		if (cistrcmp(fields[3], "localhost") == 0 ||
		    strcmp(fields[3], "127.0.0.1") == 0 ||
		    strcmp(fields[3], "::1") == 0)
			continue;

		if(strchr(domain, '.') == 0)
			strcpy(domain, fields[0]);

		strncpy(mxp->host, fields[3], sizeof mxp->host - 1);
		mxp->host[sizeof mxp->host - 1] = '\0';
		mxp->pref = atoi(fields[2]);
		mxp->sts = nil;
		nmx++;
	}
	if(debug)
		fprint(2, "dns mx: got %d mx servers\n", nmx);
	return nmx;
}

static int
mxlookup1(DS *ds, char *domain)
{
	int dns;

	nmx = 0;
	werrstr("");
	snprint(dnsname, sizeof dnsname, "%s/dns", ds->netdir);
	dns = open(dnsname, ORDWR);
	if(dns < 0)
		return -1;
	askdnsmx(dns, ds->host, domain);
	close(dns);
	if (nmx < 0)
		return -1;

	/*
	 * no mx record? try name itself.
	 *
	 * BUG? If domain has no dots, then we used to look up ds->host
	 * but return domain instead of ds->host in the list.  Now we return
	 * ds->host.  What will this break?
	 */
	if(nmx == 0){
		mx[0].pref = 1;
		strncpy(mx[0].host, ds->host, sizeof(mx[0].host));
		nmx++;
	}
	return nmx;
}

int
mxcompar(void *a, void *b)
{
	return ((Mx*)a)->pref - ((Mx*)b)->pref;
}

/* break up a network address into its component parts */
void
dial_string_parse(char *str, DS *ds)
{
	char *p, *p2;

	strncpy(ds->buf, str, sizeof(ds->buf));
	ds->buf[sizeof(ds->buf)-1] = 0;

	p = strchr(ds->buf, '!');
	if(p == 0) {
		ds->netdir = 0;
		ds->proto = "net";
		ds->host = ds->buf;
	} else {
		if(*ds->buf != '/'){
			ds->netdir = 0;
			ds->proto = ds->buf;
		} else {
			ds->netdir = ds->buf;
			for(p2 = p; *p2 != '/'; p2--)
				;
			*p2++ = 0;  /* chop "/net.alt/tcp!host" before "tcp" */
			ds->proto = p2;
		}
		*p = 0;			/* step on "!" after "tcp" */
		ds->host = p + 1;
	}
	ds->service = strchr(ds->host, '!');
	if(ds->service)
		*ds->service++ = 0;
	if(*ds->host == '$')
		expand_meta(ds);
}

static void
expand_meta(DS *ds)
{
	char buf[128], cs[128], *net, *p;
	int fd, n;

	net = ds->netdir;
	if(!net)
		net = "/net";

	if(debug)
		fprint(2, "expanding %s!%s\n", net, ds->host);
	snprint(cs, sizeof(cs), "%s/cs", net);
	if((fd = open(cs, ORDWR)) == -1){
		if(debug)
			fprint(2, "open %s: %r\n", cs);
		syslog(0, "smtp", "cannot open %s: %r", cs);
		return;
	}

	snprint(buf, sizeof buf, "!ipinfo %s", ds->host+1);	// +1 to skip $
	if(write(fd, buf, strlen(buf)) <= 0){
		if(debug)
			fprint(2, "write %s: %r\n", cs);
		syslog(0, "smtp", "%s to %s - write failed: %r", buf, cs);
		close(fd);
		return;
	}

	seek(fd, 0, 0);
	if((n = read(fd, ds->expand, sizeof(ds->expand)-1)) < 0){
		if(debug)
			fprint(2, "read %s: %r\n", cs);
		syslog(0, "smtp", "%s - read failed: %r", cs);
		close(fd);
		return;
	}
	close(fd);

	ds->expand[n] = 0;
	if((p = strchr(ds->expand, '=')) == nil){
		if(debug)
			fprint(2, "response %s: %s\n", cs, ds->expand);
		syslog(0, "smtp", "%q from %s - bad response: %r", ds->expand, cs);
		return;
	}
	ds->host = p+1;

	/* take only first one returned (quasi-bug) */
	if((p = strchr(ds->host, ' ')) != nil)
		*p = 0;
}
