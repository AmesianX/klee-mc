#include <string.h>
#include "klee/ExecutionState.h"
#include "Memory.h"
#include "TLB.h"

using namespace klee;

TLB::TLB(void) : cur_sid(~0) {}

bool TLB::get(ExecutionState& st, uint64_t addr, ObjectPair& out_op)
{
	ObjectPair	*op;

	useState(&st);

	op = &obj_cache[(addr / TLB_PAGE_SZ) % TLB_OBJCACHE_ENTS];
	if (op->first == NULL)
		return false;

	/* full boundary checks are done in the MMU. */
	if (op->first->isInBounds(addr, 1) == false)
		return false;

	out_op = *op;
	return true;
}

void TLB::put(ExecutionState& st, ObjectPair& op)
{
	useState(&st);
	obj_cache[(op.first->address / TLB_PAGE_SZ) % TLB_OBJCACHE_ENTS] = op;
}

/* if address space changed, reset TLB */
void TLB::useState(const ExecutionState* st)
{
	if (	st->getSID() == cur_sid &&
		cur_gen == st->addressSpace.getGeneration())
		return;

	memset(&obj_cache, 0, sizeof(obj_cache));
	cur_sid = st->getSID();
	cur_gen = st->addressSpace.getGeneration();
}

void TLB::invalidate(uint64_t addr)
{ obj_cache[(addr / TLB_PAGE_SZ) % TLB_OBJCACHE_ENTS].first = NULL; }
