//===-- KModule.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <llvm/Linker.h>
#include <llvm/LLVMContext.h>

#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Instructions.h>
#include <llvm/Module.h>
#include <llvm/PassManager.h>
#include <llvm/ValueSymbolTable.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/Path.h>
#include <llvm/DataLayout.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>


#include "klee/Internal/Module/KModule.h"
#include "../lib/Core/Context.h"
#include "../lib/Core/Executor.h"

#include "Passes.h"
#include "HookPass.h"

#include "llvm/GlobalVariable.h"
#include "klee/Common.h"
#include "klee/Interpreter.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Support/ModuleUtil.h"

#include "static/Sugar.h"

#include <fstream>
#include <sstream>

using namespace klee;
using namespace llvm;


namespace {
  enum SwitchImplType {
    eSwitchTypeSimple,
    eSwitchTypeLLVM,
    eSwitchTypeInternal
  };

  cl::list<std::string> MergeAtExit("merge-at-exit");

  cl::opt<bool>
  TruncateSourceLines(
  	"truncate-source-lines",
        cl::desc("Truncate long lines in the output source (kcachegrind?)"));

  cl::opt<bool>
  OutputSource(
  	"output-source",
        cl::desc("Write the assembly for the final transformed source"));

  cl::opt<bool>
  OutputModule(
  	"output-module",
	cl::desc("Write the bitcode for the final transformed module"));

  cl::opt<SwitchImplType>
  SwitchType("switch-type", cl::desc("Select the implementation of switch"),
             cl::values(clEnumValN(eSwitchTypeSimple, "simple",
                                   "lower to ordered branches"),
                        clEnumValN(eSwitchTypeLLVM, "llvm",
                                   "lower using LLVM"),
                        clEnumValN(eSwitchTypeInternal, "internal",
                                   "execute switch internally"),
                        clEnumValEnd),
             cl::init(eSwitchTypeInternal));

  cl::opt<bool> OutputFunctions(
  	"output-funcs",
	cl::desc("Write the .ll for each function"),
	cl::init(false));

  cl::opt<bool>
  DebugPrintEscapingFunctions(
  	"debug-print-escaping-functions",
         cl::desc("Print functions whose address is taken."));
  cl::opt<bool>
  CountModuleCoverage(
	"count-mod-cov",
	cl::desc("Include module instructions in uncovered count"));

  cl::opt<bool>
  UseSoftFP(
	"use-softfp",
	cl::desc("Use soft-floating point to convert fp to int."));

  cl::opt<bool> UseHookPass("use-hookpass");

  cl::opt<unsigned>
  OptimizeKModule(
  	"optimize",
	cl::desc("Optimize before execution"),
	cl::init(0));
}

KModule::KModule(
	Module *_module,
	const Interpreter::ModuleOptions &_opts)
: module(_module)
, dataLayout(new DataLayout(module))
, kleeMergeFn(0)
, infos(0)
, constantTable(0)
, fpm(new FunctionPassManager(_module))
, opts(_opts)
, updated_funcs(0)
{}

KModule::~KModule()
{
	delete infos;

	delete fpm;

	foreach (it, functions.begin(), functions.end())
		delete *it;

	foreach (it, constantMap.begin(), constantMap.end())
		delete (*it).second;
	constantMap.clear();

	delete dataLayout;
	delete module;
}

/***/

namespace llvm {
extern void Optimize(Module*);
}

// what a hack
Function *getStubFunctionForCtorList(
	Module *m,
	GlobalVariable *gv,
	std::string name)
{
	assert(!gv->isDeclaration() && !gv->hasInternalLinkage() &&
	 "do not support old LLVM style constructor/destructor lists");

	std::vector<Type*>	nullary;
	Function		*fn;
	BasicBlock		*bb;

	fn = Function::Create(
		FunctionType::get(
			Type::getVoidTy(getGlobalContext()), nullary, false),
		GlobalVariable::InternalLinkage,
		name,
		m);

	bb = BasicBlock::Create(getGlobalContext(), "entry", fn);

	// From lli:
	// Should be an array of '{ int, void ()* }' structs.  The first value is
	// the init priority, which we ignore.
	ConstantArray *arr = dyn_cast<ConstantArray>(gv->getInitializer());
	if (!arr) goto gen_return;

	for (unsigned i=0; i<arr->getNumOperands(); i++) {
		ConstantStruct *cs = cast<ConstantStruct>(arr->getOperand(i));

		assert(	cs->getNumOperands()==2 &&
			"unexpected elem in ctor init list");

		Constant *fp = cs->getOperand(1);
		if (fp->isNullValue()) continue;

		if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(fp))
			fp = ce->getOperand(0);

		if (Function *f = dyn_cast<Function>(fp)) {
			CallInst::Create(f, "", bb);
		} else {
			assert(0 && "unable to get fptr from ctor init list");
		}
	}


