all:
	gcc -shared -fPIC -o pipe.so lpipe.c -I/usr/include/lua5.3 -llua5.3
