CC=clang
CFLAGS=-Wall
# LDLIBS=-ldl

malloctrap: malloctrap.c
	gcc -shared -fPIC -ldl -o libmalloctrap.so malloctrap.c
