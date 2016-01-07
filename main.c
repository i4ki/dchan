#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#define DBG if(debug) print

static char Enotimpl[] 	= "dchan: not implemented";
static char Enotperm[] 	= "dchan: operation not permitted";
static char Ewrfail[]	= "dchan: write failed";	
static char Eintern[] = "dchan: internal error";

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
};

struct Summ {
	ulong tx;
	ulong rx;
};

enum
{
	Xctl 	= 1,
	Xstat	= 2,
};

int debug;

void
syncread(Req *r)
{
	Faux *faux;
	Data *d;
	ulong offset;

	char tname[20 + 20];	/* at max: descr len = 20, file name = 20 */
	snprintf(tname, 40, "%s in file %s", "readsync", r->fid->file->name);

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
	r->ofcall.offset = offset;
	
	free(d->val);
	free(d);

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
	DBG("fswrite: Offset = %d\n", (int)r->ifcall.offset);

	d = mallocz(sizeof *d, 1);

	if(!d)
		respond(r, Eintern);

	faux = r->fid->file->aux;

	d->valsz = r->ifcall.count;

	d->val = mallocz(sizeof(char) * (d->valsz + 1), 1);
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

	DBG("Send: %d\n", err);
	DBG("fswrite: Reporting success write");
	respond(r, nil);
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

	if(f = createfile(	r->fid->file, 
						r->ifcall.name, 
						getuser() /* r->fid->uid */, 
						0755, 
						nil)) {
		DBG("File created\n");

		chan = chancreate(sizeof(void *), 0);
		faux = mallocz(sizeof *faux, 1);

		if(faux == nil)
			sysfatal("Failed to allocate memory for faux");
	
		faux->rq = reqqueuecreate();
		faux->wq = reqqueuecreate();
		faux->chan = chan;

		f->aux = faux;
		r->fid->file = f;
		r->ofcall.qid = f->qid;
		respond(r, nil);
		return;
	}

	respond(r, Eintern);
}

void
fswstat(Req *r)
{
	r->d.mode = r->ifcall.perm;
	respond(r, nil);
}

void
fsopen(Req *r)
{
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
	.open	= fsopen,
	.read 	= fsread,
	.write 	= fswrite,
	.create	= fscreate,
	.wstat 	= fswstat,
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

	ctaux.ftype = Xctl;
	staux.ftype = Xstat;

	Qid q;

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	q = fs.tree->root->qid;

	createfile(fs.tree->root, "ctl", getuser(), 0666, &ctaux);
	createfile(fs.tree->root, "stats", getuser(), 0666, &staux);

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
