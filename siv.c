/*
 * siv
 *
 * multi-layer regular expression matching
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <utf.h>
#include <fmt.h>
#include <regexp9.h>
#include <bio.h>

#define USAGE "usage: %s [-rlh] [-e expression] [-t [0-9]] [expression] [files...]\n"
#define REMAX 10
#define STATIC_DEPTH 32
#define DYNAMIC_DEPTH 128

extern size_t Bgetre(Biobuf *bp, Reprog *progp, Resub *mp, int msize, char **wp, size_t *wsize);
extern int strgetre(char *str, Reprog *progp, Resub *mp, int msize);

typedef struct {
	char *cp;
	DIR *dp;
} Rframe; /* recursion stack frame */

char *name;
char path[PATH_MAX + 1];
unsigned char *inputbuf;
Biobuf inb, outb;
Reprog *progarr[REMAX];
Rframe sstack[STATIC_DEPTH];
Rframe *dstack;
Rframe *stack;

/* siv stuff */
char *wp;
size_t wsize;
int depth;

struct dirent *ent;
struct stat buf;
int d; /* depth in directory recursion stack */
int fd;
int t; /* target index TODO: change name */
int n; /* number of expressions */
int recur;
int locat;

char*
escape(char *s)
{
	int i, j;
	for(i = 0, j = 0; s[i]; ++i, ++j) {
		if(s[i] != '\\') {
			s[j] = s[i];
			continue;
		}

		switch(s[i + 1]) {
			case 'a':
				s[j] = '\a';
				++i;
				break;
			case 'b':
				s[j] = '\b';
				++i;
				break;
			case 't':
				s[j] = '\t';
				++i;
				break;
			case 'n':
				s[j] = '\n';
				++i;
				break;
			case 'v':
				s[j] = '\v';
				++i;
				break;
			case 'f':
				s[j] = '\f';
				++i;
				break;
			case 'r':
				s[j] = '\r';
				++i;
				break;
			case '\\':
				s[j] = '\\';
				++i;
				break;
			default:
				s[j] = s[i];
				continue;
		}
	}

	while(i > j)
		s[j++] = 0;

	return s;
}

/* TODO add locate */
void siv(Reprog *rearr[REMAX], Biobuf *inb, Biobuf *outb, int depth, int t, char **wp, size_t *wsize) {
	Resub stack[REMAX-2];
	Resub range, target;
	Reprog *base, **arr;
	size_t wlen;
	int i;

	--depth; /* sub 1 because stack is only used starting from second regex */
	base = *rearr;
	arr = rearr + 1;

	while((wlen = Bgetre(inb, base, 0, 0, wp, wsize)) > 0) {
		stack[0] = (Resub){0};
		i = 0;

		while(i >= 0) {
			range = stack[i];

			if(depth >= 0 && !strgetre(*wp, arr[i], &range, 1)) {
				--i;
				continue;
			}

			if(t != 0 && i == t) /* don't save range if target is at base */
				target = range;

			stack[i].s.sp = range.e.ep;

			if(i < depth) {
				stack[++i] = range;
				continue;
			}

			if(t == 0) {
				Bwrite(outb, *wp, wlen);
				break;
			}

			Bwrite(outb, target.s.sp, target.e.ep - target.s.sp);

			i = t;
		}
	}
}

void
cleanup(void)
{
	int i;

	for(i = 0; i < n; ++i)
		free(progarr[i]);

	if(dstack)
		free(dstack);

	free(inb.bbuf - Bungetsize);
	free(wp);
	Bterm(&inb);
	Bterm(&outb);
}

