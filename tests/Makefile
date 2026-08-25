# Core tests on the host. No target SDK, no radio, no waiting.
#
# The core must build with a plain C compiler; if it stops doing so,
# that is the bug, not the test that caught it.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wsign-conversion -Wstrict-prototypes -Wcast-qual -O1 -g
CFLAGS  += -I../src -I../port -I.
LDFLAGS ?=

SRC = ../src/loraitp_frame.c ../src/loraitp_crc.c ../src/loraitp_bitmap.c \
      ../src/loraitp_fec.c ../src/loraitp_mac.c ../src/loraitp_governor.c \
      ../src/loraitp_session.c ../port/port_sim.c
OBJ = $(SRC:.c=.o) test_core.o

all: test_core

vectors/vectors.h:
	cd .. && python3 tests/gen_vectors.py

test_core: vectors/vectors.h $(SRC) test_core.c
	$(CC) $(CFLAGS) $(SRC) test_core.c -o $@ $(LDFLAGS)

run: test_core
	./test_core

# Undefined-behaviour and address sanitizers. Worth running because the
# decoders take frames off the air and must not read past their buffers.
san: vectors/vectors.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(SRC) test_core.c -o test_core_san
	./test_core_san

clean:
	rm -f test_core test_core_san *.o ../src/*.o ../port/*.o

.PHONY: all run san clean
