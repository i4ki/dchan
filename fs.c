#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "dat.h"
#include "fns.h"

int casl(ulong *addr, ulong ov, ulong nv);

#define DEFPERM 		0650
#define CHANSZ			0	/* by default the channel is unbuffered */
#define MAXFILES		256
#define MAXFSIZE		64 * 1024
#define RSTOP			1
#define WSTOP			2
#define CTLMAX			1024

static char Enotimpl[] 		= "dchan: not implemented";
static char Enotperm[] 		= "dchan: operation not permitted";
static char Ewrfail[]		= "dchan: write failed";	
static char Eintern[] 		= "dchan: internal error";
static char Ebadspec[]		= "dchan: bad attach specification";
static char Eexist[] 		= "dchan: file already exists";
static char Enotowner[] 	= "dchan: not owner";
static char Ebadoff[] 		= "dchan: bad file offset or count";
static char Elocked[] 		= "dchan: file locked";
static char Emaxfiles[]		= "dchan: Maximum number of files reached: 256";
static char Ectlfmt[]		= "dchan: Invalid ctl config format.";
static char Enotfound[]		= "dchan: ctl write failed. Target file not found.";
static char Edir[]			= "dchan: invalid operation for directory.";
static char Edirnotempty[]	= "dchan: directory not empty.";
static char Edirdeep[]		= "dchan: directory too deep.";

static FauxList *files = nil;
QLock filelk;
static Ctl ctl;

int
waitforctl(int src)
{
	DBG("Sending message of stopping. %d\n", src);
	int err = sendul(ctl.rwok, src);

	if(err != 1)
		return 0;
	
	recvul(ctl.done);	/* block the caller until config is updated */
	return 1;
}

void
filestats(void *arg)
{
	Faux *aux;
	long t;

	aux = arg;

	for(t = 0;; t+=60) {
		sleep(1000);

		qlock(&aux->slock);
		aux->rx = aux->nreads;
		aux->tx = aux->nwrites;

		if(aux->avgtx == 0)
			aux->avgtx = aux->tx;
		else if(aux->tx > 0)
			aux->avgtx = (aux->avgtx + aux->tx)/2;

		if(aux->avgrx == 0)
			aux->avgrx = aux->rx;
		else if(aux->rx > 0)
			aux->avgrx = (aux->avgrx + aux->rx)/2;

		qunlock(&aux->slock);

		casl(&aux->nreads, aux->nreads, 0);		/* TODO: err handling */
		casl(&aux->nwrites, aux->nwrites, 0);	/* TODO: err handling */
	}
}

