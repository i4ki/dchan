#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#define DBG if(debug) print
#define defperm 		0650
#define CHANSZ			0	/* by default the channel is unbuffered */
#define THNAMESZ		40	/* Maz size for thread name */
#define MAXFILES		256
#define MAXFSIZE		64 * 1024
#define RSTOP			1
#define WSTOP			2

enum
{
	Xctl 	= 1,
	Xstat,
	Xapp,		/* application files */
};

typedef struct Data Data;	/* user data */
typedef struct Faux Faux;	/* file aux	 */
typedef struct Ctl Ctl;		/* ctl data */

struct Data {
	char *val;
	int valsz;
};

struct Faux {
	ushort ftype;
	Channel *chan;
	uint chansize;
	Reqqueue *rq;	/* queue for read requests */
	Reqqueue *wq;	/* queue for write requests */
	File *file;

	char rthreadname[THNAMESZ];	/* reader thread name */
	char wthreadname[THNAMESZ];	/* writer thread name */	
};

struct Ctl {
	Channel *rwok;	/* reader/writer threads notify channel */
	Channel *done;	/* ctl notify successfull updates here */
	int onctl;

	QLock l;
};

struct Summ {
	ulong tx;
	ulong rx;
};

static char Enotimpl[] 	= "dchan: not implemented";
static char Enotperm[] 	= "dchan: operation not permitted";
static char Ewrfail[]	= "dchan: write failed";	
static char Eintern[] 	= "dchan: internal error";
static char Ebadspec[]	= "dchan: bad attach specification";
static char Eexist[] 	= "dchan: file already exists";
static char Enotowner[] = "dchan: not owner";
static char Ebadoff[] 	= "dchan: bad file offset or count";
static char Elocked[] 	= "dchan: file locked";
static char Emaxfiles[]	= "dchan: Maximum number of files reached: 256";
static char Ectlfmt[]	= "dchan: Invalid ctl config format.";
static char Enotfound[]	= "dchan: ctl write failed. Target file not found.";

static Faux *files[MAXFILES];
static Ctl ctl;
static ushort filecnt;
int debug;

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
	if(a & AWRITE){
		f->mtime = f->atime;
		f->qid.vers++;
	}
}

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

static void
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

	DBG("fsread: Reporting a success read: (%s):(count: %d)(offset: %d)\n", r->ofcall.data, r->ofcall.count, (int)r->ofcall.offset);
	respond(r, nil);
}

void 
readctl(Req *r)
{
	int i, total = 0;
	Faux *aux;
	char line[1024];
	char *buffer;

	if(filecnt == 0)
		goto End;

	buffer = mallocz(sizeof(char)*filecnt*1024, 1);

	if(!buffer)
		sysfatal("Failed to allocate memory");

	for(i = 0; i < filecnt; i++) {
		aux = files[i];

		assert(aux != nil);

		total += snprintf(line, 1024, "/%s\t%d\n", aux->file->name, aux->chansize);

		DBG("Line: %s\n", line);
		buffer = strcat(buffer, line);
	}

	assert(total <= sizeof(char)*filecnt*1024);

	readstr(r, buffer);

	free(buffer);

End:
	respond(r, nil);
}

void
readstat(Req *r)
{
	/* empty file for now */
	readstr(r, "");
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

void syncwrite(Req *r)
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

	respond(r, nil);
	return;

Enomem:
	respond(r, Eintern);
}

Faux *
getfileaux(char *name)
{
	int i;
	Faux *tmp;

	for(i = 0; i < filecnt; i++){
		tmp = files[i];

		if(strcmp(tmp->file->name, name) == 0){
			return tmp;
		}
	}

	return nil;
}

void
writectl(Req *r)
{
	Faux *taux;
	char line[1024];
	char *buf;
	int chansize, i;
	Channel *chan;
	int op, err;

	memset(line, 0, 1024);

	if(r->ifcall.count >= 1024){
		respond(r, Ectlfmt);
		return;
	}

	memcpy(line, r->ifcall.data, r->ifcall.count);

	buf = strchr(line, ' ');

	if(buf == nil){
		respond(r, Ectlfmt);
		return;
	}

	buf[0] = '\0';
	buf++;

	chansize = atoi(buf);	/* returns 0 if buf is not a number */

	taux = getfileaux(line);

	if(taux == nil){
		respond(r, Enotfound);
		return;
	}

	qlock(&ctl.l);
	ctl.onctl = 1;
	qunlock(&ctl.l);

	chan = chancreate(sizeof(void *), chansize);

	if(!chan)
		sysfatal("failed to allocate channel");

	/* close the channel of target file */
	chanclose(taux->chan);

	for(i = 0; i < 2; i++){
		err = recv(ctl.rwok, &op);

		if(err == -1){
			respond(r, Eintern);
			return;
		}

		if(op == WSTOP)
			DBG("Thread writer stopped.\n");
		else if(op == RSTOP)
			DBG("Thread reader stopped.\n");
	}

	/* we're ready to update channel */
	
	chanfree(taux->chan);
	taux->chan = chan;
	taux->chansize = chansize;

	qlock(&ctl.l);
	ctl.onctl = 0;
	qunlock(&ctl.l);

	/* notify the reader and writer threads */
	if(	sendul(ctl.done, 1) == -1 ||
		sendul(ctl.done, 1) == -1)
		respond(r, Eintern);
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

	if(faux->ftype == Xctl)
		callb = readctl;

	if(faux->ftype == Xstat)
		callb = readstat;

	reqqueuepush(faux->rq, r, callb);
}

