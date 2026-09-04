# orouteragent - host tests for the protocol decoders
#
# Only the decode paths are covered here: they parse untrusted network
# bytes and depend on libc alone, so they build and run on the host even
# though the daemon itself cannot (it needs the OpenWrt-only libraries).
#
#   make check   unit tests + fuzz pass, both under ASan/UBSan
#   make fuzz FUZZ_ITERS=200000 FUZZ_SEED=0x1234

CC      ?= gcc
SRC     := ../src/protocol
CFLAGS  ?= -O1 -g -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Wno-unused-parameter
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
INC     := -I. -I$(SRC)

# Decoder sources under test (no external dependencies).
UNITS := $(SRC)/buf.c $(SRC)/framing.c $(SRC)/base64.c \
         $(SRC)/rtty_proto.c $(SRC)/dmp_proto.c

FUZZ_ITERS ?= 20000
# Fixed seed so a failing run is reproducible; FUZZ_SEED=0 picks one from
# the clock for broader exploration.
FUZZ_SEED  ?= 0x2545F4914F6CDD1D

export ASAN_OPTIONS ?= detect_leaks=1:abort_on_error=1
export UBSAN_OPTIONS ?= print_stacktrace=1:halt_on_error=1

all: check

test_protocol: test_protocol.c harness.h $(UNITS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(INC) -o $@ test_protocol.c $(UNITS)

fuzz_protocol: fuzz_protocol.c $(UNITS)
	$(CC) $(CFLAGS) $(SANFLAGS) $(INC) -o $@ fuzz_protocol.c $(UNITS)

check: test_protocol fuzz_protocol
	./test_protocol
	./fuzz_protocol $(FUZZ_ITERS) $(FUZZ_SEED)

fuzz: fuzz_protocol
	./fuzz_protocol $(FUZZ_ITERS) $(FUZZ_SEED)

clean:
	rm -f test_protocol fuzz_protocol

.PHONY: all check fuzz clean