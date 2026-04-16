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

run-sequential: $(TARGET)
	./$(TARGET) --mode=sequential demo1.txt demo2.txt demo3.txt demo4.txt demo5.txt out/ 123

run-parallel: $(TARGET)
	./$(TARGET) --mode=parallel demo1.txt demo2.txt demo3.txt demo4.txt demo5.txt out/ 123

run-auto: $(TARGET)
	./$(TARGET) demo1.txt demo2.txt demo3.txt demo4.txt demo5.txt out/ 123

.PHONY: all clean run-sequential run-parallel run-auto