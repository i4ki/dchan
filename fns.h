/* file.c */
void truncfile(File *f, vlong l);
void accessfile(File *f, int a);
char *fullpath(File *f);

/* fs.c */
void fscreate(Req *r);
void fsattach(Req *r);
void fsopen(Req *r);
void fsclunk(Req *r);
void fsread(Req *r);
void fswrite(Req *r);
void fsstat(Req *r);
void fswstat(Req *r);
void fsfinish(Srv *s);
void fsflush(Req *r);
void fsremove(Req *r);
void createctl(Srv *fs);
void createstats(Srv *fs);
void fsdestroyfile(File *f);
