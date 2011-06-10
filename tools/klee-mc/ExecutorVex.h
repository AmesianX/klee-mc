#ifndef KLEE_EXECUTOR_VEX_H
#define KLEE_EXECUTOR_VEX_H

#include "../../lib/Core/Executor.h"

#include <hash_map>
#include <assert.h>

class GuestState;
class VexXlate;
class VexSB;
class VexFCache;

namespace llvm
{
class Function;
}

namespace klee {  
class KModule;
class MemoryObject;
class ObjectState;

// ugh g++, you delicate garbage
typedef __gnu_cxx::hash_map<uintptr_t /* Func*/, VexSB*> func2vsb_map;

class ExecutorVex : public Executor
{
public:
	ExecutorVex(
		const InterpreterOptions &opts,
		InterpreterHandler *ie,
		GuestState* gs);
	virtual ~ExecutorVex(void);

	const llvm::Module * setModule(
		llvm::Module *module,
		const ModuleOptions &opts) { assert (0 == 1 && "BUSTED"); }

	void runFunctionAsMain(
		llvm::Function *f,
		int argc,
		char **argv,
		char **envp) { assert (0 == 1 && "LOL"); }

	void runImage(void);

protected:
  	virtual void executeInstruction(
		ExecutionState &state, KInstruction *ki);
	virtual void executeCallNonDecl(
		ExecutionState &state, 
		KInstruction *ki,
		Function *f,
		std::vector< ref<Expr> > &arguments) { assert (0 == 1 && "STUB"); }
	virtual void instRet(ExecutionState &state, KInstruction *ki);
  	virtual void run(ExecutionState &initialState);

	virtual const Cell& eval(
		KInstruction *ki,
		unsigned index,
		ExecutionState &state) const;

	virtual void callExternalFunction(
		ExecutionState &state,
		KInstruction *target,
		Function *function,
		std::vector< ref<Expr> > &arguments) { assert (0 == 1 && "STUB"); }
private:
	void bindModuleConstants(void);
	void bindKFuncConstants(KFunction *kfunc);
	void bindModuleConstTable(void);
	void initializeGlobals(ExecutionState& state);
	void prepState(ExecutionState* state, llvm::Function*);
	void setupRegisterContext(ExecutionState* state, llvm::Function* f);
	void setupProcessMemory(ExecutionState* state, llvm::Function* f);
	llvm::Function* getFuncFromAddr(uint64_t addr);
	void allocGlobalVariableDecl(
		ExecutionState& state,
		const GlobalVariable& gv);
	void allocGlobalVariableNoDecl(
		ExecutionState& state,
		const GlobalVariable& gv);

	void handleXferCall(
		ExecutionState& state, KInstruction* ki);
	void handleXferSyscall(
		ExecutionState& state, KInstruction* ki);
	void handleXferReturn(
		ExecutionState& state, KInstruction* ki);
	void handleXferJmp(
		ExecutionState& state, KInstruction* ki);
	void jumpToKFunc(ExecutionState& state, KFunction* kf);

	struct XferStateIter
	{
		ref<Expr>	v;
		ExecutionState* free;
		llvm::Function*	f;
		StatePair 	res;
		bool		first;
	};
	void xferIterInit(
		struct XferStateIter& iter,
		ExecutionState* state,
		KInstruction* ki);
	bool xferIterNext(struct XferStateIter& iter);

	func2vsb_map	func2vsb_table;
	GuestState	*gs;
	VexFCache	*xlate_cache;
	MemoryObject	*state_regctx_mo;
};

}
#endif
