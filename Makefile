CC = gcc
CFLAGS = -Wall -Wextra -pedantic -fPIC -O2
LDFLAGS = -ldl

LIBNAME = libcaesar
LIBFILE = $(LIBNAME).so
LIB_SRC = caesar.c
LIB_OBJ = caesar.o

TEST = test
TEST_SRC = test.c
TEST_OBJ = test.o

PREFIX = /usr/local
LIBDIR = $(PREFIX)/lib

all: $(LIBFILE)

$(LIBFILE): $(LIB_OBJ)
	$(CC) -shared -o $@ $^

$(LIB_OBJ): $(LIB_SRC) caesar.h
	$(CC) $(CFLAGS) -c $<

$(TEST_OBJ): $(TEST_SRC)
	$(CC) $(CFLAGS) -c $<

test_bin: $(TEST_OBJ)
	$(CC) -o $@ $< $(LDFLAGS)

install: $(LIBFILE)
	@echo "Installing $(LIBFILE) to $(LIBDIR)..."
	@mkdir -p $(LIBDIR)
	cp $(LIBFILE) $(LIBDIR)/
	ldconfig
	@echo "Library installed successfully"

test: $(LIBFILE) test_bin input.txt
	@echo ""
	@echo "Running test"
	./test_bin ./$(LIBFILE) 'X' input.txt output.enc
	@echo ""
	@echo "Verifying decryption"
	./test_bin ./$(LIBFILE) 'X' output.enc output_decrypted.txt
	@echo ""
	@echo "Comparing original and decrypted"
	diff input.txt output_decrypted.txt && echo "SUCCESS: Files match!" || echo "FAILED: Files differ!"
	@echo ""
	@echo "Contents of output files (hexdump)"
	@echo "Original input:"
	@hexdump -C input.txt | head -5
	@echo "Encrypted output:"
	@hexdump -C output.enc | head -5
	@echo "Decrypted output:"
	@hexdump -C output_decrypted.txt | head -5

clean:
	rm -f $(LIB_OBJ) $(LIBFILE)
	rm -f $(TEST_OBJ) test_bin
	rm -f output.enc output_decrypted.txt

distclean: clean
	rm -f $(LIBDIR)/$(LIBFILE)
	ldconfig

.PHONY: all install test clean distclean
