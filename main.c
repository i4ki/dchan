#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#define DBG if(debug) print
#define defperm 		0650

static char Enotimpl[] 	= "dchan: not implemented";
static char Enotperm[] 	= "dchan: operation not permitted";
static char Ewrfail[]	= "dchan: write failed";	
static char Eintern[] 	= "dchan: internal error";
static char Ebadspec[]	= "dchan: bad attach specification";
static char Eexist[] 	= "file already exists";
static char Enotowner[] = "not owner";
static char Ebadoff[] 	= "bad file offset or count";
static char Elocked[] 	= "file locked";

int debug;

long ctlatime, ctlmtime, statatime, statmtime;

typedef struct Data Data;	/* user data */
typedef struct Faux Faux;	/* file aux	 */
typedef struct Summ Summ;	/* summary / stats */

struct Data {
	char *val;
	int valsz;
};

struct Faux {
	ushort ftype;
	Channel *chan;
	Reqqueue *rq;	/* queue for read requests */
	Reqqueue *wq;	/* queue for write requests */

	long atime;	/* last read time */
	long mtime;	/* last write time */
};

struct Summ {
	ulong tx;
	ulong rx;
};

enum
{
	Xctl 	= 1,
	Xstat,
	Xapp,		/* application files */
};

enum
{
	MAXFSIZE	= 64 * 1024,
};

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
syncread(Req *r)
{
	Faux *faux;
	Data *d;
	ulong offset;

	// BUGGY
	char tname[20 + 20];	/* at max: descr len = 20, file name = 20 */
	snprintf(tname, 40, "%s in file %s", "readsync", r->fid->file->name);

	/* now you can do: ps -a -r | grep <filename being read> */
	threadsetname(tname);

	DBG("fsread: Count = %d\n", r->ifcall.count);
	DBG("fsread: Offset = %d\n", (int)r->ifcall.offset);

	faux = r->fid->file->aux;

	d = recvp(faux->chan);

	if(d == nil) {
		/* recv failed, client interrupted or disconnected */
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

	/* success read */
	time(&faux->atime);

	DBG("fsread: Reporting a success read: (%s):(count: %d)(offset: %d)\n", r->ofcall.data, r->ofcall.count, (int)r->ofcall.offset);
	respond(r, nil);
}

void 
readctl(Req *r)
{
	/* empty file for now */
	readstr(r, "");
	respond(r, nil);
}

void
readstat(Req *r)
{
	/* empty file for now */
	readstr(r, "");
	respond(r, nil);
}

void
fsread(Req *r)
{
	Faux *faux =  r->fid->file->aux;

	if(faux == nil)
		sysfatal("aux is nil");

	switch(faux->ftype) {
	case Xctl:
		readctl(r);
		return;
	case Xstat:
		readstat(r);
		return;
	}

	reqqueuepush(faux->rq, r, syncread);
}

void syncwrite(Req *r)
{
	Faux *faux;
	Data *d;

	DBG("fswrite: Count = %d\n", r->ifcall.count);
	DBG("fswrite: Offsetan = %d\n", (int)r->ifcall.offset);

	d = mallocz(sizeof *d, 1);

	if(!d)
		goto Enomem;

	faux = r->fid->file->aux;

	d->valsz = r->ifcall.count;

	d->val = mallocz(sizeof(char) * (d->valsz + 1), 1);

	if(!d->val)
		goto Enomem;

	d->val = memcpy(d->val, r->ifcall.data, d->valsz);
	d->val[d->valsz] = 0;

	r->ofcall.count = d->valsz;	/* 	store valsz now because after sendp return the value 
									will be freed by syncread */	
	
	int err = sendp(faux->chan, d);

	/* now d is an invalid pointer, freed in another coroutine */

	if(err != 1) {
		DBG("Sendp failed, probably cancelled/interrupted by client\n");

		/* data does not arrived in the other side, we need to release memory here */
		free(d->val);
		free(d);

		/* does not respond anything, flush will do the job */
		return;
	}

	/* success write */
	time(&faux->mtime);

	DBG("Send: %d\n", err);
	DBG("fswrite: Reporting success write");
	respond(r, nil);
	return;

Enomem:
	respond(r, Eintern);
}

void
writectl(Req *r)
{
	respond(r, Enotimpl);
}

void
fswrite(Req *r)
{
	Faux *faux =  r->fid->file->aux;

	if(faux == nil)
		sysfatal("aux is nil");

	switch(faux->ftype) {
	case Xctl:
		writectl(r);
		return;
	case Xstat:
		/* stat file isnt writable */
		respond(r, Enotperm);
		return;
	}

	reqqueuepush(faux->wq, r, syncwrite);
}

void
fscreate(Req *r)
{
	File *f;
	Faux *faux;
	Channel *chan;

	DBG("Create mode: %o\n", r->ifcall.perm);

	if(f = createfile(r->fid->file, r->ifcall.name, r->fid->uid, defperm|r->ifcall.perm, nil)) {
		DBG("File created\n");

		chan = chancreate(sizeof(void *), 0);
		faux = mallocz(sizeof *faux, 1);

		if(faux == nil)
			sysfatal("Failed to allocate memory for faux");
	
		faux->ftype = Xapp;
		faux->rq = reqqueuecreate();
		faux->wq = reqqueuecreate();
		faux->chan = chan;

		time(&faux->atime);
		faux->mtime = faux->atime;

		f->atime = faux->atime;
		f->mtime = faux->mtime;
		f->aux = faux;
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

	DBG("Stating file: %s\n", r->fid->file->name);

	if(aux = r->fid->file->aux)
	switch(aux->ftype) {
	case Xctl:
		r->d.mode = 0655;
		r->d.length = 4 * 1024;
		r->d.atime = ctlatime;	/* last read equals to now() */
		r->d.mtime = ctlmtime;
		break;
	case Xstat:
		r->d.mode = 0444;
		r->d.length = 4 * 1024;
		r->d.atime = statatime;
		r->d.mtime = statmtime;
		break;
	case Xapp:
		r->d.mode = 655;
		r->d.length = 0;
		r->d.atime = aux->atime;
		r->d.mtime = aux->mtime;
		break;
	}

	respond(r, nil);
}

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
		DBG("user = %s\n", u);
		DBG("file user = %s\n", f->uid);
		DBG("file group = %s\n", f->gid);

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
		reqqueueflush(faux->rq, o);
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
	Faux ctaux, staux;
	File *cf, *sf;

	ctaux.ftype = Xctl;
	staux.ftype = Xstat;

	Qid q;

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	q = fs.tree->root->qid;

	time(&ctlatime);
	ctlmtime = ctlatime;
	statatime = ctlatime;
	statmtime = ctlatime;

	cf = createfile(fs.tree->root, "ctl", getuser(), 0655, &ctaux);
	sf = createfile(fs.tree->root, "stats", getuser(), 0444, &staux);

	cf->atime = ctlatime;
	cf->mtime = ctlmtime;
	sf->atime = statatime;
	sf->mtime = statmtime;

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
