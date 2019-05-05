all: lua5-3

lua5-3:
	gcc -shared -fPIC -o pipe.so lpipe.c -I/usr/include/lua5.3 -llua5.3

lua5-2:
	gcc -shared -fPIC -o pipe.so lpipe.c -I/usr/include/lua5.2 -llua5.2

clean:
	rm pipe.so
