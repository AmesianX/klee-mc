#ifndef KLEE_EXECUTOR_VEX_H
#define KLEE_EXECUTOR_VEX_H

#include "collection.h"
#include "guest.h"
#include "../../lib/Core/Executor.h"

#include <iostream>
#include <hash_map>
#include <assert.h>

class VexXlate;
class VexSB;
class VexFCache;

namespace llvm
{
class Function;
class GlobalVariable;
}

namespace klee {
class KModule;
class KFunction;
class MemoryObject;
class ObjectState;
class SyscallSFH;

// ugh g++, you delicate garbage
typedef __gnu_cxx::hash_map<uintptr_t /* Func*/, VexSB*> func2vsb_map;

class ExecutorVex : public Executor
{
public:
	ExecutorVex(
		const InterpreterOptions &opts,
		InterpreterHandler *ie,
		Guest* gs);
	virtual ~ExecutorVex(void);

	const llvm::Module * setModule(
		llvm::Module *module,
		const ModuleOptions &opts) { assert (0 == 1 && "BUSTED"); }

	void runFunctionAsMain(
		llvm::Function *f,
		int argc,
		char **argv,
		char **envp) { assert (0 == 1 && "LOL"); }

	virtual void runImage(void);

	Guest* getGuest(void) { return gs; }
	const Guest* getGuest(void) const { return gs; }

	MemoryManager* getMM(void) { return memory; }
	MemoryObject* allocRegCtx(ExecutionState* state, llvm::Function* f = 0);

	void setRegCtx(ExecutionState& state, MemoryObject* mo);
	ObjectState* getRegObj(ExecutionState&);
	void dumpSCRegs(const std::string& fname);
protected:
	virtual llvm::Function* getFuncByAddr(uint64_t addr);
  	virtual void executeInstruction(
		ExecutionState &state, KInstruction *ki);
	virtual void executeCallNonDecl(
		ExecutionState &state,
		KInstruction *ki,
		llvm::Function *f,
		std::vector< ref<Expr> > &arguments) { assert (0 == 1 && "STUB"); }
	virtual void instRet(ExecutionState &state, KInstruction *ki);
  	virtual void run(ExecutionState &initialState);

	virtual void callExternalFunction(
		ExecutionState &state,
		KInstruction *target,
		llvm::Function *function,
		std::vector< ref<Expr> > &arguments);

	virtual void handleXferSyscall(
		ExecutionState& state, KInstruction* ki);
	void handleXferJmp(
		ExecutionState& state, KInstruction* ki);

	virtual void printStateErrorMessage(
		ExecutionState& state,
		const std::string& message,
		std::ostream& os);

	virtual void handleXfer(ExecutionState& state, KInstruction *ki);
	void updateGuestRegs(ExecutionState& s);

	VexXlate	*xlate;
	Guest		*gs;

private:
	llvm::Function* getFuncByAddrNoKMod(uint64_t guest_addr, bool& is_new);

	void markExit(ExecutionState& state, uint8_t);

	void bindMapping(
		ExecutionState* state,
		llvm::Function* f,
		GuestMem::Mapping m);
	void bindMappingPage(
		ExecutionState* state,
		llvm::Function* f,
		const GuestMem::Mapping& m,
		unsigned int pgnum);

	void initializeGlobals(ExecutionState& state);
	void initGlobalFuncs(void);

	void prepState(ExecutionState* state, llvm::Function*);
	void installFDTInitializers(llvm::Function *init_func);
	void installFDTConfig(ExecutionState& state);
	void makeArgsSymbolic(ExecutionState* state);
	void setupRegisterContext(ExecutionState* state, llvm::Function* f);
	void setupProcessMemory(ExecutionState* state, llvm::Function* f);
	void allocGlobalVariableDecl(
		ExecutionState& state,
		const llvm::GlobalVariable& gv);
	void allocGlobalVariableNoDecl(
		ExecutionState& state,
		const llvm::GlobalVariable& gv);

	void handleXferCall(
		ExecutionState& state, KInstruction* ki);
	void handleXferReturn(
		ExecutionState& state, KInstruction* ki);
	void jumpToKFunc(ExecutionState& state, KFunction* kf);

	struct XferStateIter
	{
		ref<Expr>	v;
		ExecutionState* free;
		llvm::Function*	f;
		uint64_t	f_addr;
		StatePair 	res;
		bool		first;
	};
	void xferIterInit(
		struct XferStateIter& iter,
		ExecutionState* state,
		KInstruction* ki);
	bool xferIterNext(struct XferStateIter& iter);

	void logXferRegisters(ExecutionState& state);

	void logSCRegs(ExecutionState& state);

	func2vsb_map	func2vsb_table;
	VexFCache	*xlate_cache;
	unsigned int	native_code_bytes;

	bool		dump_funcs;
	bool		in_sc;

	KFunction		*kf_scenter;
	SyscallSFH		*sfh;
	
	std::set<uint64_t> legalFunctions;
};

}
#endif
