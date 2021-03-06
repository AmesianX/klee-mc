#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_os_ostream.h>
#include "Executor.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/KModule.h"
#include "SoftMMUHandlers.h"
#include "SymMMU.h"

using namespace klee;

namespace {
	llvm::cl::opt<std::string>
	SymMMUType(
		"sym-mmu-type",
		llvm::cl::desc("Suffix for symbolic MMU operations."),
		llvm::cl::init("objwide" /*uniqptr */));
};

void SymMMU::initModule(Executor& exe, const std::string& mmu_type)
{
	mh = std::make_unique<SoftMMUHandlers>(exe, mmu_type);
	assert (mh != NULL);

	std::cerr << "[SymMMU] Using '" << mmu_type << "'\n";
	if (mh->getCleanup()) exe.addFiniFunction(mh->getCleanup()->function);
	if (mh->getInit()) exe.addInitFunction(mh->getInit()->function);
}

SymMMU::SymMMU(Executor& exe)
: MMU(exe)
{ initModule(exe, SymMMUType); }

SymMMU::SymMMU(Executor& exe, const std::string& type)
: MMU(exe)
{ initModule(exe, type); }


bool SymMMU::exeMemOp(ExecutionState &state, MemOp& mop)
{
	KFunction		*kf;
	Expr::Width		w(mop.getType(exe.getKModule()));
	std::vector<ref<Expr> >	args;

	if (mop.isWrite) {
		kf = mh->getStore(w);
		if (kf == NULL) {
			if (mop.target && mop.target->getInst())  {
				llvm::raw_os_ostream os(std::cerr);
				os << "[SymMMU] TARGET="
					<< *(mop.target->getInst()) << '\n'
					<< "[SymMMU] Func=\n"
					<< *(mop.target->getInst()->
						getParent()->getParent());
			}
			std::cerr << "\n[SymMMU] BAD WIDTH! W=" << w << '\n';
			assert (0 == 1 && "BAD WIDTH");
		}

		args.push_back(mop.address);
		if (w == 128) {
			args.push_back(MK_EXTRACT(mop.value, 0, 64));
			args.push_back(MK_EXTRACT(mop.value, 64, 64));
		} else
			args.push_back(mop.value);
		exe.executeCallKFunc(state, *kf, args);

		sym_w_c++;
		return true;
	}

	kf = mh->getLoad(w);
	assert (kf != NULL && "BAD WIDTH");

	args.push_back(mop.address);
	exe.executeCallKFunc(state, *kf, args);

	sym_r_c++;
	return true;
}

void SymMMU::signal(ExecutionState& state, void* addr, uint64_t len)
{
	std::vector<ref<Expr> >	args;
	KFunction		*kf(mh->getSignal());

	if (kf == NULL) return;

	args.push_back(MK_CONST((uintptr_t)addr, 64));
	args.push_back(MK_CONST(len, 64));

	/* XXX this doesn't work really right after Executor::makeSymbolic;
	 * it returns a pointer to symbolic data which trashes the args. I think
	 * the call is meant to go after the return result binding. Not sure.
	 *
	 * Right now this only works for klee-mc because I put the call in 
	 * make_sym_range. */
	exe.executeCallKFunc(state, *kf, args);
}