void
fsattach(Req *r)
{
	char *spec;

	spec = r->ifcall.aname;
	if(spec && spec[0]){
		respond(r, Ebadspec);
		return;
	}

	r->fid->qid = r->srv->tree->root->qid;
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

void
readersetname(Faux *aux, char *fn, char *name)
{
	if(aux->rthreadname[0] == 0){
		snprintf(aux->rthreadname, THNAMESZ, "dchan: %s in file %s", fn, name);
		threadsetname(aux->rthreadname);
	}
}

void
writersetname(Faux *aux, char *fn, char *name)
{
	if(aux->wthreadname[0] == 0){
		snprintf(aux->wthreadname, THNAMESZ, "dchan: %s in file %s", fn, name);
		threadsetname(aux->wthreadname);
	}
}

void
syncread(Req *r)
{
	Faux *faux;
	Data *d;
	ulong offset;
	int onctl;

	faux = r->fid->file->aux;

	if(faux == nil)
		sysfatal("syncread: faux is nil.");

	readersetname(faux, "syncread", r->fid->file->name);

Recv:
	d = recvp(faux->chan);

	if(d == nil) {
		/* recv failed, client interrupted, disconnected or ctl is being updated*/

		qlock(&ctl.l);
		onctl = ctl.onctl;
		qunlock(&ctl.l);

		if(onctl){
			if(!waitforctl(RSTOP))
				goto RecvFail;

			goto Recv;
		}

RecvFail:
		/* no respond here */
		return;
	}

	offset = r->ifcall.offset;
	r->ifcall.offset = 0;

	DBG("Recv: %s\n", d->val);
	readbuf(r, d->val, d->valsz);

	r->ifcall.offset = offset;
	
	free(d->val);
	free(d);

	accessfile(r->fid->file, AREAD);

	ainc((long *)&faux->nreads);

	DBG("fsread: Reporting a success read: (%s):(count: %d)(offset: %d)\n", r->ofcall.data, r->ofcall.count, (int)r->ofcall.offset);
	respond(r, nil);
}

void 
readctl(Req *r)
{
	int total = 0, written;
	Faux *aux;
	FauxList *l;
	char *buffer = nil;
	char line[1024];

	qlock(&filelk);

	for(l = files; l; l = l->next) {
		assert(l != nil);

		aux = l->aux;

		memset(line, 0, sizeof line);

		qlock(&aux->slock);	/* sync with filestats() */

		written = snprintf(line, 1024, "%s\t%d\t%ld\t%ld\t%ld\t%ld\n", aux->fullpath, aux->chansize, aux->rx, aux->tx, aux->avgrx, aux->avgtx);

		qunlock(&aux->slock);

		buffer = realloc(buffer, total + written + 1);

		if(!buffer)
			sysfatal("Failed to allocate memory.\n");

		memset(buffer+total, 0, written + 1);

		buffer = strcat(buffer, line);
		total += written;
	}

	qunlock(&filelk);

	if(buffer) {
		readstr(r, buffer);
		free(buffer);
	} else {
		readstr(r, "");
	}

	respond(r, nil);
}

int
filldata(Data *d, Req *r)
{
	d->valsz = r->ifcall.count;
	d->val = mallocz(sizeof(char) * (d->valsz + 1), 1);

	if(!d->val)
		return -1;

	d->val = memcpy(d->val, r->ifcall.data, d->valsz);
	d->val[d->valsz] = 0;

	return 0;
}

void
syncwrite(Req *r)
{
	Faux *faux;
	Data *d;
	int err, onctl;

	d = mallocz(sizeof *d, 1);

	if(!d)
		goto Enomem;

	faux = r->fid->file->aux;

	if(faux == nil)
		sysfatal("syncwrite: faux is nil.");

	writersetname(faux, "syncwrite", r->fid->file->name);

	/* copy request into data */
	if(filldata(d, r) == -1)
		goto Enomem;

	r->ofcall.count = d->valsz;	/* 	store valsz now because after sendp return the value 
									will be freed by syncread */	

Send:
	err = sendp(faux->chan, d);

	/* now d is an invalid pointer, free'd in another coroutine */

	if(err != 1) {
		DBG("Sendp failed: %d\n", err);

		if(err == -1){
			qlock(&ctl.l);
			onctl = ctl.onctl;
			qunlock(&ctl.l);

			if(onctl){
				DBG("ONCTL: waiting for ctl complete the task\n");
				if(!waitforctl(WSTOP))
					goto SendFail;

				goto Send;
			}
		}

SendFail:
		DBG("Sendp really failed.\n");
		/* data does not arrived in the other side, we need to release memory here */
		free(d->val);
		free(d);

		/* does not respond anything, flush will do the job */
		return;
	}

	/* success write */
	accessfile(r->fid->file, AWRITE);

	ainc((long *)&faux->nwrites);

	respond(r, nil);
	return;

Enomem:
	respond(r, Eintern);
}

Faux *
getfileaux(char *name)
{
	Faux *tmp;
	FauxList *l;

	if(files == nil)
		return nil;

	qlock(&filelk);

	for(l = files; l; l = l->next){
		tmp = l->aux;

		if(strcmp(tmp->fullpath, name) == 0) {
			qunlock(&filelk);
			return tmp;
		}
	}

	qunlock(&filelk);
	return nil;
}

char *
updatecfg(char *filename, int chansize)
{
	Faux *taux;
	int op, err, i;
	Channel *chan;

	taux = getfileaux(filename);

	if(taux == nil){
		return Enotfound;
	}

	qlock(&ctl.l);
	ctl.onctl = 1;
	qunlock(&ctl.l);

	chan = chancreate(sizeof(void *), chansize);

	if(!chan)
		sysfatal("failed to allocate channel");

	/* close the channel of target file */
	chanclose(taux->chan);

	/* no one can open files while updating cfg */
	qlock(&taux->openl);
	if(taux->open > 0) {
		for(i = 0; i < 2; i++){
			err = recv(ctl.rwok, &op);

			if(err == -1){
				qunlock(&taux->openl);
				return Eintern;
			}

			if(op == WSTOP)
				DBG("Thread writer stopped.\n");
			else if(op == RSTOP)
				DBG("Thread reader stopped.\n");
		}
	}

	/* we're ready to update channel */
	
	chanfree(taux->chan);
	taux->chan = chan;
	taux->chansize = chansize;

	qlock(&ctl.l);
	ctl.onctl = 0;
	qunlock(&ctl.l);

	/* notify the reader and writer threads */
	if(taux->open > 0)
	if(	sendul(ctl.done, 1) == -1 ||
		sendul(ctl.done, 1) == -1){
		qunlock(&taux->openl);
		return Eintern;
	}

	qunlock(&taux->openl);
	return nil;
}

void
writectl(Req *r)
{
	char line[CTLMAX];
	char *err = nil, *name, *buf, *buf2;
	int chansize;

	memset(line, 0, CTLMAX);

	if(r->ifcall.count >= CTLMAX){
		respond(r, Ectlfmt);
		return;
	}

	memcpy(line, r->ifcall.data, r->ifcall.count);

	name = line;

	while(buf = strchr(name, ' ')){
		if(buf == nil){
			respond(r, Ectlfmt);
			return;
		}

		buf[0] = '\0';
		buf++;

		buf2 = strchr(buf, '\n');

		if(buf2 != nil){
			buf2[0] = '\0';
			buf2++;
		}

		chansize = atoi(buf);	/* returns 0 if buf is not a number */

		if((err = updatecfg(name, chansize)) != nil)
			break;

		if(buf2 == nil)
			break;

		name = buf2;
	}
	
	if(err != nil)
		respond(r, err);
	else
		respond(r, nil);
}

void
fsread(Req *r)
{
	Faux *faux =  r->fid->file->aux;
	void *callb = syncread;

	if(faux == nil)
		sysfatal("aux is nil");

	if(faux->ftype == Xdir){
		respond(r, Edir);
		return;
	}

	if(faux->ftype == Xctl)
		callb = readctl;

	reqqueuepush(faux->rq, r, callb);
}

void
fswrite(Req *r)
{
	Faux *faux =  r->fid->file->aux;
	void *callb = syncwrite;

	if(faux == nil)
		sysfatal("aux is nil");

	if(faux->ftype == Xdir){
		respond(r, Edir);
		return;
	}

	if(faux->ftype == Xctl)
		callb = writectl;

	reqqueuepush(faux->wq, r, callb);
}

void
fscreate(Req *r)
{
	File *f;
	Faux *faux;
	Channel *chan;
	FauxList *fl;
	char *path;

	f = createfile(r->fid->file, r->ifcall.name, r->fid->uid, DEFPERM|r->ifcall.perm, nil);

	if(f) {
		path = fullpath(f);

		if(path == nil) {
			closefile(f);
			respond(r, Edirdeep);
			return;
		}

		faux = mallocz(sizeof *faux, 1);

		if(faux == nil)
			sysfatal("Failed to allocate memory for faux");

		r->fid->file = f;
		r->ofcall.qid = f->qid;

		if(r->ifcall.perm&DMDIR){
			faux->ftype = Xdir;
		} else {
			chan = chancreate(sizeof(void *), 0);

			if(!chan)
				sysfatal("Failed to allocate channel");
	
			faux->ftype = Xapp;
			faux->rq = reqqueuecreate();
			faux->wq = reqqueuecreate();
			faux->fullpath = path;
			faux->chan = chan;
			faux->chansize = 0;

			proccreate(filestats, faux, 1024);
			
			qlock(&filelk);

			fl = emalloc9p(sizeof *fl);
			fl->aux = faux;
			fl->next = files;
			files = fl;

			qunlock(&filelk);
		}

		incref(f);
		faux->file = f;
		f->aux = faux;

		respond(r, nil);
		return;
	}

	respond(r, Eintern);
}

void
fsstat(Req *r)
{
	Faux *aux;
	File *f;

	f = r->fid->file;

	if(aux = r->fid->file->aux)
	switch(aux->ftype) {
	case Xctl:
		r->d.mode = 0655;
		r->d.length = 1024;
		r->d.atime = f->atime;	/* last read equals to now() */
		r->d.mtime = f->mtime;
		break;
	case Xapp:
		r->d.mode = 655;
		r->d.length = 0;
		r->d.atime = f->atime;
		r->d.mtime = f->mtime;
		break;
	}

	respond(r, nil);
}

void
fswstat(Req *r)
{
	File *f, *w;
	char *u;

	f = r->fid->file;
	u = r->fid->uid;

	/*
	 * To change length, must have write permission on file.
	 */
	if(r->d.length != ~0 && r->d.length != f->length){
		if(r->d.length > MAXFSIZE){
			respond(r, Ebadoff);
			return;
		}
	 	if(!hasperm(f, u, AWRITE) || (f->mode & DMDIR) != 0)
			goto Perm;
	}

	/*
	 * To change name, must have write permission in parent.
	 */
	if(r->d.name[0] != '\0' && strcmp(r->d.name, f->name) != 0){
		if((w = f->parent) == nil)
			goto Perm;
		incref(w);
	 	if(!hasperm(w, u, AWRITE)){
			closefile(w);
			goto Perm;
		}
		if((w = walkfile(w, r->d.name)) != nil){
			closefile(w);
			respond(r, Eexist);
			return;
		}
	}

	/*
	 * To change mode, must be owner or group leader.
	 * Because of lack of users file, leader=>group itself.
	 */
	if(r->d.mode != ~0 && f->mode != r->d.mode){
		if(strcmp(u, f->uid) != 0)
		if(strcmp(u, f->gid) != 0){
			respond(r, Enotowner);
			return;
		}
	}

	/*
	 * To change group, must be owner and member of new group,
	 * or leader of current group and leader of new group.
	 * Second case cannot happen, but we check anyway.
	 */
	while(r->d.gid[0] != '\0' && strcmp(f->gid, r->d.gid) != 0){
		if(strcmp(u, f->uid) == 0)
			break;
		if(strcmp(u, f->gid) == 0)
		if(strcmp(u, r->d.gid) == 0)
			break;
		respond(r, Enotowner);
		return;
	}

	if(r->d.mode != ~0){
		f->mode = r->d.mode;
		f->qid.type = f->mode >> 24;
	}
	if(r->d.name[0] != '\0'){
		free(f->name);
		f->name = estrdup9p(r->d.name);
	}
	if(r->d.length != ~0 && r->d.length != f->length)
		truncfile(f, r->d.length);

	accessfile(f, AWRITE);
	if(r->d.mtime != ~0){
		f->mtime = r->d.mtime;
	}

	respond(r, nil);
	return;

Perm:
	respond(r, Enotperm);
}

void
fsopen(Req *r)
{
	File *f;
	Faux *aux;

	f = r->fid->file;
	if((f->mode & DMEXCL) != 0){
		if(f->ref > 2 && (long)((ulong)time(0)-(ulong)f->atime) < 300){
			respond(r, Elocked);
			return;
		}
	}
	if((f->mode & DMAPPEND) == 0 && (r->ifcall.mode & OTRUNC) != 0){
		truncfile(f, 0);
		accessfile(f, AWRITE);
	}

	aux = f->aux;

	if(aux == nil){
		respond(r, nil);
		return;
	}

	qlock(&aux->openl);
	aux->open++;
	qunlock(&aux->openl);

	respond(r, nil);
}

void
fsclunk(Req *r)
{
	Faux *aux;

	aux = r->fid->file->aux;

	if(aux == nil){
		respond(r, nil);
		return;
	}

	qlock(&aux->openl);
	if(aux->open > 0)
		aux->open--;
	
	qunlock(&aux->openl);

	respond(r, nil);
}

void fsfinish(Srv *)
{
	DBG("Finishing server.\n");
	
	chanfree(ctl.done);
	chanfree(ctl.rwok);
}

void
fsflush(Req *r)
{
	Faux *faux;
	Req *o;

	if(o = r->oldreq)
	if(faux = o->fid->file->aux) {
		if(o->ifcall.type == Tread)
			reqqueueflush(faux->rq, o);
		else if(o->ifcall.type == Twrite)
			reqqueueflush(faux->wq, o);
	}

	respond(r, nil);
}

int
dirhasfiles(File *f)
{
	Faux *auxp;
	FauxList *fl;

	if(!f->mode&DMDIR)
		return 0;

	qlock(&filelk);
	for(fl=files; fl; fl=fl->next) {
		auxp = fl->aux;

		if(auxp->file->parent == f){
			qunlock(&filelk);
			return 1;
		}
	}

	qunlock(&filelk);
	return 0;
}

void
fsremove(Req *r)
{
	File *f = r->fid->file;
	Faux *aux = f->aux;

	switch(aux->ftype){
	case Xctl:
	case Xapp:
		closefile(f);
		respond(r, nil);
		break;
	case Xdir:
		if(!dirhasfiles(f)) {
			closefile(f);
			respond(r, nil);
		} else {
			respond(r, Edirnotempty);
		}
		break;
	default:
		respond(r, Eintern);
	}
}

void
filelistremove(Faux *aux)
{
	FauxList *current, *prev, *tmp;

	if(!aux)
		return;

	qlock(&filelk);

	if(files->aux == aux) {
		tmp = files;
		files = files->next;

		free(tmp);
		free(aux);
		qunlock(&filelk);

		return;
	}

	current = files->next;
	prev = files;

	while(current != nil && prev != nil) {
		if(current->aux == aux) {
			tmp = current;
			prev->next = current->next;
			free(tmp);
			free(aux);

			qunlock(&filelk);

			return;
		}

		prev = current;
		current = current->next;
	}

	qunlock(&filelk);
}

void
fsdestroyfile(File *f)
{
	Faux *faux;

	if(faux = f->aux)
	switch(faux->ftype) {
	case Xctl:
		chanfree(ctl.done);
		chanfree(ctl.rwok);
	case Xapp:
		reqqueuefree(faux->rq);
		reqqueuefree(faux->wq);
		chanfree(faux->chan);
		free(faux->fullpath);
		break;
	}

	filelistremove(faux);
}

void
createctl(Srv *fs)
{
	File *f;
	Faux *aux;

	f = createfile(fs->tree->root, "ctl", getuser(), 0655, nil);

	if(!f)
		sysfatal("Failed to create ctl file.");

	aux = mallocz(sizeof *aux, 1);
	
	if(!aux)
		sysfatal("Failed to allocate Faux");

	memset(&ctl, 0, sizeof ctl);

	aux->ftype = Xctl;
	aux->rq = reqqueuecreate();
	aux->wq = reqqueuecreate();
	ctl.done = chancreate(sizeof(int), 2);
	ctl.rwok = chancreate(sizeof(int), 2);

	f->aux = aux;
	aux->file = f;
	incref(f);
}
