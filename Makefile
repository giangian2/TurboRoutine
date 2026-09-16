CC      = gcc

# Optimization level, overridable from the command line:
#   make            ordinary build, -O0, easy to step through
#   make OPT=-O2    optimized build, required for profiling and measuring
# Changing OPT invalidates the objects already compiled (see $(OPTSTAMP)
# below), so build/ can never end up holding a mix of the two.
OPT     ?= -O0

# Structured debug logging (see include/Debug.h), off by default:
#   make            LOG_DEBUG/LOG_ERROR compile to nothing
#   make DEBUG=1    -DDEBUG, every LOG_DEBUG/LOG_ERROR prints to stderr
DEBUG   ?= 0
CFLAGS  = -std=c11 -Wall -Wextra -g -Iinclude $(OPT)
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG
endif

# Worker threads in the demo, and the switch routine's own debug info.
LDLIBS  = -pthread

# library sources (main.c is NOT part of the library)
SRC = 
ASM = src/coro_x86_64.S
HDR = $(wildcard include/*.h)
OBJ = $(SRC:src/%.c=build/%.o) $(ASM:src/%.S=build/%.o)
LIB = bin/libcoro.a

# Witness of the optimization level (and DEBUG setting) build/ was produced
# with: the file name embeds both, so switching either means it does not
# exist, the rule fires and throws the stale objects away before recompiling.
OPTSTAMP = build/.opt$(subst -,,$(OPT))-debug$(DEBUG)

all: $(LIB) bin/main

# the demo program, linked against the library
bin/main: src/main.c $(LIB) | bin
	$(CC) $(CFLAGS) $< -Lbin -lcoro $(LDLIBS) -o $@

$(LIB): $(OBJ) | bin
	ar rcs $@ $^

build/%.o: src/%.c $(HDR) $(OPTSTAMP) | build
	$(CC) $(CFLAGS) -c $< -o $@

# .S (capital S) goes through the C preprocessor first, so the assembly can
# #include the same ABI constants the C side asserts against.
build/%.o: src/%.S $(HDR) $(OPTSTAMP) | build
	$(CC) $(CFLAGS) -c $< -o $@

$(OPTSTAMP): | build
	rm -f build/*.o build/.opt* $(LIB)
	touch $@

run: bin/main | out
	./bin/main

# output directories, created on demand
bin build out:
	mkdir -p $@

memcheck:
	valgrind --leak-check=full --track-origins=yes bin/main

clean:
	rm -rf build bin

.PHONY: all run clean memcheck
