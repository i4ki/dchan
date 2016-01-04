#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#define DBG if(debug) print

static char Enotimpl[] = "dchan: not implemented";
static char Enotperm[] = "dchan: operation not permitted";
static char Eintern[] = "dchan: internal error";

typedef struct Data Data;

struct Data {
	char *val;
	int valsz;
};

enum
{
	Xctl = 1,
};

Channel *chan;
Reqqueue *rqueue, *wqueue;
int debug;

void
syncread(Req *r)
{
	Data d;
	DBG("fsread: Count = %d\n", r->ifcall.count);

	int err = recv(chan, &d);

	DBG("Recv: %d\n", err);
	readbuf(r, d.val, d.valsz);
	
	free(d.val);
	free(&d);

	DBG("fsread: Reporting a success read: (%s):%d\n", r->ofcall.data, r->ofcall.count);
	respond(r, nil);
}

void
fsread(Req *r)
{
	reqqueuepush(rqueue, r, syncread);
}

void syncwrite(Req *r)
{
	Data *d;
	DBG("fswrite: Count = %d\n", r->ifcall.count);

	d = mallocz(sizeof *d, 1);

	if(!d)
		respond(r, Eintern);

	d->val = strdup(r->ifcall.data);
	d->valsz = r->ifcall.count;
	int err = send(chan, d);

	DBG("Send: %d\n", err);

	r->ofcall.count = d->valsz;

	DBG("fswrite: Reporting success write: %s\n", d->val);
	respond(r, nil);
}

void
fswrite(Req *r)
{
	reqqueuepush(wqueue, r, syncwrite);
}

void 
fscreate(Req *r)
{
	respond(r, Enotperm);
}

void
fsopen(Req *r)
{
	respond(r, nil);
}

void fsend(Srv *)
{
	chanfree(chan);
}

void
fsdestroyfile(File *f)
{
	closefile(f);
}

void usage(void)
{
	fprint(2, "Usage: dchan [-D] [-s srvname] [-m mptp]\n");
	threadexits("usage");
}

Srv fs = {
	.open	= fsopen,
	.read 	= fsread,
	.write 	= fswrite,
	.create	= fscreate,
	.end = fsend,
};

void
threadmain(int argc, char *argv[])
{
	char *addr = nil;
	char *srvname = nil;
	char *mptp = nil;
	
	chan = chancreate(sizeof(struct Data), 0);
	rqueue = reqqueuecreate();
	wqueue = reqqueuecreate();

	Qid q;

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	q = fs.tree->root->qid;

	createfile(fs.tree->root, "ctl", "ctl", 0666, (void*)Xctl);

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
