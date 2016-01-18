</$objtype/mkfile

TARG = dchan

dchan:		main.$O fs.$O file.$O
			$LD $LDFLAGS -o $TARG fs.$O file.$O main.$O 

main.$O:	main.c
			$CC $CFLAGS main.c

fs.$O:		fs.c
			$CC $CFLAGS fs.c

file.$O:	file.c
			$CC $CFLAGS file.c

clean:
	rm *.$O $TARG

run:
	kill $TARG | rc
	./$TARG -s $TARG -a tcp!*!6666 -d 

rund:
	kill $TARG | rc
	./$TARG -s $TARG -a tcp!*!6666 -d -D
