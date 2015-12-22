</$objtype/mkfile

dchan:		main.$O
			$LD $LDFLAGS -o dchan main.$O

main.$O:	main.c
			$CC $CFLAGS main.c
	
	