HOST_CC ?= cc
HOST_CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
TEST_BINARY := tests/test_libtrpak
DEFAULT_BACKEND_OBJECT := tests/libtrpak_default_backend.o

.PHONY: all test clean

all: test

$(TEST_BINARY): libtrpak.c libtrpak.h tests/test_libtrpak.c
	$(HOST_CC) $(HOST_CFLAGS) -DTRPAK_NO_DEFAULT_IO \
		libtrpak.c tests/test_libtrpak.c -o $(TEST_BINARY)

$(DEFAULT_BACKEND_OBJECT): libtrpak.c libtrpak.h \
		tests/stubs/libdragon.h tests/stubs/sys.h
	$(HOST_CC) $(HOST_CFLAGS) -Itests/stubs -DTRPAK_ENABLE_ED64_DMA \
		-c libtrpak.c -o $(DEFAULT_BACKEND_OBJECT)

test: $(TEST_BINARY) $(DEFAULT_BACKEND_OBJECT)
	./$(TEST_BINARY)

clean:
	$(RM) $(TEST_BINARY) $(DEFAULT_BACKEND_OBJECT)
