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

# The RadioLib adapter, compiled against tests/mock/. Proves the port
# satisfies the core's contract; does not prove it matches the real
# RadioLib, which only hardware can.
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -O1 -g
PORT_SRC = ../src/loraitp_frame.c ../src/loraitp_crc.c ../src/loraitp_bitmap.c \
           ../src/loraitp_fec.c ../src/loraitp_mac.c ../src/loraitp_governor.c \
           ../src/loraitp_session.c

test_port: vectors/vectors.h
	$(CC)  $(CFLAGS) -c $(PORT_SRC)
	$(CXX) $(CXXFLAGS) -I../src -I../port -Imock \
	  ../port/port_radiolib.cpp mock/mock.cpp test_port_radiolib.cpp \
	  loraitp_*.o -o $@
	rm -f loraitp_*.o

port: test_port
	./test_port

# The image store, against real files in a temporary directory. Written
# on stdio so the same code runs over LittleFS on the target.
test_store: ../port/loraitp_store.c test_store.c
	$(CC) $(CFLAGS) -I../port ../port/loraitp_store.c test_store.c -o $@

store: test_store
	./test_store

# The JPEG encoder. Structural checks in C; the real verification is
# verify_jpeg.py, which decodes the output with an independent decoder.
test_jpeg: ../firmware/app/jpeg.c test_jpeg.c
	$(CC) $(CFLAGS) -I../firmware/app ../firmware/app/jpeg.c test_jpeg.c \
	  -o $@ -lm

jpeg: test_jpeg
	./test_jpeg

# Board headers as C++. They are included from .cpp on the target, and
# C++ rejects out-of-order designated initialisers where C accepts them.
# Designated initialisers are a C++20 feature that GCC also accepts as an
# extension before then, which is how the firmware uses them. Either way
# the declaration-order rule applies, and that is what this catches.
test_boards: test_boards.cpp ../firmware/boards/*.h
	$(CXX) -std=c++17 -Wall -Wextra -Wno-c++20-extensions -O1 \
	  -I../firmware/boards test_boards.cpp -o $@

boards: test_boards
	./test_boards

clean:
	rm -f test_core test_core_san test_port test_store test_jpeg test_boards *.o ../src/*.o ../port/*.o

.PHONY: all run san port store jpeg boards clean
