#ifndef EXESTATEVEX_H
#define EXESTATEVEX_H

#include "klee/ExecutionState.h"

struct breadcrumb;

namespace klee
{

typedef std::vector <std::vector<unsigned char> > RecordLog;
class ExeStateVex : public ExecutionState
{
private:
	ExeStateVex &operator=(const ExeStateVex&);

	RecordLog	bc_log;	/* list of uninterpreted breadcrumbs */
	MemoryObject	*reg_mo;
	unsigned int	syscall_c;

protected:
	ExeStateVex() {}
  	ExeStateVex(KFunction *kf) : ExecutionState(kf) {}
	ExeStateVex(const std::vector<ref<Expr> > &assumptions)
	: ExecutionState(assumptions) {}
	ExeStateVex(const ExeStateVex& src);

	virtual ExecutionState* create(void) const { return new ExeStateVex(); }
	virtual ExecutionState* create(KFunction* kf) const
	{ return new ExeStateVex(kf); }
	virtual ExecutionState* create(
		const std::vector<ref<Expr> >& assumptions) const
	{ return new ExeStateVex(assumptions); }

public:
	static ExeStateVex* make(KFunction* kf)
	{ return new ExeStateVex(kf); }
	static ExeStateVex* make(const std::vector<ref<Expr> >& assumptions)
	{ return new ExeStateVex(assumptions); }


	virtual ExecutionState* copy(void) const { return copy(this); }
	virtual ExecutionState* copy(const ExecutionState* es) const
	{ return new ExeStateVex(*(static_cast<const ExeStateVex*>(es))); }

	virtual ~ExeStateVex() {}

	void recordBreadcrumb(const struct breadcrumb* );
	RecordLog::const_iterator crumbBegin(void) const { return bc_log.begin(); }
	RecordLog::const_iterator crumbEnd(void) const { return bc_log.end(); }

	void recordRegisters(const void* regs, int sz);

	MemoryObject* setRegCtx(MemoryObject* mo)
	{
		MemoryObject	*old_mo;
		old_mo = reg_mo;
		reg_mo = mo;
		return old_mo;
	}

	MemoryObject* getRegCtx(void) const { return reg_mo; }

	void incSyscallCount(void) { syscall_c++; }
	unsigned int getSyscallCount(void) const { return syscall_c; }
};
}

#endif
