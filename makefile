
ppexec: ppexec.c libpp.a  P5TEST1.c P5TEST2.c P5TEST3.c
	gcc -g -o ppexec ppexec.c  -lpp -L. 
	gcc -g -o P5TEST1 P5TEST1.c  -lpp -L.	
	gcc -g -o P5TEST2 P5TEST2.c  -lpp -L.
	gcc -g -o P5TEST3 P5TEST3.c  -lpp -L.
	
libpp.a: pplib.c
	gcc -g -c pplib.c -o pp.o 
	ar rvf libpp.a pp.o 

clean:
	rm -rf *.o
	rm -f ppexec P5TEST1 P5TEST2 P5TEST3 libpp.a
