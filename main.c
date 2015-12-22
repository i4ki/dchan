#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

static char Enotimpl[] = "dchan: not implemented";
static char Enotperm[] = "dchan: operation not permitted";

enum
{
	Xctl = 1,
};

void
fsread(Req *r)
{
	char msg[] = "hacked by i4k";
	readstr(r, msg);
	respond(r, nil);
}

void
fswrite(Req *r)
{
	respond(r, Enotimpl);
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

void
fsdestroyfile(File *f)
{
	closefile(f);
}

Srv fs = {
	.open	= fsopen,
	.read 	= fsread,
	.write 	= fswrite,
	.create	= fscreate,
};

void usage(void)
{
	fprint(2, "Usage: dchan [-D] [-s srvname] [-m mptp]\n");
	exits("usage");
}


void main(int argc, char *argv[])
{
	char *addr = nil;
	char *srvname = nil;
	char *mptp = nil;

	Qid q;

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	q = fs.tree->root->qid;

	createfile(fs.tree->root, "ctl", "ctl", 0666, (void*)Xctl);

	ARGBEGIN {
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
		listensrv(&fs, addr);
	}

	if(srvname || mptp) {
		postmountsrv(&fs, srvname, mptp, MREPL|MCREATE);
	}

	exits(0);
}
