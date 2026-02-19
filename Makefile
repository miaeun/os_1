CC = gcc
CFLAGS = -fPIC -Wall -O2
LDFLAGS = -shared
LIB_NAME = libcaesar.so
TARGET = caesar_program

all: $(LIB_NAME) $(TARGET)
$(LIB_NAME): caesar.o
	$(CC) $(LDFLAGS) -o $@ $^

caesar.o: caesar.c caesar.h
	$(CC) $(CFLAGS) -c caesar.c

main.o: main.c
	$(CC) -c main.c

$(TARGET): main.o
	$(CC) -o $@ $^ -ldl

clean:
	rm -f *.o $(LIB_NAME) $(TARGET) output.txt decrypted_output.txt

install: $(LIB_NAME)
	sudo cp $(LIB_NAME) /usr/local/lib/
	sudo ldconfig

run: all
	@echo "creatin test file..."
	echo "hello world! I'm miaeun" > input.txt
	@echo "startin..."
	./$(TARGET) ./$(LIB_NAME) 3 input.txt output.txt
	@echo "content of the file:"
	cat input.txt
	@echo "content of the ciphered file:"
	cat output.txt
	@echo "content of deciphered file:"
	cat decrypted_output.txt

.PHONY: all clean install run