INCLUDES=-I..

OBJS := test.o

ifeq ($(BOARD_TESTS), 1)
	OBJS += test_undo.o board_regtest.o
endif

all: lib.a
lib.a: $(OBJS)


-include ../Makefile.lib
