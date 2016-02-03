#define DBG 			if(debug) print
#define THNAMESZ		40	/* Maz size for thread name */

enum
{
	Xctl 	= 1,
	Xstat,
	Xdir,
	Xapp,					/* application files */
};

typedef struct Data Data;			/* user data */
typedef struct Faux Faux;			/* file aux	 */
typedef struct FauxList FauxList;	/* file aux list */
typedef struct Ctl Ctl;				/* ctl data */

struct Data {
	char *val;
	int valsz;
};

struct Faux {
	ushort ftype;	/* Dchan File type */
	Channel *chan;	/* Channel for file data */
	uint chansize;
	Reqqueue *rq;	/* queue for read requests */
	Reqqueue *wq;	/* queue for write requests */
	File *file;
	char *fullpath;
	uint open;		/* number of clients that have this file open */
	QLock openl;	/* lock for open/clunk update */

	/* statistics */
	QLock slock;
	ulong nwrites;	/* number of writes in interval */
	ulong nreads;	/* number of reads in interval */

	Channel *timer;	/* interval timer for collect stats */
	long rx;		/* receive rate (req/s) */
	long tx;		/* transmission rate (req/s) */
	long avgrx;		/* average receive rate */
	long avgtx;		/* average transmission rate */

	char rthreadname[THNAMESZ];	/* reader thread name */
	char wthreadname[THNAMESZ];	/* writer thread name */	
};

struct FauxList {
	Faux *aux;
	FauxList *next;
};

/* data type used to synchronize access to file channels */
struct Ctl {
	Channel *rwok;	/* reader/writer threads notify channel */
	Channel *done;	/* ctl notify successfull updates here */
	int onctl;

	QLock l;
};

static int debug;	/* enable debugging info */