gen_return:
	ReturnInst::Create(getGlobalContext(), bb);

	return fn;
}

static void injectStaticConstructorsAndDestructors(Module *m)
{
	GlobalVariable *ctors = m->getNamedGlobal("llvm.global_ctors");
	GlobalVariable *dtors = m->getNamedGlobal("llvm.global_dtors");

	if (!ctors && !dtors) return;

	Function *mainFn = m->getFunction("main");
	assert(mainFn && "unable to find main function");

	if (ctors) {
		CallInst::Create(
			getStubFunctionForCtorList(m, ctors, "klee.ctor_stub"),
			"",
			mainFn->begin()->begin());
	}

	if (dtors) {
		Function *dtorStub;
		dtorStub = getStubFunctionForCtorList(m, dtors, "klee.dtor_stub");
		foreach (it, mainFn->begin(), mainFn->end()) {
			if (!isa<ReturnInst>(it->getTerminator())) continue;
			CallInst::Create(dtorStub, "", it->getTerminator());
		}
	}
}

void KModule::dumpFuncs(std::ostream& os) const
{
	foreach (it, module->begin(), module->end())
		os << it->getName().str() << '\n';
}

static void forceImport(Module *m, const char *name, Type *retType, ...)
{
	// If module lacks an externally visible symbol for the name then we
	// need to create one. We have to look in the symbol table because
	// we want to check everything (global variables, functions, and
	// aliases).
	//
	// Note that the function will stay a declaration without
	// linking in the library after calling forceImport.

	Value *v = m->getValueSymbolTable().lookup(name);
	GlobalValue *gv = dyn_cast_or_null<GlobalValue>(v);

	if (gv && !gv->hasInternalLinkage()) return;
	va_list ap;

	va_start(ap, retType);
	std::vector<Type *> argTypes;
	while (Type *t = va_arg(ap, Type*))
		argTypes.push_back(t);
	va_end(ap);

	m->getOrInsertFunction(
		name, FunctionType::get(retType, argTypes, false));
}

void KModule::prepareMerge(InterpreterHandler *ih)
{
	Function *mergeFn;

	mergeFn = module->getFunction("klee_merge");
	if (!mergeFn) {
		llvm::FunctionType *Ty;
		Ty = FunctionType::get(
			Type::getVoidTy(getGlobalContext()),
			std::vector<Type*>(), false);
		mergeFn = Function::Create(
			Ty,
			GlobalVariable::ExternalLinkage,
			"klee_merge",
			module);
	}


	foreach (it, MergeAtExit.begin(), MergeAtExit.end())
		addMergeExit(mergeFn, *it);
}

void KModule::addMergeExit(Function* mergeFn, const std::string& name)
{
	Function	*f;
	BasicBlock	*exit;
	PHINode		*result;

	f = module->getFunction(name);
	if (!f) {
		klee_error("can't insert merge-at-exit for: %s (can't find)",
		name.c_str());
	} else if (f->isDeclaration()) {
		klee_error("can't insert merge-at-exit for: %s (external)",
		name.c_str());
	}

	exit = BasicBlock::Create(getGlobalContext(), "exit", f);
	result = NULL;
	if (!f->getReturnType()->isVoidTy())
		result = PHINode::Create(f->getReturnType(), 0, "retval", exit);

	CallInst::Create(mergeFn, "", exit);
	ReturnInst::Create(getGlobalContext(), result, exit);

	llvm::errs() << "KLEE: adding klee_merge at exit of: " << name << "\n";
	foreach (bbit, f->begin(), f->end()) {
		Instruction *i;

		if (&*bbit == exit)
			continue;

		i = bbit->getTerminator();

		if (i->getOpcode() != Instruction::Ret)
			continue;

		if (result) {
			result->addIncoming(i->getOperand(0), bbit);
		}
		i->eraseFromParent();
		BranchInst::Create(exit, bbit);
	}
}

