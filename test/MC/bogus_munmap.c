// RUN: gcc %s -O0 -o %t1
//
// Don't crash on a bogus unmap
// RUN: klee-mc - ./%t1 2>%t1.err >%t1.out
//
// It should exit nicely
// RUN: grep "exitcode" %t1.err
//
// Don't relay a symbolic, this should be well-defined.
// RUN: ls klee-last | grep ktest | wc -l | grep 1

#include <sys/mman.h>

int main(int argc, char* argv[])
{
	int	err;

	err = munmap((void*)0x123456000, 4096);
	if (err == 0) return -1;

	return 0;
}