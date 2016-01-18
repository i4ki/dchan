#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "dat.h"
#include "fns.h"

void usage(void)
{
	fprint(2, "Usage: dchan [-D] [-d] [-s srvname] [-m mptp]\n");
	threadexits("usage");
}

Srv fs = {
	.attach		= fsattach,
	.open		= fsopen,
	.clunk		= fsclunk,
	.read 		= fsread,
	.write 		= fswrite,
	.create		= fscreate,
	.stat 		= fsstat,
	.wstat		= fswstat,
	.end 		= fsfinish,
	.flush 		= fsflush,
	.remove		= fsremove,
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