int
main(int argc, char *argv[])
{
	name = argv[0];

	if(argc == 1) {
		fprint(2, "%s: no options given\n", name);
		fprint(2, USAGE, name);
		return 1;
	}

	inputbuf = malloc(Bsize);
	wp = malloc((wsize = 1024));
	inb.bbuf = inputbuf;
	inb.bsize = Bsize;
	Binit(&outb, 1, O_WRONLY);
	stack = sstack;
	dstack = 0;
	fd = 0;
	t = -2;
	n = 0;
	recur = 0;
	locat = 0;

	size_t optind;
	for(optind = 1; optind < argc && argv[optind][0] == '-'; ++optind) {
		switch(argv[optind][1]) {
			case 'e':
				if(++optind == argc) {
					fprint(2, "%s: '-e' requires an argument\n", name);
					cleanup();
					return 1;
				}

				if(n >= REMAX) {
					fprint(2, "%s: too expressions given\n", name);
					cleanup();
					return 1;
				}

				progarr[n++] = regcompnl(escape(argv[optind]));
				break;
			case 'r':
				recur = 1;
				break;
			case 'l':
				locat = 1;
				break;
			case 't':
				if(++optind == argc) {
					fprint(2, "%s: '-t' requires an argument\n", name);
					cleanup();
					return 1;
				}

				t = atoi(argv[optind]);
				break;
			case 'h':
				fprint(2, USAGE, name);
				cleanup();
				return 1;
			default:
				fprint(2, "%s: unknown option %s\n", name, argv[optind]);
				fprint(2, USAGE, name);
				cleanup();
				return 1;
		}
	}

	if(n == 0 && optind < argc)
		progarr[n++] = regcompnl(escape(argv[optind++]));

	if(t < 0)
		t += n > 1 ? n : 2;

	if(t > n - 1) {
		fprint(2, "%s: index %i out of range\n", name, t);
		cleanup();
		return 1;
	}

	depth = n - 1; /* set max depth for siv */

	if(optind == argc) {
		Binits(&inb, 0, O_RDONLY, inputbuf, Bsize);
		strcpy(path, "<stdin>");
		siv(progarr, &inb, &outb, depth, t, &wp, &wsize);
		cleanup();
		return 0;
	}

	for(; optind < argc; ++optind) {
		fd = open(argv[optind], O_RDONLY);

		if(fd < 0) {
			fprint(2, "%s: %s: no such file or directory\n", name, argv[optind]);
			cleanup();
			return 1;
		}

		strcpy(path, argv[optind]);

		fstat(fd, &buf);

		if(!S_ISDIR(buf.st_mode)) {
			Binits(&inb, fd, O_RDONLY, inb.bbuf, inb.bsize);
			siv(progarr, &inb, &outb, depth, t, &wp, &wsize);
			close(fd);
			continue;
		}

		if(!recur) {
			fprint(2, "%s: %s: is a directory\n", name, argv[optind]);
			close(fd);
			continue;
		}

		d = 0;
		stack[d].cp = path + strlen(path);
		if(*(stack[d].cp - 1) != '/')
			*(stack[d].cp++) = '/';
		stack[d].dp = fdopendir(fd);
		while(d > -1) {
			while((ent = readdir(stack[d].dp)) != NULL) {
				if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
					continue;

				strcpy(stack[d].cp, ent->d_name);

				if(ent->d_type == DT_DIR) {
					++d;
					if(dstack == 0 && d >= STATIC_DEPTH) {
						dstack = malloc(DYNAMIC_DEPTH * sizeof(Rframe));
						memcpy(dstack, sstack, STATIC_DEPTH * sizeof(Rframe));
						stack = dstack;
					}

					stack[d].cp = stack[d - 1].cp + strlen(ent->d_name);
					stack[d].dp = opendir(path);
					*(stack[d].cp++) = '/';
					continue;
				}

				if(ent->d_type == DT_REG) {
					fd = open(path, O_RDONLY);
					Binits(&inb, fd, O_RDONLY, inb.bbuf, inb.bsize);
					siv(progarr, &inb, &outb, depth, t, &wp, &wsize);
					close(fd);
				}
			}

			closedir(stack[d--].dp);
		}
	}

	cleanup();
	return 0;
}
