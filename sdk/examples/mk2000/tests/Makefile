# Host tests for the parts of MK2000 that touch no hardware. Run: make -C sdk/examples/mk2000/tests
CC     ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I.. -I../../../include
OUT    := ../../../build/mk2000-tests

all: engine store arp clock

engine: | $(OUT)
	$(CC) $(CFLAGS) ../engine.c ../pattern.c engine_test.c -o $(OUT)/engine_test
	$(OUT)/engine_test

arp: | $(OUT)
	$(CC) $(CFLAGS) ../arp.c ../pattern.c arp_test.c -o $(OUT)/arp_test
	$(OUT)/arp_test

store: | $(OUT)
	$(CC) $(CFLAGS) ../store.c ../pattern.c store_test.c -o $(OUT)/store_test
	$(OUT)/store_test

$(OUT):
	mkdir -p $(OUT)

clock: | $(OUT)
	$(CC) $(CFLAGS) ../clock.c clock_test.c -o $(OUT)/clock_test
	$(OUT)/clock_test

.PHONY: all engine store arp clock