void KModule::outputTruncSource(
	std::ostream* os, llvm::raw_os_ostream* ros) const
{
	std::string			string;
	llvm::raw_string_ostream	rss(string);
	bool				truncated = false;
	const char			*position;

	rss << *module;
	rss.flush();
	position = string.c_str();

	for (;;) {
		const char *end = index(position, '\n');
		unsigned count;
		if (!end) {
			*ros << position;
			break;
		}

		count = (end - position) + 1;
		if (count<255) {
			ros->write(position, count);
		} else {
			ros->write(position, 254);
			*ros << "\n";
			truncated = true;
		}

		position = end+1;
	}
}

void KModule::outputSource(InterpreterHandler* ih)
{
	std::ostream		*os;
	llvm::raw_os_ostream	*ros;

	os = ih->openOutputFile("assembly.ll");
	assert(os && os->good() && "unable to open source output");


	ros = new llvm::raw_os_ostream(*os);

	// We have an option for this in case the user wants a .ll they
	// to work with kcachegrind
	if (TruncateSourceLines)
		outputTruncSource(os, ros);
	else
		*ros << *module;

	delete ros;
	delete os;
}

void KModule::dumpModule(void)
{
	// Write out the .ll assembly file. We truncate long lines to work
	// around a kcachegrind parsing bug (it puts them on new lines), so
	// that source browsing works.
	if (OutputSource)
		outputSource(ih);

	if (OutputModule) {
		std::ostream *f = ih->openOutputFile("final.bc");
		llvm::raw_os_ostream* rfs = new llvm::raw_os_ostream(*f);
		WriteBitcodeToFile(module, *rfs);
		delete rfs;
		delete f;
	}
}

void KModule::addModule(Module* in_mod)
{
	std::string	err;
	std::string	mod_name;
	bool		isLinked;

	mod_name = in_mod->getModuleIdentifier();

	/* insertion failed? already loaded */
	if (addedModules.insert(mod_name).second == false) {
		std::cerr << "[KModule] Already loaded \""<< mod_name <<"\"\n";
		return;
	}

	std::cerr << "[KModule] Adding module \"" << mod_name << "\"\n";

	isLinked = Linker::LinkModules(
		module, in_mod, Linker::PreserveSource, &err);

	foreach (it, in_mod->begin(), in_mod->end()) {
		Function	*kmod_f;
		KFunction	*kf;

		kmod_f = module->getFunction(it->getName().str());
		assert (kmod_f != NULL);

		kf = addFunction(kmod_f);
		if (kf) kf->trackCoverage = CountModuleCoverage;
	}

	dumpModule();
//	assert (isLinked);
}

// Inject checks prior to optimization... we also perform the
// invariant transformations that we will end up doing later so that
// optimize is seeing what is as close as possible to the final
// module.
void KModule::injectRawChecks()
{
	PassManager pm;
	pm.add(new RaiseAsmPass(module));

	// pm.add(createLowerAtomicPass());

	// There was a bug in IntrinsicLowering which caches values which
	// may eventually  deleted (via RAUW).
	// This should now be fixed in LLVM
	pm.add(new IntrinsicCleanerPass(this, *dataLayout, false));
	pm.run(*module);
}

// Finally, run the passes that maintain invariants we expect during
// interpretation. We run the intrinsic cleaner just in case we
// linked in something with intrinsics but any external calls are
// going to be unresolved. We really need to handle the intrinsics
// directly I think?
void KModule::passEnforceInvariants(void)
{
	PassManager pm;

	pm.add(createCFGSimplificationPass());
	switch(SwitchType) {
	case eSwitchTypeInternal: break;
	case eSwitchTypeSimple: pm.add(new LowerSwitchPass()); break;
	case eSwitchTypeLLVM:  pm.add(createLowerSwitchPass()); break;
	// default: klee_error("invalid --switch-type");
	}

	// pm.add(createLowerAtomicPass());

	pm.add(new IntrinsicCleanerPass(this, *dataLayout));
	pm.add(new PhiCleanerPass());
	pm.run(*module);
}

