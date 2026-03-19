CC = gcc
CFLAGS = -Wall -pthread -O2
TARGET = secure_copy
OBJS = secure_copy.o caesar.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(CFLAGS)

secure_copy.o: secure_copy.c caesar.h
	$(CC) -c secure_copy.c -o secure_copy.o $(CFLAGS)

caesar.o: caesar.c caesar.h
	$(CC) -c caesar.c -o caesar.o $(CFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run