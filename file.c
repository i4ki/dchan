#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#define MAXDEEPDIR	16	/* maximum levels of directory */
#define MAXPATH		1024
#define PATHBYTES	(sizeof(char) * MAXPATH)

void
truncfile(File *f, vlong l)
{
	/** TODO: free queue */
	f->length = l;
}

void
accessfile(File *f, int a)
{
	f->atime = time(0);

	if(a & AWRITE) {
		f->mtime = f->atime;
		f->qid.vers++;
	}
}

char *
fullpath(File *f)
{
	char *path;
	char *elem[MAXDEEPDIR];
	int pos = 0, i;

	path = emalloc9p(PATHBYTES);

	memset(path, 0, PATHBYTES);

	while(strcmp(f->name, "/") != 0 && pos < MAXDEEPDIR) {
		elem[pos++] = f->name;

		f = f->parent;
	}

	if(pos >= (MAXDEEPDIR - 1))
		return nil;

	elem[pos++] = f->name;

	for(i = pos - 1; i >= 0; i--) {
		path = strncat(path, elem[i], PATHBYTES);

		if(i < (pos - 1) && i > 0)
			path = strncat(path, "/", PATHBYTES);
	}

	return path;
}