void KModule::prepare(InterpreterHandler *in_ih)
{
	ih = in_ih;

	if (!MergeAtExit.empty())
		prepareMerge(ih);

	loadIntrinsicsLib();
	injectRawChecks();

	infos = new InstructionInfoTable(module);
	if (OptimizeKModule) Optimize(module);

	// Needs to happen after linking (since ctors/dtors can be modified)
	// and optimization (since global optimization can rewrite lists).
	injectStaticConstructorsAndDestructors(module);

	passEnforceInvariants();

	// For cleanliness see if we can discard any of the functions we
	// forced to import during loadIntrinsicsLib.
	// We used to remove forced functions here. Not any more-- we load
	//  f = module->getFunction("memcpy");
	//  if (f && f->use_empty()) f->eraseFromParent();
	dumpModule();

	kleeMergeFn = module->getFunction("klee_merge");

	setupFunctionPasses();

	/* Build shadow structures */
	foreach (it, module->begin(), module->end())
		addFunction(it);

	if (DebugPrintEscapingFunctions && !escapingFunctions.empty()) {
		llvm::errs() << "KLEE: escaping functions: [";
		foreach (it, escapingFunctions.begin(), escapingFunctions.end())
		{
			llvm::errs() << (*it)->getName() << ", ";
		}
		llvm::errs() << "]\n";
	}
}

void KModule::setupFunctionPasses(void)
{
	if (OptimizeKModule) {
		PassManagerBuilder	pmb;
		pmb.OptLevel = OptimizeKModule;
		pmb.populateFunctionPassManager(*fpm);
	}

	fpm->add(new RaiseAsmPass(module));
	fpm->add(createLowerAtomicPass());
	fpm->add(new IntrinsicCleanerPass(this, *dataLayout));
	fpm->add(new PhiCleanerPass());

	if (UseHookPass) fpm->add(new HookPass(this));
	if (UseSoftFP) fpm->add(new SoftFPPass(this));
}

void KModule::addFunctionPass(llvm::FunctionPass* fp) { fpm->add(fp); }

KFunction* KModule::addUntrackedFunction(llvm::Function* f)
{
	KFunction	*kf;

	kf = addFunction(f);
	if (kf != NULL)
		kf->trackCoverage = false;

	return kf;
}

/* add a function that hasn't been cleaned up by any of our passes */
KFunction* KModule::addFunction(Function* f)
{
	bool		changed;
	unsigned	old_sz;

	if (f->isDeclaration()) return NULL;

	old_sz = f->size();
	changed = fpm->run(*f);
	if (changed) updated_funcs++;

	fpm->doFinalization();
	fpm->doInitialization();

	return addFunctionProcessed(f);
}

static void appendFunction(std::ofstream& os, const Function* f)
{
	llvm::raw_os_ostream	*ros;

	ros = new llvm::raw_os_ostream(os);
	*ros << *f;

	delete ros;
}

void KModule::outputFunction(const KFunction *kf)
{
	std::ostream		*os;
	llvm::raw_os_ostream	*ros;

	os = ih->openOutputFile((kf->function->getName().str() + ".ll"));
	assert(os && os->good() && "unable to open source output");

	ros = new llvm::raw_os_ostream(*os);
	*ros << *kf->function;
	delete ros;
	delete os;
}

KFunction* KModule::addFunctionProcessed(Function* f)
{
	KFunction	*kf;

	if (f->isDeclaration()) return NULL;

	kf = new KFunction(f, this);
	for (unsigned i=0; i < kf->numInstructions; ++i) {
		KInstruction *ki = kf->instructions[i];
		ki->setInfo(&infos->getInfo(ki->getInst()));
	}

	functions.push_back(kf);
	functionMap.insert(std::make_pair(f, kf));
	/* Compute various interesting properties */
	if (functionEscapes(kf->function))
		escapingFunctions.insert(kf->function);

	if (OutputSource) {
		std::ofstream os(
			ih->getOutputFilename("assembly.ll").c_str(),
			std::ios::binary | std::ios::app);
		appendFunction(os, f);
	}

	if (OutputFunctions)
		outputFunction(kf);

	return kf;
}

KConstant* KModule::getKConstant(Constant *c)
{
	std::map<llvm::Constant*, KConstant*>::iterator it = constantMap.find(c);
	if (it == constantMap.end()) return NULL;
	return it->second;
}

unsigned KModule::getConstantID(Constant *c, KInstruction* ki)
{
	KConstant *kc = getKConstant(c);

	if (kc) return kc->id;

	unsigned id = constants.size();
	kc = new KConstant(c, id, ki);
	constantMap.insert(std::make_pair(c, kc));
	constants.push_back(c);
	return id;
}

KConstant::KConstant(llvm::Constant* _ct, unsigned _id, KInstruction* _ki)
: ct(_ct)
, id(_id)
, ki(_ki)
{}

KFunction* KModule::getKFunction(llvm::Function* f) const
{
	func2kfunc_ty::const_iterator it;

	it = functionMap.find(f);
	if (it == functionMap.end()) return NULL;

	return it->second;
}