void
fswrite(Req *r)
{
	Faux *faux =  r->fid->file->aux;
	void *callb = syncwrite;

	if(faux == nil)
		sysfatal("aux is nil");

	if(faux->ftype == Xstat){
		respond(r, Enotperm);
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

	if(filecnt >= MAXFILES){
		respond(r, Emaxfiles);
		return;
	}

	f = createfile(r->fid->file, r->ifcall.name, r->fid->uid, defperm|r->ifcall.perm, nil);

	if(f) {
		faux = mallocz(sizeof *faux, 1);

		if(faux == nil)
			sysfatal("Failed to allocate memory for faux");

		chan = chancreate(sizeof(void *), 0);

		if(!chan)
			sysfatal("Failed to allocate channel");
	
		faux->ftype = Xapp;
		faux->rq = reqqueuecreate();
		faux->wq = reqqueuecreate();
		faux->file = f;
		faux->chan = chan;
		faux->chansize = 0;

		 /* accessfile(f, AWRITE); */

		f->aux = faux;
		files[filecnt++] = faux;
		r->fid->file = f;
		r->ofcall.qid = f->qid;
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
		r->d.length = filecnt * 1024;
		r->d.atime = f->atime;	/* last read equals to now() */
		r->d.mtime = f->mtime;
		break;
	case Xstat:
		r->d.mode = 0444;
		r->d.length = 4 * 1024;
		r->d.atime = f->atime;
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
	respond(r, nil);
}

void fsfinish(Srv *)
{
	
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

void
fsdestroyfile(File *f)
{
	Faux *faux;

	DBG("Closing %s\n", f->name);

	if(faux = f->aux)
	if(faux->ftype != Xctl)
	if(faux->ftype != Xstat) {
		reqqueuefree(faux->rq);
		reqqueuefree(faux->wq);
		chanfree(faux->chan);

		free(faux);
	}

	closefile(f);
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
}

void
createstats(Srv *fs)
{
	File *f;
	Faux *aux;

	f = createfile(fs->tree->root, "stats", getuser(), 0444, nil);

	if(!f)
		sysfatal("Failed to create stats file.");

	aux = mallocz(sizeof *aux, 1);

	if(!aux)
		sysfatal("Failed to allocate faux");

	aux->ftype = Xstat;
	aux->rq = reqqueuecreate();
	/* stats does not handle writes */

	f->aux = aux;
	aux->file = f;
}

void usage(void)
{
	fprint(2, "Usage: dchan [-D] [-d] [-s srvname] [-m mptp]\n");
	threadexits("usage");
}

Srv fs = {
	.attach	= fsattach,
	.open	= fsopen,
	.read 	= fsread,
	.write 	= fswrite,
	.create	= fscreate,
	.stat 	= fsstat,
	.wstat	= fswstat,
	.end 	= fsfinish,
	.flush 	= fsflush,
};

void
threadmain(int argc, char *argv[])
{
	char *addr = nil;
	char *srvname = nil;
	char *mptp = nil;

	Qid q;

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	q = fs.tree->root->qid;

	createctl(&fs);
	createstats(&fs);

	ARGBEGIN {
	case 'd':
		debug++;
		break;
	case 'D':
		chatty9p++;
		break;
	case 'a':
		addr = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mptp = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND
	
	if(argc)
		usage();

	if(chatty9p) {
		fprint(2, "fs.nopipe %d srvname %s mptp %s\n", fs.nopipe, srvname, mptp);
	}

	threadsetname("threadmain");

	if(addr == nil && srvname == nil && mptp == nil) {
		sysfatal("must specify -a, -s, or -m option");
	}

	if(addr) {
		threadlistensrv(&fs, addr);
	}

	if(srvname || mptp) {
		threadpostmountsrv(&fs, srvname, mptp, MREPL|MCREATE);
	}

	threadexits(0);
}
