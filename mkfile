</$objtype/mkfile

BIN=$home/bin
TARG = 	dchan

OFILES=\
	main.$O\
	fs.$O\
	file.$O\

HFILES=\
	dat.h\
	fns.h\

UPDATE=\
	mkfile\
	$HFILES\
	${OFILES:%.$O=%.c}\

</sys/src/cmd/mkone

install:V:	$BIN/$TARG install-dstats

install-dstats:VE:
	mkdir -p $BIN/aux
	cp aux/dstats $BIN/aux/dstats

run:V:
	kill $O.out | rc
	./$O.out -s $TARG -a tcp!*!6666 -d 

rund:V:
	kill $TARG | rc
	./$O.out -s $TARG -a tcp!*!6666 -d -D


