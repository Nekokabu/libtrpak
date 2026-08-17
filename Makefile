# Host build for libtrpak.
#
# Nothing here builds an N64 ROM: the goal is to exercise the library's logic
# on a PC. Two independent targets are produced.
#
#   tests/test_libtrpak            The test suite. libtrpak.c is compiled with
#                                  TRPAK_NO_DEFAULT_IO, so the libdragon
#                                  backend is removed and the tests install a
#                                  simulated Transfer Pak instead.
#
#   tests/libtrpak_default_backend.o
#                                  Compile-only check of the platform layer.
#                                  It builds libtrpak.c with the default
#                                  backend and TRPAK_ENABLE_ED64_DMA against
#                                  the stub headers in tests/stubs, catching
#                                  renamed or retyped libdragon/EverDrive
#                                  functions without an N64 toolchain. It is
#                                  never linked or run.
#
# Usage: `make test` builds both and runs the suite. Override HOST_CC or
# HOST_CFLAGS to test other compilers or standards; -Werror is intentional, as
# the library is meant to stay warning-free.

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
