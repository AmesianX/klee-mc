/*
 * branches three states: min value, max value, and in between
 */
#include "klee/klee.h"
#include "mmu.h"

MMUOPS_S_EXTERN(vminmaxc);
MMUOPS_S_EXTERN(vminmax);

static uint64_t	values_y[3];

#define CHK_V_FAST(t, v)	do {	\
	int	n = klee_get_values(v, values_y, 3); \
	if (n == 1) {				\
		klee_assume_eq(v, values_y[0]);	\
		v = values_y[0];		\
		break; }			\
	if (n == 2) {	\
		if (v == values_y[0]) { v = values_y[0]; break; }	\
		klee_assume_eq(v, values_y[1]);	\
		break; }	\
	uint64_t c = klee_min_value(v);		\
	if (v == c) { klee_assume_eq(v, c); v = c; break; }	\
	c = klee_max_value(v);		\
	if (v == c) { klee_assume_eq(v, c); v = c; break; }	\
	} while (0)

#define CHK_V(t, v)	do {	\
	t c = klee_get_value(v);	\
	if (klee_valid_eq(v, c)) {	\
		klee_assume_eq(v, c);	\
		v = c;			\
		break; }		\
	c = klee_min_value(v);		\
	if (v == c) { klee_assume_eq(v, c); v = c; break; }	\
	c = klee_max_value(v);		\
	if (v == c) { klee_assume_eq(v, c); v = c; break; }	\
	} while (0)

#define MMU_LOADC(x,y)		\
y mmu_load_##x##_vminmaxc(void* addr)	\
{	y ret = MMU_FWD_LOAD(vminmaxc,x,addr);	\
	CHK_V_FAST(y, ret);	\
	return ret; }

#define MMU_STOREC(x,y)			\
void mmu_store_##x##_vminmaxc(void* addr, y v)	\
{	CHK_V_FAST(y, v);				\
	return mmu_store_##x##_cnulltlb(addr, v); }

#define MMU_LOAD(x,y)		\
y mmu_load_##x##_vminmax(void* addr)	\
{	y ret = MMU_FWD_LOAD(vminmax,x,addr);	\
	CHK_V_FAST(y, ret);			\
	return ret; }

#define MMU_STORE(x,y)			\
void mmu_store_##x##_vminmax(void* addr, y v)	\
{	CHK_V_FAST(y, v);			\
	MMU_FWD_STORE(vminmax,x,addr,v); }

#undef MMU_ACCESS
#define MMU_ACCESS(x,y)			\
	MMU_LOAD(x,y)	MMU_LOADC(x,y)	\
	MMU_STORE(x,y)	MMU_STOREC(x,y)

MMU_ACCESS(8, uint8_t);
MMU_ACCESS(16, uint16_t);
MMU_ACCESS(32, uint32_t);
MMU_ACCESS(64, uint64_t);

void mmu_store_128_vminmax(void* addr, __uint128_t v)
{ MMU_FWD_STORE(vminmax,128,addr,v); }
__uint128_t mmu_load_128_vminmax(void* addr)
{ return MMU_FWD_LOAD(vminmax,128,addr); }

void mmu_store_128_vminmaxc(void* addr, __uint128_t v)
{ MMU_FWD_STORE(vminmaxc,128,addr,v); }
__uint128_t mmu_load_128_vminmaxc(void* addr)
{ return MMU_FWD_LOAD(vminmaxc,128,addr); }

DECL_MMUOPS_S(vminmax);
DECL_MMUOPS_S(vminmaxc);