KFunction* KModule::getKFunction(const char* fname) const
{
	Function	*f;

	f = module->getFunction(fname);
	if (f == NULL) return NULL;

	return getKFunction(f);
}


namespace klee { extern Module* getBitcodeModule(const char* s); }

void KModule::loadIntrinsicsLib()
{
	// Force importing functions required by intrinsic lowering. Kind of
	// unfortunate clutter when we don't need them but we won't know
	// that until after all linking and intrinsic lowering is
	// done. After linking and passes we just try to manually trim these
	// by name. We only add them if such a function doesn't exist to
	// avoid creating stale uses.

	llvm::Type *i8Ty = Type::getInt8Ty(getGlobalContext());

	forceImport(
		module, "memcpy", PointerType::getUnqual(i8Ty),
		PointerType::getUnqual(i8Ty),
		PointerType::getUnqual(i8Ty),
		dataLayout->getIntPtrType(getGlobalContext()), (Type*) 0);
	forceImport(
		module, "memmove", PointerType::getUnqual(i8Ty),
		PointerType::getUnqual(i8Ty),
		PointerType::getUnqual(i8Ty),
		dataLayout->getIntPtrType(getGlobalContext()), (Type*) 0);
	forceImport(
		module, "memset", PointerType::getUnqual(i8Ty),
		PointerType::getUnqual(i8Ty),
		Type::getInt32Ty(getGlobalContext()),
		dataLayout->getIntPtrType(getGlobalContext()), (Type*) 0);

	// FIXME: Missing force import for various math functions.

	llvm::sys::Path path(getLibraryDir());

	path.appendComponent("libkleeRuntimeIntrinsic.bc");
	Module	*m = klee::getBitcodeModule(path.str().c_str());
	std::string err;

	assert (m != NULL);
	foreach(it, m->begin(), m->end()) {
		Function	*f;
		f = module->getFunction(it->getName().str());
		if (f == NULL)
			continue;
		f->deleteBody();
	}

	Linker::LinkModules(module, m, Linker::DestroySource, &err);
	if (!err.empty()) {
		std::cerr << "err: " << err << '\n';
		exit(1);
	}
////	path.appendComponent("libkleeRuntimeIntrinsic.bca");
//	module = linkWithLibrary(module, path.c_str());
}

void KModule::bindModuleConstants(Executor* exe)
{
	foreach (it, kfuncsBegin(), kfuncsEnd())
		bindKFuncConstants(exe, *it);

	bindModuleConstTable(exe);
}

void KModule::bindKFuncConstants(Executor* exe, KFunction* kf)
{
	for (unsigned i=0; i<kf->numInstructions; ++i)
		bindInstructionConstants(exe, kf->instructions[i]);
}

void KModule::bindInstructionConstants(Executor* exe, KInstruction *KI)
{
	KGEPInstruction* kgepi;

	kgepi = dynamic_cast<KGEPInstruction*>(KI);
	if (kgepi == NULL)
		return;

	kgepi->resolveConstants(this, exe->getGlobals());
}

void KModule::bindModuleConstTable(Executor* exe)
{
	unsigned int	old_ct_sz;

	old_ct_sz = constantTable.size();
	for (unsigned i = old_ct_sz; i < constants.size(); i++) {
		Cell	c;
		c.value = exe->evalConstant(constants[i]);
		constantTable.push_back(c);
	}
}

unsigned KModule::getWidthForLLVMType(Type* type) const
{ return dataLayout->getTypeSizeInBits(type); }

/* return a function which calls every func in list of nullary functions */
KFunction* KModule::buildListFunc(
	const std::vector<llvm::Function*>& kf,
	const char* name)
{
	FunctionType	*ft;
	BasicBlock	*bb;
	Function	*f;
	Type		*retType;

	retType = Type::getVoidTy(getGlobalContext());
	ft = FunctionType::get(retType, false);
	f = Function::Create(ft, GlobalValue::ExternalLinkage, name);
	bb = BasicBlock::Create(getGlobalContext(), "entry", f);

	foreach (it, kf.begin(), kf.end())
		CallInst::Create((*it), "", bb);

	ReturnInst::Create(getGlobalContext(), bb);

	return addFunction(f);
}

void KModule::setPrettyName(const llvm::Function* f, const std::string& s)
{ prettyNames[f] = s; }

std::string KModule::getPrettyName(const llvm::Function* f) const
{
	prettymap_ty::const_iterator	it;

	it = prettyNames.find(f);
	if (it == prettyNames.end())
		return f->getName().str();

	return it->second;
}
