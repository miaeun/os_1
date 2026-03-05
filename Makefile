CC = gcc
CFLAGS = -Wall -pthread -fPIC

all: libcaesar.so secure_copy

libcaesar.so: caesar.o
	$(CC) -shared -o libcaesar.so caesar.o

caesar.o: caesar.c caesar.h
	$(CC) $(CFLAGS) -c caesar.c

secure_copy: secure_copy.c caesar.h
	$(CC) secure_copy.c -L. -lcaesar -Wl,-rpath=. -pthread -o secure_copy

clean:
	rm -f *.o *.so secure_copy