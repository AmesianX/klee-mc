// RUN: gcc %s -O0 -I../../../include/  -o %t1
// RUN: klee-mc -pipe-solver -sym-mmu-type=objwide -sconc-mmu-type=cnull -use-sym-mmu - ./%t1 2>%t1.err >%t1.out
// RUN: ls klee-last | not grep ptr.err
// RUN: ls klee-last | grep ktest.gz | wc -l | grep 2
#include "klee/klee.h"
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	uint64_t	c;
	uint64_t	*p64;
	void		*x;

	if (read(0, &c, sizeof(c)) != sizeof(c)) return 0;
	p64 = (void*)c;

	x = malloc(16);
	free(x);
	((char*)x)[c & 0xf] = 10;

	return 0;
}