</$objtype/mkfile

TARG = dchan

dchan:		main.$O
			$LD $LDFLAGS -o $TARG main.$O

main.$O:	main.c
			$CC $CFLAGS main.c

clean:
	rm *.$O $TARG

run:
	kill $TARG | rc
	./$TARG -s $TARG -a tcp!*!6666 -d 

rund:
	kill $TARG | rc
	./$TARG -s $TARG -a tcp!*!6666 -d -D
