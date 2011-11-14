#ifndef EXESTATESYMHOOK_H
#define EXESTATESYMHOOK_H

#include "klee/Internal/ADT/ImmutableSet.h"
#include "ExeStateVex.h"

struct breadcrumb;

namespace llvm
{
class Function;
}

namespace klee
{
class MemoryObject;

#define ESVSymHookBuilder DefaultExeStateBuilder<ESVSymHook>

class ESVSymHook : public ExeStateVex
{
friend class ESVSymHookBuilder;
private:
	ESVSymHook &operator=(const ESVSymHook&);

protected:
	ESVSymHook() : cur_watched_f(0) {}
  	ESVSymHook(KFunction *kf)
	: ExeStateVex(kf), cur_watched_f(0) {}
	ESVSymHook(const std::vector<ref<Expr> > &assumptions)
	: ExeStateVex(assumptions), cur_watched_f(0) {}
	ESVSymHook(const ESVSymHook& src);

public:
	virtual ExecutionState* copy(void) const { return copy(this); }
	virtual ExecutionState* copy(const ExecutionState* es) const
	{ return new ESVSymHook(*(static_cast<const ESVSymHook*>(es))); }

	virtual ~ESVSymHook() {}

	void enterWatchedFunc(llvm::Function* f, ref<Expr> p, uint64_t);
	void unwatch(void) { cur_watched_f = NULL; }

	uint64_t getWatermark(void) const { return enter_stack_watermark; }
	llvm::Function* getWatchedFunc(void) const { return cur_watched_f; }
	bool isWatched(void) const { return (cur_watched_f != NULL); }
	const ref<Expr>& getWatchParam(void) const { return param; }

	void addHeapPtr(uint64_t);
	void rmvHeapPtr(uint64_t);
	bool hasHeapPtr(uint64_t) const;

private:
	/* need two extra address spaces:
	 * 'blessed' mmap objects-- MO's that were created outside malloc()
	 * 'heap' objects -- ranges allocated by malloc()
	 *
	 * Inside malloc/free-- may access everything
	 * Outside malloc/free -- may only access blessed or heap
	 */
	ImmutableSet<uint64_t>	heap_set;

	llvm::Function*	cur_watched_f;
	ref<Expr>	param;
	uint64_t	enter_stack_watermark;
};

}

#endif
