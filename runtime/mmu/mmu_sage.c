#include "klee/klee.h"
#include "mmu.h"

#define TAB_SZ	4096

struct hent { uint64_t he_hash; void* he_a; void* he_v; };

struct hent* get_hent(struct hent* htab, void* addr)
{
	uint64_t	h = klee_expr_hash(addr);
	for (unsigned i = 0; i < TAB_SZ; i++) {
		struct hent	*he = &htab[(h+i) % TAB_SZ];

		if (he->he_hash == h && he->he_a == addr) return he;
		if (he->he_v) continue;

		he->he_hash = h;
		he->he_a = addr;
		he->he_v = malloc(128/8);
		klee_make_symbolic(he->he_v, 128/8, "sage");
		return he;
	}

	klee_uerror("ran out of hash entries", "sage.err");
}

#define MMU_LOAD(x,y)			\
struct hent	htab_##x[TAB_SZ];	\
y mmu_load_##x##_sage(void* addr)	\
{ return *((y*)get_hent(htab_##x, addr)->he_v); }

#define MMU_STORE(x,y) void mmu_store_##x##_sage(void* addr, y v) { return; }

MMU_ACCESS_ALL();
DECL_MMUOPS_S(sage);
