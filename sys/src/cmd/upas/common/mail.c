#include "common.h"

/* format of REMOTE FROM lines */
char *REMFROMRE =
	"^>?From[ \t]+((\".*\")?[^\" \t]+?(\".*\")?[^\" \t]+?)[ \t]+(.+)[ \t]+remote[ \t]+from[ \t]+(.*)\n$";
int REMSENDERMATCH = 1;
int REMDATEMATCH = 4;
int REMSYSMATCH = 5;

/* format of LOCAL FROM lines */
char *FROMRE =
	"^>?From[ \t]+((\".*\")?[^\" \t]+?(\".*\")?[^\" \t]+?)[ \t]+(.+)\n$";
int SENDERMATCH = 1;
int DATEMATCH = 4;

/* output a unix style local header */
int
print_header(Biobuf *fp, char *sender, char *date)
{
	return Bprint(fp, "From %s %s\n", sender, date);
}

/* output a unix style remote header */
int
print_remote_header(Biobuf *fp, char *sender, char *date, char *system)
{
	return Bprint(fp, "From %s %s remote from %s\n", sender, date, system);
}

/* parse a mailbox style header */
int
parse_header(char *line, String *sender, String *date)
{
	if (!IS_HEADER(line))
		return -1;
	line += sizeof("From ") - 1;
	s_restart(sender);
	while(*line==' '||*line=='\t')
		line++;
	if(*line == '"'){
		s_putc(sender, *line++);
		while(*line && *line != '"')
			s_putc(sender, *line++);
		s_putc(sender, *line++);
	} else {
		while(*line && *line != ' ' && *line != '\t')
			s_putc(sender, *line++);
	}
	s_terminate(sender);
	s_restart(date);
	while(*line==' '||*line=='\t')
		line++;
	while(*line)
		s_putc(date, *line++);
	s_terminate(date);
	return 0;
}

int
Bnonulwrite(Biobufhdr *bp, char *addr, long nbytes)
{
	char *nulp;

	while (nbytes > 0) {
		nulp = memchr(addr, '\0', nbytes);
		if (nulp == nil)		/* free of NULs? */
			return Bwrite(bp, addr, nbytes);
		if (nulp > addr && Bwrite(bp, addr, nulp - addr) < 0)
			return Beof;
		nulp++;				/* skip the NUL */
		nbytes -= nulp - addr;
		addr = nulp;
	}
	return 0;
}

int
nonulwrite(int fd, char *addr, long nbytes)
{
	int wrbytes, r;
	char *nulp;

	wrbytes = 0;
	while (nbytes > 0) {
		nulp = memchr(addr, '\0', nbytes);
		if (nulp == nil) {		/* free of NULs? */
			r = write(fd, addr, nbytes);
			if (r < 0)
				return r;
			wrbytes += r;
			return wrbytes;
		}
		r = 0;
		if (nulp > addr && (r = write(fd, addr, nulp - addr)) < 0)
			return -1;
		wrbytes += r + 1;		/* pretend we wrote the NUL */
		nulp++;				/* skip the NUL */
		nbytes -= nulp - addr;
		addr = nulp;
	}
	return wrbytes;
}
