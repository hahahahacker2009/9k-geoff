#include "rc.h"
#include "exec.h"
#include "io.h"
#include "fns.h"

struct here *here, **ehere;
static int ser = 0;

void psubst(io*, uchar*);
void pstrs(io*, word*);

tree*
heredoc(tree *tag)
{
	struct here *h = new(struct here);
	io *heref;

	if(tag->type != WORD)
		yyerror("Bad here tag");
	h->next = 0;
	if(here)
		*ehere = h;
	else
		here = h;
	ehere = &h->next;
	h->tag = tag;

	heref = openstr();
	pfmt(heref, "/tmp/here%d.%d", getpid(), ser++);
	h->name = strdup((char *)heref->strp);
	closeio(heref);
	return token(h->name, WORD);	/* duplicates h->name for new tree */
}

/*
 * bug: lines longer than NLINE get split -- this can cause spurious
 * missubstitution, or a misrecognized EOF marker.
 */
#define	NLINE	4096

void
readhere(void)
{
	int fd, subst;
	Rune c;
	char *s, *tag;
	char line[NLINE+1];
	io *f;
	struct here *h, *nexth;

	for(h = here; h; h = nexth){
		subst = !h->tag->quoted;
		tag = h->tag->str;
		fd = Creat(h->name);
		if(fd < 0)
			yyerror("can't create here document");
		f = openfd(fd);
		s = line;
		pprompt();
		while((c = rchr(runq->cmdfd)) != EOF){
			if(c == '\n' || s == &line[NLINE]){
				*s = '\0';
				if(tag && strcmp(line, tag) == 0)
					break;
				if(subst)
					psubst(f, (uchar *)line);
				else
					pstr(f, line);
				s = line;
				if(c == '\n'){
					pprompt();
					pchr(f, c);
				}else
					*s++ = c;
			}else
				*s++ = c;
		}
		flush(f);
		closeio(f);
		cleanhere(h->name);
		nexth = h->next;
		efree((char *)h);
	}
	here = 0;
	doprompt = 1;
}

void
psubst(io *f, uchar *s)
{
	int savec, n;
	uchar *t, *u;
	Rune r;
	word *star;

	while(*s){
		if(*s != '$'){		/* copy plain text rune */
			if(*s < Runeself)
				pchr(f, *s++);
			else{
				n = chartorune(&r, (char *)s);
				while(n-- > 0)
					pchr(f, *s++);
			}
		}else{			/* $something -- perform substitution */
			t = ++s;
			if(*t == '$')
				pchr(f, *t++);
			else{
				while(*t && idchr(*t))
					t++;
				savec = *t;
				*t = '\0';
				n = 0;
				for(u = s; *u && '0' <= *u && *u <= '9'; u++)
					n = n*10 + *u - '0';
				if(n && *u == '\0'){
					star = vlook("*")->val;
					if(star && 1 <= n && n <= count(star)){
						while(--n)
							star = star->next;
						pstr(f, star->word);
					}
				}else
					pstrs(f, vlook((char *)s)->val);
				*t = savec;
				if(savec == '^')
					t++;
			}
			s = t;
		}
	}
}

void
pstrs(io *f, word *a)
{
	if(a){
		while(a->next && a->next->word){
			pstr(f, a->word);
			pchr(f, ' ');
			a = a->next;
		}
		pstr(f, a->word);
	}
}
