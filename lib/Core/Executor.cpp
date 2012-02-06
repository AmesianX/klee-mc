//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


#include "Executor.h"
#include "ExecutorBC.h"	/* for 'interpreter::create()' */
#include "ExeStateManager.h"

#include "Context.h"
#include "CoreStats.h"
#include "ImpliedValue.h"
#include "HeapMM.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "MemUsage.h"
#include "MMU.h"
#include "StatsTracker.h"
#include "Globals.h"

#include "static/Sugar.h"
#include "klee/Common.h"
#include "klee/ExeStateBuilder.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/System/Time.h"

#include <llvm/Support/CallSite.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/LLVMContext.h>
#include <llvm/IntrinsicInst.h>
#include <llvm/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetData.h>

#include <cassert>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <netdb.h>

#include <errno.h>

using namespace llvm;
using namespace klee;

bool	WriteTraces = false;
double	MaxSTPTime;

namespace llvm
{
  namespace cl
  {
    template <>
    class parser<sockaddr_in_opt> : public basic_parser<sockaddr_in_opt> {
    public:
      bool parse(llvm::cl::Option &O, const char *ArgName, const std::string &Arg, sockaddr_in_opt &Val);
      virtual const char *getValueName() const { return "sockaddr_in"; }
    };
  }
}

unsigned MakeConcreteSymbolic;

namespace {
  cl::opt<bool>
  DumpSelectStack("dump-select-stack", cl::init(false));

  cl::opt<unsigned,true>
  MakeConcreteSymbolicProxy(
  	"make-concrete-symbolic",
	cl::desc("Rate at which to make concrete reads symbolic (0=off)"),
	cl::location(MakeConcreteSymbolic),
	cl::init(0));

  cl::opt<bool>
  UsePID("use-pid",
	 cl::desc("Use proportional-integral-derivative state control"),
	 cl::init(false));

  cl::opt<bool>
  ChkConstraints("chk-constraints", cl::init(false));

  cl::opt<bool>
  DumpStatesOnHalt("dump-states-on-halt",
                   cl::init(true));

  cl::opt<bool>
  PreferCex("prefer-cex", cl::init(true));


  cl::opt<bool>
  RandomizeFork("randomize-fork",
                cl::init(false));

  cl::opt<bool>
  DebugPrintInstructions("debug-print-instructions",
                         cl::desc("Print instructions during execution."));
  cl::opt<bool>
  DebugCheckForImpliedValues("debug-check-for-implied-values");

  cl::opt<bool>
  OnlyOutputStatesCoveringNew("only-output-states-covering-new",
                              cl::init(false));

  cl::opt<bool>
  AlwaysOutputSeeds("always-output-seeds",
                              cl::init(true));

  cl::opt<bool>
  EmitAllErrors("emit-all-errors",
                cl::init(false),
                cl::desc("Generate tests cases for all errors "
                         "(default=one per (error,instruction) pair)"));

  cl::opt<bool>
  OnlyReplaySeeds("only-replay-seeds",
                  cl::desc("Discard states that do not have a seed."));

  cl::opt<bool>
  OnlySeed("only-seed",
           cl::desc("Stop execution after seeding is done."));

  cl::opt<bool>
  AllowSeedExtension("allow-seed-extension",
                     cl::desc("Allow extra (unbound) values to become symbolic during seeding."));

  cl::opt<bool>
  ZeroSeedExtension("zero-seed-extension");

  cl::opt<bool>
  AllowSeedTruncation("allow-seed-truncation",
                      cl::desc("Allow smaller buffers than in seeds."));

  cl::opt<bool>
  NamedSeedMatching("named-seed-matching",
                    cl::desc("Use names to match symbolic objects to inputs."));

  cl::opt<double>
  MaxStaticForkPct("max-static-fork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticSolvePct("max-static-solve-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPForkPct("max-static-cpfork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPSolvePct("max-static-cpsolve-pct", cl::init(1.));

  cl::opt<double>
  MaxInstructionTime("max-instruction-time",
                     cl::desc("Only allow a single instruction to take this much time (default=0 (off))"),
                     cl::init(0));

  cl::opt<double>
  SeedTime("seed-time",
           cl::desc("Amount of time to dedicate to seeds, before normal search (default=0 (off))"),
           cl::init(0));

  cl::opt<double,true>
  MaxSTPTimeProxy("max-stp-time",
             cl::desc("Maximum amount of time for a single query (default=120s)"),
	     cl::location(MaxSTPTime),
             cl::init(120.0));

  cl::opt<unsigned int>
  StopAfterNInstructions("stop-after-n-instructions",
                         cl::desc("Stop execution after specified number of instructions (0=off)"),
                         cl::init(0));

  cl::opt<unsigned>
  MaxForks("max-forks",
           cl::desc("Only fork this many times (-1=off)"),
           cl::init(~0u));

  cl::opt<unsigned>
  MaxDepth("max-depth",
           cl::desc("Only allow this many symbolic branches (0=off)"),
           cl::init(0));

  cl::opt<unsigned>
  MaxMemory("max-memory",
            cl::desc("Refuse to fork when more above this about of memory (in MB, 0=off)"),
            cl::init(0));

  cl::opt<bool>
  MaxMemoryInhibit("max-memory-inhibit",
            cl::desc("Inhibit forking at memory cap (vs. random terminate)"),
            cl::init(true));

  // use 'external storage' because also needed by tools/klee/main.cpp
  cl::opt<bool, true>
  WriteTracesProxy("write-traces",
           cl::desc("Write .trace file for each terminated state"),
           cl::location(WriteTraces),
           cl::init(false));

  cl::opt<bool>
  ReplayPathOnly("replay-path-only",
            cl::desc("On replay, terminate states when branches have been exhausted"),
            cl::init(false));

  cl::opt<bool>
  ReplayInhibitedForks(
  	"replay-inhibited-forks",
        cl::desc("Replay fork inhibited path as new state"),
	cl::init(true));

  cl::opt<bool>
  AllExternalWarnings("all-external-warnings");

  cl::opt<bool>
  UseIVC(
  	"use-ivc",
	cl::desc("Implied Value Concretization"),
	cl::init(true));
}


namespace klee {
  RNG theRNG;
}

bool llvm::cl::parser<sockaddr_in_opt>::parse(llvm::cl::Option &O,
     const char *ArgName, const std::string &Arg, sockaddr_in_opt &Val)
{
  // find the separator
  std::string::size_type p = Arg.rfind(':');
  if (p == std::string::npos)
    return O.error("'" + Arg + "' not in format <host>:<port>");

  // read the port number
  unsigned short port;
  if (std::sscanf(Arg.c_str() + p + 1, "%hu", &port) < 1)
    return O.error("'" + Arg.substr(p + 1) + "' invalid port number");

  // resolve server name
  std::string host = Arg.substr(0, p);
  struct hostent* h = gethostbyname(host.c_str());
  if (!h)
    return O.error("cannot resolve '" + host + "' (" + hstrerror(h_errno) + ")");

  // prepare the return value
  Val.str = Arg;
  std::memset(&Val.sin, 0, sizeof(Val.sin));
  Val.sin.sin_family = AF_INET;
  Val.sin.sin_port = htons(port);
  Val.sin.sin_addr = *(struct in_addr*)h->h_addr;

  return false;
}

Executor::Executor(InterpreterHandler *ih)
: kmodule(0)
, globals(0)
, interpreterHandler(ih)
, target_data(0)
, statsTracker(0)
, pathTree(0)
, symPathWriter(0)
, replayOut(0)
, replayPaths(0)
, usingSeeds(0)
, atMemoryLimit(false)
, inhibitForking(false)
, haltExecution(false)
, onlyNonCompact(false)
, initialStateCopy(0)
, ivcEnabled(UseIVC)
, lastMemoryLimitOperationInstructions(0)
, stpTimeout(MaxInstructionTime ?
	std::min(MaxSTPTime,(double)MaxInstructionTime) : MaxSTPTime)
{
	this->solver = Solver::createTimerChain(
		stpTimeout,
		interpreterHandler->getOutputFilename("queries.pc"),
		interpreterHandler->getOutputFilename("stp-queries.pc"));

	memory = MemoryManager::create();
	mmu = new MMU(*this);
	stateManager = new ExeStateManager();
	ExecutionState::setMemoryManager(memory);
	ExeStateBuilder::replaceBuilder(new BaseExeStateBuilder());
}


Executor::~Executor()
{
	if (globals) delete globals;

	std::for_each(timers.begin(), timers.end(), deleteTimerInfo);
	delete stateManager;
	delete mmu;
	delete memory;
	if (pathTree) delete pathTree;
	if (statsTracker) delete statsTracker;
	delete solver;
	ExeStateBuilder::replaceBuilder(NULL);
}

inline void Executor::replaceStateImmForked(
	ExecutionState* os, ExecutionState* ns)
{
	stateManager->replaceStateImmediate(os, ns);
	removePTreeState(os);
}

bool Executor::isStateSeeding(ExecutionState* s)
{
	SeedMapType::iterator it = seedMap.find(s);
	return (it != seedMap.end());
}

bool Executor::isForkingCondition(ExecutionState& current, ref<Expr> condition)
{
	if (isStateSeeding(&current)) return false;

	if (isa<ConstantExpr>(condition)) return false;

	if (	!(MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
		MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.))
	{
		return false;
	}

	if (statsTracker->elapsed() > 60.) return false;

	return true;
}

/* TODO: understand this */
bool Executor::isForkingCallPath(CallPathNode* cpn)
{
  StatisticManager &sm = *theStatisticManager;
  if (MaxStaticForkPct<1. &&
    sm.getIndexedValue(stats::forks, sm.getIndex()) > stats::forks*MaxStaticForkPct)
  {
    return true;
  }

  if (MaxStaticSolvePct<1 &&
    sm.getIndexedValue(stats::solverTime, sm.getIndex()) > stats::solverTime*MaxStaticSolvePct)
  {
    return true;
  }

  /* next conditions require cpn anyway.. */
  if (cpn == NULL) return false;

  if (MaxStaticCPForkPct<1. &&
    (cpn->statistics.getValue(stats::forks) > stats::forks*MaxStaticCPForkPct))
    return true;

  if (MaxStaticCPForkPct<1. &&
    (cpn->statistics.getValue(stats::solverTime) >
      stats::solverTime*MaxStaticCPSolvePct))
    return true;

  return false;
}

Executor::StatePair
Executor::fork(ExecutionState &current, ref<Expr> cond, bool isInternal)
{
	ref<Expr> conds[2];

	// !!! is this the correct behavior?
	if (isForkingCondition(current, cond)) {
		CallPathNode *cpn = current.stack.back().callPathNode;
		if (isForkingCallPath(cpn)) {
			bool			ok;
			ref<ConstantExpr>	value;

			ok = solver->getValue(current, cond, value);
			assert(ok && "FIXME: Unhandled solver failure");

			addConstraint(current, EqExpr::create(value, cond));
			cond = value;
		}
	}

	// set in forkSetupNoSeeding, if possible
	//  conditions[0] = Expr::createIsZero(condition);
	conds[1] = cond;

	StateVector results = fork(current, 2, conds, isInternal, true);
	return std::make_pair(
		results[1] /* first label in br => true */,
		results[0] /* second label in br => false */);
}

bool Executor::forkFollowReplay(ExecutionState& current, struct ForkInfo& fi)
{
	// Replaying non-internal fork; read value from replayBranchIterator
	unsigned targetIndex;

	targetIndex = current.stepReplay();
	fi.wasReplayed = true;

	// Verify that replay target matches current path constraints
	assert(targetIndex <= fi.N && "replay target out of range");
	if (fi.res[targetIndex]) {
		// Suppress forking; constraint will be added to path
		// after forkSetup is complete.
		fi.res.assign(fi.N, false);
		fi.res[targetIndex] = true;

		return true;
	}

	std::stringstream ss;
	ss	<< "hit invalid branch in replay path mode (line="
		<< current.prevPC->getInfo()->assemblyLine
		<< ", expected target="
		<< targetIndex << ", actual targets=";

	bool first = true;
	for(unsigned i = 0; i < fi.N; i++) {
		if (!fi.res[i]) continue;
		if (!first) ss << ",";
		ss << i;
		first = false;
	}
	ss << ")";
	terminateStateOnError(current, ss.str().c_str(), "branch.err");

	for (unsigned i = 0; i < fi.N; i++) {
		if (fi.conditions[i].isNull())
			continue;
	}

	klee_warning("hit invalid branch in replay path mode");
	return false;
}

bool Executor::forkSetupNoSeeding(ExecutionState& current, struct ForkInfo& fi)
{
	if (!fi.isInternal && current.isCompact()) {
		// Can't fork compact states; sanity check
		assert(false && "invalid state");
	}

	if (!fi.isInternal && ReplayPathOnly &&
		current.isReplay && current.isReplayDone())
	{
		// Done replaying this state, so kill it (if -replay-path-only)
		terminateStateEarly(current, "replay path exhausted");
		return false;
	}

	if (!fi.isInternal && current.isReplayDone() == false)
		return forkFollowReplay(current, fi);

	if (fi.validTargets <= 1)  return true;

	// Multiple branch directions are possible; check for flags that
	// prevent us from forking here
	assert(	!replayOut &&
		"in replay mode, only one branch can be true.");

	if (fi.isInternal) return true;

	const char* reason = 0;
	if (MaxMemoryInhibit && atMemoryLimit)
		reason = "memory cap exceeded";
	if (current.forkDisabled)
		reason = "fork disabled on current path";
	if (inhibitForking)
		reason = "fork disabled globally";
	if (MaxForks!=~0u && stats::forks >= MaxForks)
		reason = "max-forks reached";

	// Skipping fork for one of above reasons; randomly pick target
	if (!reason)
		return true;
	if (ReplayInhibitedForks) {
		klee_warning_once(
			reason,
			"forking into compact forms (%s)",
			reason);
		fi.forkCompact = true;
		return true;
	}

	skipAndRandomPrune(fi, reason);
	return true;
}

void Executor::skipAndRandomPrune(struct ForkInfo& fi, const char* reason)
{
	TimerStatIncrementer timer(stats::forkTime);
	unsigned randIndex, condIndex;

	klee_warning_once(
		reason,
		"skipping fork and pruning randomly (%s)",
		reason);

	randIndex = (theRNG.getInt32() % fi.validTargets) + 1;
	for (condIndex = 0; condIndex < fi.N; condIndex++) {
		if (fi.res[condIndex]) randIndex--;
		if (!randIndex) break;
	}

	assert(condIndex < fi.N);
	fi.validTargets = 1;
	fi.res.assign(fi.N, false);
	fi.res[condIndex] = true;
}

void Executor::forkSetupSeeding(
  ExecutionState& current,
  struct ForkInfo& fi)
{
  SeedMapType::iterator it = seedMap.find(&current);
  assert (it != seedMap.end());

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.

  // Assume each seed only satisfies one condition (necessarily true
  // when conditions are mutually exclusive and their conjunction is
  // a tautology).
  // This partitions the seed set for the current state
  foreach (siit, it->second.begin(), it->second.end()) {
    unsigned i;
    for (i = 0; i < fi.N; ++i) {
      ref<ConstantExpr> seedCondRes;
      bool success = solver->getValue(current,
        siit->assignment.evaluate(fi.conditions[i]), seedCondRes);
      assert(success && "FIXME: Unhandled solver failure");
      if (seedCondRes->isTrue()) break;
    }

    // If we didn't find a satisfying condition, randomly pick one
    // (the seed will be patched).
    if (i == fi.N) i = theRNG.getInt32() % fi.N;

    fi.resSeeds[i].push_back(*siit);
  }

  // Clear any valid conditions that seeding rejects
  if ((current.forkDisabled || OnlyReplaySeeds) && fi.validTargets > 1) {
    fi.validTargets = 0;
    for (unsigned i = 0; i < fi.N; i++) {
      if (fi.resSeeds[i].empty()) fi.res[i] = false;
      if (fi.res[i]) fi.validTargets++;
    }
    assert(fi.validTargets && "seed must result in at least one valid target");
  }

  // Remove seeds corresponding to current state
  seedMap.erase(it);

  // !!! it's possible for the current state to end up with no seeds. Does
  // this matter? Old fork() used to handle it but branch() didn't.
}

// !!! for normal branch, conditions = {false,true} so that replay 0,1 reflects
// index
Executor::StateVector
Executor::fork(
	ExecutionState &current,
        unsigned N,
	ref<Expr> conditions[],
        bool isInternal,
	bool isBranch)
{
	SeedMapType::iterator	it;
	ForkInfo		fi(conditions, N);

	fi.timeout = stpTimeout;
	fi.isInternal = isInternal;
	fi.isBranch = isBranch;

	it = seedMap.find(&current);
	fi.isSeeding = it != seedMap.end();
	if (fi.isSeeding) fi.timeout *= it->second.size();

	if (evalForks(current, fi) == false)
		return StateVector(N, NULL);

	// need a copy telling us whether or not we need to add
	// constraints later; especially important if we skip a fork for
	// whatever reason
	fi.feasibleTargets = fi.validTargets;
	assert(fi.validTargets && "invalid set of fork conditions");

	fi.wasReplayed = false;
	if (fi.isSeeding == false) {
		if (!forkSetupNoSeeding(current, fi))
			return StateVector(N, NULL);
	} else {
		forkSetupSeeding(current, fi);
	}

	makeForks(current, fi);
	constrainForks(current, fi);

	return fi.resStates;
}

bool Executor::evalForkBranch(ExecutionState& current, struct ForkInfo& fi)
{
	Solver::Validity	result;
	bool			success;

	assert (fi.isBranch);

	solver->setTimeout(fi.timeout);
	success = solver->evaluate(current, fi.conditions[1], result);
	solver->setTimeout(stpTimeout);

	if (!success) {
		terminateStateEarly(current, "query timed out");
		return false;
	}

	// known => [0] when false, [1] when true
	// unknown => take both routes
	fi.res[0] = (
		result == Solver::False ||
		result == Solver::Unknown);
	fi.res[1] = (
		result == Solver::True ||
		result == Solver::Unknown);

	fi.validTargets = (result == Solver::Unknown) ? 2 : 1;
	if (fi.validTargets > 1 || fi.isSeeding) {
		/* branch on both true and false conditions */
		fi.conditions[0] = Expr::createIsZero(fi.conditions[1]);
	}

	return true;
}

// Evaluate fork conditions
bool Executor::evalForks(ExecutionState& current, struct ForkInfo& fi)
{
	if (fi.isBranch) {
		return evalForkBranch(current, fi);
	}

	assert (fi.isBranch == false);

	for (unsigned int condIndex = 0; condIndex < fi.N; condIndex++) {
		ConstantExpr *CE;
		bool result;

		CE = dyn_cast<ConstantExpr>(fi.conditions[condIndex]);
		// If condition is a constant
		// (e.g., from constant switch statement),
		// don't generate a query
		if (CE != NULL) {
			if (CE->isFalse()) result = false;
			else if (CE->isTrue()) result = true;
			else assert(false && "Invalid constant fork condition");
		} else {
			solver->setTimeout(fi.timeout);
			bool success;
			success = solver->mayBeTrue(
				current,
				fi.conditions[condIndex],
				result);
			solver->setTimeout(stpTimeout);
			if (!success) {
				terminateStateEarly(current, "query timed out");
				return false;
			}
		}

		fi.res[condIndex] = result;
		if (result)
			fi.validTargets++;
	}

	return true;
}


void Executor::makeForks(ExecutionState& current, struct ForkInfo& fi)
{
	ExecutionState	**curStateUsed = NULL;

	for(unsigned int condIndex = 0; condIndex < fi.N; condIndex++) {
		ExecutionState	*newState, *baseState;

		// Process each valid target and generate a state
		if (!fi.res[condIndex]) continue;

		if (!curStateUsed) {
			/* reuse current state, when possible */
			fi.resStates[condIndex] = &current;
			curStateUsed = &fi.resStates[condIndex];
			continue;
		}

		assert (!fi.forkCompact || ReplayInhibitedForks);

		// Update stats
		TimerStatIncrementer timer(stats::forkTime);
		++stats::forks;

		// Do actual state forking
		baseState = &current;
		newState = fi.forkCompact
			? current.branchForReplay()
			: current.branch();

		stateManager->queueAdd(newState);
		fi.resStates[condIndex] = newState;

		// Split pathWriter stream
		if (!fi.isInternal) {
			if (symPathWriter && newState != baseState)
				newState->symPathOS = symPathWriter->open(
					current.symPathOS);
		}

		// Randomize path tree layout
		if (RandomizeFork && theRNG.getBool()) {
			std::swap(baseState, newState);
			// Randomize which actual state gets the T/F branch;
			// Affects which state BatchinSearcher retains.
			std::swap(*curStateUsed, fi.resStates[condIndex]);
		}

		// Update path tree with new states
		current.ptreeNode->data = 0;
		pathTree->splitStates(current.ptreeNode, baseState, newState);
	}
}

void Executor::constrainFork(
	ExecutionState& current,
	struct ForkInfo& fi,
	unsigned int condIndex)
{
	ExecutionState* curState;

	if (fi.res[condIndex] == false)
		return;

	curState = fi.resStates[condIndex];
	assert(curState);

	// Add path constraint
	if (!curState->isCompact() && fi.feasibleTargets > 1) {
		bool	constraint_added;

		constraint_added = addConstraint(
			*curState, fi.conditions[condIndex]);
		if (constraint_added == false) {
			terminateStateEarly(
				*curState, "contradiction on branch");
			fi.resStates[condIndex] = NULL;
			fi.res[condIndex] = false;
			return;
		}
	}

	// XXX - even if the constraint is provable one way or the other we
	// can probably benefit by adding this constraint and allowing it to
	// reduce the other constraints. For example, if we do a binary
	// search on a particular value, and then see a comparison against
	// the value it has been fixed at, we should take this as a nice
	// hint to just use the single constraint instead of all the binary
	// search ones. If that makes sense.

	// Kinda gross, do we even really still want this option?
	if (MaxDepth && MaxDepth <= curState->depth) {
		terminateStateEarly(*curState, "max-depth exceeded");
		fi.resStates[condIndex] = NULL;
		return;
	}

	// Auxiliary bookkeeping
	if (!fi.isInternal) {
		if (symPathWriter && fi.validTargets > 1) {
			std::stringstream ssPath;
			ssPath << condIndex << "\n";
			curState->symPathOS << ssPath.str();
		}

		// only track NON-internal branches
		if (!fi.wasReplayed)
			curState->trackBranch(
				condIndex,
				current.prevPC->getInfo()->assemblyLine);
	}

	if (fi.isSeeding) {
		seedMap[curState].insert(
			seedMap[curState].end(),
			fi.resSeeds[condIndex].begin(),
			fi.resSeeds[condIndex].end());
	}
}

void Executor::constrainForks(ExecutionState& current, struct ForkInfo& fi)
{
	// Loop for bookkeeping
	// (loops must be separate since states are forked from each other)
	for (unsigned int condIndex = 0; condIndex < fi.N; condIndex++) {
		constrainFork(current, fi, condIndex);
	}
}

// Check to see if this constraint violates seeds.
// N.B. There's a problem here-- it won't catch what happens if the seed
// is wrong but we screwed up the constraints so that it's permitted.
//
// Offline solution-- save states, run through them, check for subsumption
//
void Executor::checkAddConstraintSeeds(ExecutionState& state, ref<Expr>& cond)
{
	std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it;
	bool	warn = false;

	it = seedMap.find(&state);
	if (it == seedMap.end())
		return;

	foreach (siit, it->second.begin(), it->second.end()) {
		bool	mustBeFalse, ok;

		ok = solver->mustBeFalse(
			state,
			siit->assignment.evaluate(cond),
			mustBeFalse);
		assert(ok && "FIXME: Unhandled solver failure");
		if (mustBeFalse) {
			siit->patchSeed(state, cond, solver);
			warn = true;
		}
	}

	if (warn)
		klee_warning("seeds patched for violating constraint");
}

bool Executor::addConstraint(ExecutionState &state, ref<Expr> condition)
{
	if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
		assert(CE->isTrue() && "attempt to add invalid constraint");
		return true;
	}

	checkAddConstraintSeeds(state, condition);

	if (ChkConstraints) {
		bool	mayBeTrue, ok;

		ok = solver->mayBeTrue(state, condition, mayBeTrue);
		assert (ok);

		if (!mayBeTrue) {
			std::cerr
				<< "[CHKCON] WHOOPS: "
				<< condition << " is never true!\n";
		}
		assert (mayBeTrue);
	}

	if (!state.addConstraint(condition)) {
		std::cerr << "[CHKCON] Failed to add constraint\n";
		return false;
	}

	if (ivcEnabled) {
		doImpliedValueConcretization(
			state,
			condition,
			ConstantExpr::alloc(1, Expr::Bool));
	}

	if (ChkConstraints) {
		bool	mustBeTrue, ok;

		ok = solver->mustBeTrue(state, condition, mustBeTrue);
		assert (ok);

		if (!mustBeTrue) {
			std::cerr
				<< "[CHKCON] WHOOPS2: "
				<< condition << " is never true!\n";
		}
		assert (mustBeTrue);
	}

	return true;
}

ref<klee::ConstantExpr> Executor::evalConstant(
	const KModule* km, const Globals* gm, Constant *c)
{
	if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c))
		return evalConstantExpr(km, gm, ce);

	if (const ConstantInt *ci = dyn_cast<ConstantInt>(c))
		return ConstantExpr::alloc(ci->getValue());

	if (const ConstantFP *cf = dyn_cast<ConstantFP>(c))
		return ConstantExpr::alloc(cf->getValueAPF().bitcastToAPInt());

	if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
		assert (gm != NULL);
		ref<klee::ConstantExpr>	ce(gm->findAddress(gv));
		assert (!ce.isNull() && "No global address!");
		return ce;
	}

	if (isa<ConstantPointerNull>(c))
		return Expr::createPointer(0);

	if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
		return ConstantExpr::create(
			0, km->getWidthForLLVMType(c->getType()));
	}

	if (isa<ConstantVector>(c))
		return ConstantExpr::createVector(cast<ConstantVector>(c));

	// Constant{AggregateZero,Array,Struct,Vector}
	c->dump();
	assert(0 && "invalid argument to evalConstant()");
}

/* Concretize the given expression, and return a possible constant value.
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr>
Executor::toConstant(
	ExecutionState &state, ref<Expr> e, const char *reason,
	bool showLineInfo)
{
	ref<ConstantExpr>	value;
	bool			success;

	e = state.constraints.simplifyExpr(e);
	if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
		return CE;

	success = solver->getValue(state, e, value);
	assert(success && "FIXME: Unhandled solver failure");

	std::ostringstream	os;
	os	<< "silently concretizing (reason: " << reason
		<< ") expression " << e
		<< " to value " << value;

	if (showLineInfo) {
		os	<< " (" << (*(state.pc)).getInfo()->file << ":"
			<< (*(state.pc)).getInfo()->line << ")";
	}

	os << '\n';

	if (AllExternalWarnings)
		klee_warning(reason, os.str().c_str());
	else
		klee_warning_once(reason, "%s", os.str().c_str());

	addConstraint(state, EqExpr::create(e, value));

	return value;
}

bool Executor::getSeedInfoIterRange(
	ExecutionState* s, SeedInfoIterator &b, SeedInfoIterator& e)
{
	std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it;
	it = seedMap.find(s);
	if (it == seedMap.end()) return false;
	b = it->second.begin();
	e = it->second.end();
	return false;
}

void Executor::executeGetValue(
	ExecutionState &state,
	ref<Expr> e,
	KInstruction *target)
{
	bool              isSeeding;
	SeedInfoIterator  si_begin, si_end;

	e = state.constraints.simplifyExpr(e);
	isSeeding = getSeedInfoIterRange(&state, si_begin, si_end);

	if (!isSeeding || isa<ConstantExpr>(e)) {
		ref<ConstantExpr> value;
		bool ok = solver->getValue(state, e, value);
		assert(ok && "FIXME: Unhandled solver failure");
		if (target) state.bindLocal(target, value);
		return;
	}

	std::set< ref<Expr> > values;
	foreach (siit, si_begin, si_end) {
		ref<ConstantExpr> value;
		bool		ok;

		ok = solver->getValue(state, siit->assignment.evaluate(e), value);
		assert(ok && "FIXME: Unhandled solver failure");

		values.insert(value);
	}

	std::vector< ref<Expr> > conditions;
	foreach (vit, values.begin(), values.end())
		conditions.push_back(EqExpr::create(e, *vit));

	StateVector branches;
	branches = fork(state, conditions.size(), conditions.data(), true);
	if (!target) return;

	StateVector::iterator bit = branches.begin();

	foreach (vit, values.begin(), values.end()) {
		ExecutionState *es = *bit;
		if (es) es->bindLocal(target, *vit);
		++bit;
	}
}

void Executor::stepInstruction(ExecutionState &state)
{
	assert (state.checkCanary() && "Not a valid state");

	if (DebugPrintInstructions) {
		printFileLine(state, state.pc);
		std::cerr
			<< std::setw(10) << stats::instructions << " "
			<< (state.pc->getInst()) << "\n";
	}

	if (statsTracker) statsTracker->stepInstruction(state);

	++stats::instructions;
	state.prevPC = state.pc;
	++state.pc;
	if (stats::instructions==StopAfterNInstructions)
		haltExecution = true;
}

void Executor::executeCallNonDecl(
	ExecutionState &state,
	KInstruction *ki,
	Function *f,
	std::vector< ref<Expr> > &arguments)
{
	// FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
	// guess. This just done to avoid having to pass KInstIterator everywhere
	// instead of the actual instruction, since we can't make a KInstIterator
	// from just an instruction (unlike LLVM).
	KFunction	*kf;
	unsigned	callingArgs, funcArgs, numFormals;

	assert (!f->isDeclaration() && "Expects a non-declaration function!");
	kf = kmodule->getKFunction(f);
	assert (kf != NULL && "Executing non-shadowed function");

	state.pushFrame(state.prevPC, kf);
	state.pc = kf->instructions;

	if (statsTracker)
		statsTracker->framePushed(
			state,
			&state.stack[state.stack.size()-2]);

	// TODO: support "byval" parameter attribute
	// TODO: support zeroext, signext, sret attributes
	//
	//

	callingArgs = arguments.size();
	funcArgs = f->arg_size();
	if (callingArgs < funcArgs) {
		terminateStateOnError(
			state,
			"calling function with too few arguments",
			"user.err");
		return;
	}

	if (!f->isVarArg()) {
		if (callingArgs > funcArgs) {
			klee_warning_once(f, "calling %s with extra arguments.",
			f->getName().data());
		}
	} else {
		if (!state.setupCallVarArgs(funcArgs, arguments)) {
			terminateStateOnExecError(state, "out of memory (varargs)");
			return;
		}
	}

	numFormals = f->arg_size();
	for (unsigned i=0; i<numFormals; ++i)
		state.bindArgument(kf, i, arguments[i]);
}


void Executor::executeCall(
	ExecutionState &state,
	KInstruction *ki,
	Function *f,
	std::vector< ref<Expr> > &arguments)
{
  assert (f);

  if (WriteTraces)
    state.exeTraceMgr.addEvent(
      new FunctionCallTraceEvent(state, ki, f->getName()));

  Instruction *i = ki->getInst();
  Function* f2 = NULL;
  if (!f->isDeclaration() || (f2 = kmodule->module->getFunction(f->getNameStr())))
  {
    /* this is so that vexllvm linked modules work */
    if (f2 == NULL) f2 = f;
    if (!f2->isDeclaration()) {
      executeCallNonDecl(state, ki, f2, arguments);
      return;
    }
  }

  switch(f->getIntrinsicID()) {
	case Intrinsic::not_intrinsic:
	// state may be destroyed by this call, cannot touch
	callExternalFunction(state, ki, f, arguments);
	break;

	// va_arg is handled by caller and intrinsic lowering, see comment for
	// ExecutionState::varargs
	case Intrinsic::vastart:  {
	StackFrame &sf = state.stack.back();
	assert(sf.varargs &&
	   "vastart called in function with no vararg object");
     // FIXME: This is really specific to the architecture, not the pointer
    // size. This happens to work fir x86-32 and x86-64, however.
#define MMU_WORD_OP(x,y)       mmu->exeMemOp(  \
                               state,  \
                               MMU::MemOp(true,        \
                                       AddExpr::create(        \
                                               arguments[0],   \
                                               ConstantExpr::create(x, 64)),\
                                       y, NULL))

	Expr::Width WordSize = Context::get().getPointerWidth();
	if (WordSize == Expr::Int32) {
		MMU_WORD_OP(0, sf.varargs->getBaseExpr());
		break;
	}

	assert(WordSize == Expr::Int64 && "Unknown word size!");
	// X86-64 has quite complicated calling convention. However,
	// instead of implementing it, we can do a simple hack: just
	// make a function believe that all varargs are on stack.

	// gp offset
	MMU_WORD_OP(0, ConstantExpr::create(48,32));
	// fp_offset
	MMU_WORD_OP(4, ConstantExpr::create(304,32));
	// overflow_arg_area
	MMU_WORD_OP(8, sf.varargs->getBaseExpr());
	// reg_save_area
	MMU_WORD_OP(16, ConstantExpr::create(0, 64));
#undef MMU_WORD_OP
    break;
  }
  case Intrinsic::vaend:
    // va_end is a noop for the interpreter.
    //
    // FIXME: We should validate that the target didn't do something bad
    // with vaeend, however (like call it twice).
    break;

  case Intrinsic::vacopy:
    // va_copy should have been lowered.
    //
    // FIXME: It would be nice to check for errors in the usage of this as
    // well.
  default:
    klee_error("unknown intrinsic: %s", f->getName().data());
  }

  if (InvokeInst *ii = dyn_cast<InvokeInst>(i)) {
    state.transferToBasicBlock(ii->getNormalDest(), i->getParent());
  }
}

void Executor::printFileLine(ExecutionState &state, KInstruction *ki) {
  const InstructionInfo &ii = *ki->getInfo();
  if (ii.file != "")
    std::cerr << "     " << ii.file << ":" << ii.line << ":";
  else
    std::cerr << "     [no debug info]:";
}

bool Executor::isDebugIntrinsic(const Function *f)
{ return f->getIntrinsicID() == Intrinsic::dbg_declare; }

static inline const llvm::fltSemantics * fpWidthToSemantics(unsigned width)
{
	switch(width) {
	case Expr::Int32:	return &llvm::APFloat::IEEEsingle;
	case Expr::Int64:	return &llvm::APFloat::IEEEdouble;
	case Expr::Fl80:	return &llvm::APFloat::x87DoubleExtended;
	default:		return 0;
	}
}

void Executor::retFromNested(ExecutionState &state, KInstruction *ki)
{
	ReturnInst	*ri;
	KInstIterator	kcaller;
	Instruction	*caller;
	bool		isVoidReturn;
	ref<Expr>	result;

	assert (isa<ReturnInst>(ki->getInst()) && "Expected ReturnInst");

	ri = cast<ReturnInst>(ki->getInst());
	kcaller = state.getCaller();
	caller = kcaller ? kcaller->getInst() : 0;
	isVoidReturn = (ri->getNumOperands() == 0);

	assert (state.stack.size() > 1);

	if (!isVoidReturn) {
		result = eval(ki, 0, state).value;
	}

	state.popFrame();

	if (statsTracker) statsTracker->framePopped(state);

	if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
		state.transferToBasicBlock(ii->getNormalDest(), caller->getParent());
	} else {
		state.pc = kcaller;
		++state.pc;
	}

	if (isVoidReturn) {
		// We check that the return value has no users instead of
		// checking the type, since C defaults to returning int for
		// undeclared functions.
		if (!caller->use_empty()) {
			terminateStateOnExecError(
				state,
				"return void when caller expected a result");
		}
		return;
	}

	assert (!isVoidReturn);
	Type *t = caller->getType();
	if (t->isVoidTy())
		return;

	// may need to do coercion due to bitcasts
	assert (result.isNull() == false);
	Expr::Width from = result->getWidth();
	Expr::Width to = kmodule->getWidthForLLVMType(t);

	if (from != to) {
		CallSite cs = (isa<InvokeInst>(caller) ?
			CallSite(cast<InvokeInst>(caller)) :
			CallSite(cast<CallInst>(caller)));

		// XXX need to check other param attrs ?
		if (cs.paramHasAttr(0, llvm::Attribute::SExt)) {
			result = SExtExpr::create(result, to);
		} else {
			result = ZExtExpr::create(result, to);
		}
	}

	state.bindLocal(kcaller, result);
}

const Cell& Executor::eval(
	KInstruction *ki,
	unsigned index,
	ExecutionState &state) const
{
	int vnumber;

	assert(index < ki->getInst()->getNumOperands());

	vnumber = ki->getOperand(index);
	assert(	vnumber != -1 &&
		"Invalid operand to eval(), not a value or constant!");

	// Determine if this is a constant or not.
	if (vnumber < 0) return kmodule->constantTable[-vnumber - 2];

	return state.readLocalCell(state.stack.size() - 1, vnumber);
}

void Executor::instRet(ExecutionState &state, KInstruction *ki)
{
	if (state.stack.size() <= 1) {
		assert (!(state.getCaller()) &&
			"caller set on initial stack frame");
		terminateStateOnExit(state);
		return;
	}

	retFromNested(state, ki);
}

void Executor::instBranch(ExecutionState& state, KInstruction* ki)
{
	BranchInst *bi = cast<BranchInst>(ki->getInst());

	if (bi->isUnconditional()) {
		state.transferToBasicBlock(
			bi->getSuccessor(0), bi->getParent());
		return;
	}

	// FIXME: Find a way that we don't have this hidden dependency.
	assert (bi->getCondition() == bi->getOperand(0) &&
		"Wrong operand index!");

	const Cell &cond = eval(ki, 0, state);

	StatePair branches = fork(state, cond.value, false);

	if (WriteTraces) {
		bool isTwoWay = (branches.first && branches.second);

		if (branches.first) {
			branches.first->exeTraceMgr.addEvent(
				new BranchTraceEvent(
					state, ki, true, isTwoWay));
		}

		if (branches.second) {
			branches.second->exeTraceMgr.addEvent(
				new BranchTraceEvent(
					state, ki, false, isTwoWay));
		}
	}

	// NOTE: There is a hidden dependency here, markBranchVisited
	// requires that we still be in the context of the branch
	// instruction (it reuses its statistic id). Should be cleaned
	// up with convenient instruction specific data.
	if (statsTracker && state.getCurrentKFunc()->trackCoverage)
		statsTracker->markBranchVisited(
			branches.first, branches.second);

	finalizeBranch(branches.first, bi, 0 /* [0] successor => true/then */);
	finalizeBranch(branches.second, bi, 1 /* [1] successor => false/else */);
}

void Executor::finalizeBranch(
	ExecutionState* st,
	BranchInst*	bi,
	int branchIdx)
{
  	KFunction	*kf;

	if (st == NULL) return;

	kf = st->getCurrentKFunc();

	// reconstitute the state if it was forked into compact form but will
	// immediately cover a new instruction
	// !!! this can be done more efficiently by simply forking a regular
	// state inside fork() but that will change the fork() semantics
	if (	st->isCompact() &&
		kf->trackCoverage &&
		theStatisticManager->getIndexedValue(
			stats::uncoveredInstructions,
			kf->instructions[kf->getBasicBlockEntry(
				bi->getSuccessor(branchIdx))]->getInfo()->id))
	{
		ExecutionState *newState;
		newState = st->reconstitute(*initialStateCopy);
		replaceStateImmForked(st, newState);
		st = newState;
	}

	st->transferToBasicBlock(
		bi->getSuccessor(branchIdx),
		bi->getParent());
}

void Executor::instCall(ExecutionState& state, KInstruction *ki)
{
	CallSite cs(ki->getInst());
	unsigned numArgs = cs.arg_size();
	Function *f = cs.getCalledFunction();

	// Skip debug intrinsics, we can't evaluate their metadata arguments.
	if (f && isDebugIntrinsic(f)) return;

	// evaluate arguments
	std::vector< ref<Expr> > arguments;
	arguments.reserve(numArgs);

	for (unsigned j=0; j<numArgs; ++j)
		arguments.push_back(eval(ki, j+1, state).value);

	if (!f) {
		// special case the call with a bitcast case
		Value *fp = cs.getCalledValue();
		llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(fp);

		if (isa<InlineAsm>(fp)) {
			terminateStateOnExecError(
				state, "inline assembly is unsupported");
			return;
		}

		if (ce && ce->getOpcode()==Instruction::BitCast)
			f = executeBitCast(state, cs, ce, arguments);
	}

	if (f == NULL) {
		XferStateIter	iter;

		xferIterInit(iter, &state, ki);
		while (xferIterNext(iter))
			executeCall(*(iter.res.first), ki, iter.f, arguments);

		return;
	}

	executeCall(state, ki, f, arguments);
}

llvm::Function* Executor::executeBitCast(
	ExecutionState &state,
	CallSite&		cs,
	llvm::ConstantExpr*	ce,
	std::vector< ref<Expr> > &arguments)
{
	llvm::Function		*f;
	const FunctionType	*fType, *ceType;

	f = dyn_cast<Function>(ce->getOperand(0));
	if (f == NULL) {
		GlobalAlias*	ga;

		ga = dyn_cast<GlobalAlias>(ce->getOperand(0));
		assert (ga != NULL && "Not alias-- then what?");

		f = dyn_cast<Function>(ga->getAliasee());
		if (f == NULL) {
			llvm::ConstantExpr *new_ce;

			new_ce =  dyn_cast<llvm::ConstantExpr>(ga->getAliasee());
			if (new_ce && new_ce->getOpcode() == Instruction::BitCast)
			{
				return executeBitCast(
					state, cs, new_ce, arguments);
			}
		}
		assert (f != NULL && "Alias not function??");
	}
     	assert(f && "XXX unrecognized constant expression in call");

        fType = dyn_cast<FunctionType>(
		cast<PointerType>(f->getType())->getElementType());
	ceType = dyn_cast<FunctionType>(
		cast<PointerType>(ce->getType())->getElementType());

	assert(fType && ceType && "unable to get function type");

	// XXX check result coercion

	// XXX this really needs thought and validation
	unsigned i=0;
	for (	std::vector< ref<Expr> >::iterator
		ai = arguments.begin(), ie = arguments.end();
		ai != ie; ++ai, i++)
	{
		Expr::Width to, from;

		if (i >= fType->getNumParams()) continue;

		from = (*ai)->getWidth();
		to = kmodule->getWidthForLLVMType(fType->getParamType(i));
		if (from == to) continue;

		// XXX need to check other param attrs ?
		if (cs.paramHasAttr(i+1, llvm::Attribute::SExt)) {
			arguments[i] = SExtExpr::create(arguments[i], to);
		} else {
			arguments[i] = ZExtExpr::create(arguments[i], to);
		}
	}

	return f;
}

void Executor::instCmp(ExecutionState& state, KInstruction *ki)
{
	CmpInst*		ci = cast<CmpInst>(ki->getInst());
	Type*			op_type = ci->getOperand(1)->getType();
	ICmpInst*		ii = cast<ICmpInst>(ci);
	ICmpInst::Predicate	pred;
	VectorType*		vt;

	ref<Expr> left = eval(ki, 0, state).value;
	ref<Expr> right = eval(ki, 1, state).value;
	ref<Expr> result;

	pred = ii->getPredicate();
	if ((vt = dyn_cast<VectorType>(op_type))) {
		bool ok;
		result = cmpVector(state, pred, vt, left, right, ok);
		if (!ok) return;
	} else {
		bool ok;
		result = cmpScalar(state, pred, left, right, ok);
		if (!ok) return;
	}

	state.bindLocal(ki, result);
}

#define SETUP_VOP(x)					\
	ref<Expr>	result;				\
	unsigned int	v_elem_c;			\
	unsigned int	v_elem_w;			\
	v_elem_c = (x)->getNumElements();		\
	v_elem_w = (x)->getBitWidth() / v_elem_c;

/* FIXME: cheaper way to do this (e.g. left == right => spit out constant expr?) */
#define V_OP_APPEND(y)		V_OP(y, ConcatExpr::create(result, op_i))
#define V_OP_PREPEND(y)		V_OP(y, ConcatExpr::create(op_i, result))
#define V_OP(y,z)						\
	for (unsigned int i = 0; i < v_elem_c; i++) {		\
		ref<Expr>	left_i, right_i;		\
		ref<Expr>	op_i;				\
		left_i = ExtractExpr::create(			\
			left, i*v_elem_w, v_elem_w);		\
		right_i = ExtractExpr::create(			\
			right, i*v_elem_w, v_elem_w);		\
		op_i = y##Expr::create(left_i, right_i);	\
		if (i == 0) result = op_i;			\
		else result = z;				\
	}

#define SETUP_VOP_CAST(x,y)					\
	ref<Expr>	result;					\
	unsigned int	v_elem_c;				\
	unsigned int	v_elem_w_src, v_elem_w_dst;		\
	v_elem_c = (x)->getNumElements();			\
	assert (v_elem_c == (y)->getNumElements());		\
	v_elem_w_src = (x)->getBitWidth() / v_elem_c;		\
	v_elem_w_dst = (y)->getBitWidth() / v_elem_c;		\


ref<Expr> Executor::cmpVector(
	ExecutionState& state,
	int pred,
	llvm::VectorType* vt,
	ref<Expr> left, ref<Expr> right,
	bool& ok)
{
	SETUP_VOP(vt)

	ok = false;
	assert (left->getWidth() > 0);
	assert (right->getWidth() > 0);

	switch(pred) {
#define VCMP_OP(x, y) \
	case ICmpInst::x: V_OP_APPEND(y); break;

	VCMP_OP(ICMP_EQ, Eq)
	VCMP_OP(ICMP_NE, Ne)
	VCMP_OP(ICMP_UGT, Ugt)
	VCMP_OP(ICMP_UGE, Uge)
	VCMP_OP(ICMP_ULT, Ult)
	VCMP_OP(ICMP_ULE, Ule)
	VCMP_OP(ICMP_SGT, Sgt)
	VCMP_OP(ICMP_SGE, Sge)
	VCMP_OP(ICMP_SLT, Slt)
	VCMP_OP(ICMP_SLE, Sle)
	default:
	terminateStateOnExecError(state, "invalid vector ICmp predicate");
	return result;
	}
	ok = true;
	return result;
}

ref<Expr> Executor::sextVector(
	ExecutionState& state,
	ref<Expr> v,
	VectorType* srcTy,
	VectorType* dstTy)
{
	SETUP_VOP_CAST(srcTy, dstTy);
	for (unsigned int i = 0; i < v_elem_c; i++) {
		ref<Expr>	cur_elem;
		cur_elem = ExtractExpr::create(
			v, i*v_elem_w_src, v_elem_w_src);
		cur_elem = SExtExpr::create(cur_elem, v_elem_w_dst);
		if (i == 0)
			result = cur_elem;
		else
			result = ConcatExpr::create(result, cur_elem);
	}

	return result;
}

ref<Expr> Executor::cmpScalar(
	ExecutionState& state,
	int pred, ref<Expr> left, ref<Expr> right, bool& ok)
{
  ref<Expr> result;
  ok = false;
  switch(pred) {
  case ICmpInst::ICMP_EQ: result = EqExpr::create(left, right); break;
  case ICmpInst::ICMP_NE: result = NeExpr::create(left, right); break;
  case ICmpInst::ICMP_UGT: result = UgtExpr::create(left, right); break;
  case ICmpInst::ICMP_UGE: result = UgeExpr::create(left, right); break;
  case ICmpInst::ICMP_ULT: result = UltExpr::create(left, right); break;
  case ICmpInst::ICMP_ULE: result = UleExpr::create(left, right); break;
  case ICmpInst::ICMP_SGT: result = SgtExpr::create(left, right); break;
  case ICmpInst::ICMP_SGE: result = SgeExpr::create(left, right); break;
  case ICmpInst::ICMP_SLT: result = SltExpr::create(left, right); break;
  case ICmpInst::ICMP_SLE: result = SleExpr::create(left, right); break;
  default:
    terminateStateOnExecError(state, "invalid scalar ICmp predicate");
    return result;
  }
  ok = true;
  return result;
}

void Executor::forkSwitch(
	ExecutionState& state,
	BasicBlock*	parent_bb,
	const TargetTy& defaultTarget,
	const TargetsTy& targets)
{
	StateVector			resultStates;
	std::vector<ref<Expr> >		caseConds(targets.size()+1);
	std::vector<BasicBlock*>	caseDests(targets.size()+1);
	unsigned			index;
	bool				found;

	// prepare vectors for fork call
	caseDests[0] = defaultTarget.first;
	caseConds[0] = defaultTarget.second;
	index = 1;
	foreach (mit, targets.begin(), targets.end()) {
		caseDests[index] = (*mit).second.first;
		caseConds[index] = (*mit).second.second;
		index++;
	}

	resultStates = fork(state, caseConds.size(), caseConds.data(), false);
	assert(resultStates.size() == caseConds.size());

	found = false;
	for(index = 0; index < resultStates.size(); index++) {
		ExecutionState	*es;
		BasicBlock	*destBlock;
		KFunction	*kf;
		unsigned	entry;

		es = resultStates[index];
		if (!es) continue;

		found = true;
		destBlock = caseDests[index];
		kf = state.getCurrentKFunc();

		entry = kf->getBasicBlockEntry(destBlock);
		if (	es->isCompact() &&
			kf->trackCoverage &&
			/* XXX I don't understand this condition */
			theStatisticManager->getIndexedValue(
				stats::uncoveredInstructions,
				kf->instructions[entry]->getInfo()->id))
		{
			ExecutionState *newState;
			newState = es->reconstitute(*initialStateCopy);
			replaceStateImmForked(es, newState);
			es = newState;
		}

		if (!es->isCompact())
			es->transferToBasicBlock(destBlock, parent_bb);

		// Update coverage stats
		if (	kf->trackCoverage &&
			theStatisticManager->getIndexedValue(
				stats::uncoveredInstructions,
				kf->instructions[entry]->getInfo()->id))
		{
			es->coveredNew = true;
			es->instsSinceCovNew = 1;
		}
	}

	if (!found)
		terminateState(state);
}

ref<Expr> Executor::toUnique(const ExecutionState &state, ref<Expr> &e)
{ return solver->toUnique(state, e); }

void Executor::instSwitch(ExecutionState& state, KInstruction *ki)
{
	SwitchInst *si = cast<SwitchInst>(ki->getInst());
	ref<Expr> cond = eval(ki, 0, state).value;

	cond = toUnique(state, cond);

	/* deterministically order basic blocks by lowest value */
	std::vector<Val2TargetTy >	cases(si->getNumCases());
	TargetsTy			targets;
	TargetTy			defaultTarget;
	TargetValsTy			minTargetValues; // lowest val -> BB

	assert (cases.size () >= 1);

	/* initialize minTargetValues and cases */
	cases[0] = Val2TargetTy(ref<ConstantExpr>(), si->getDefaultDest());
	for (unsigned i = 1; i < cases.size(); i++) {
		ref<ConstantExpr>	value;
		BasicBlock		*target;
		TargetValsTy::iterator	it;

		value  = evalConstant(si->getCaseValue(i));
		target = si->getSuccessor(i);
		cases[i] = Val2TargetTy(value, target);

		it = minTargetValues.find(target);
		if (it == minTargetValues.end())
			minTargetValues[target] = value;
		else if (value < it->second)
			it->second = value;
	}

	if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
		defaultTarget = getConstCondSwitchTargets(
			ki,
			CE,
			cases,
			minTargetValues,
			targets);
	} else {
		defaultTarget = getExprCondSwitchTargets(
			cond, cases, minTargetValues, targets);
	}

	// may not have any targets to jump to!
	if (targets.empty()) {
		terminateState(state);
		return;
	}

	forkSwitch(state, si->getParent(), defaultTarget, targets);
}

Executor::TargetTy Executor::getExprCondSwitchTargets(
	ref<Expr> cond,
	const std::vector<Val2TargetTy >& cases,
	const TargetValsTy& minTargetValues,
	TargetsTy& targets)
{
	std::map<BasicBlock*, std::set<uint64_t> > caseMap;
	ref<Expr>	defaultCase;

	defaultCase = ConstantExpr::alloc(1, Expr::Bool);
	// build map from target BasicBlock to value(s) that lead to that block
	for (unsigned i = 1; i < cases.size(); ++i)
		caseMap[cases[i].second].insert(cases[i].first->getZExtValue());

	// generate conditions for each block
	foreach (cit, caseMap.begin(), caseMap.end()) {
		BasicBlock *target = cit->first;
		std::set<uint64_t> &values = cit->second;
		ref<Expr> match = ConstantExpr::create(0, Expr::Bool);

		foreach (vit, values.begin(), values.end()) {
			// try run-length encoding long sequences of consecutive
			// switch values that map to the same BasicBlock
			std::set<uint64_t>::iterator vit2 = vit;
			uint64_t runLen = 1;

			for (++vit2; vit2 != values.end(); ++vit2) {
				if (*vit2 == *vit + runLen)
					runLen++;
				else
					break;
			}

			if (runLen < EXE_SWITCH_RLE_LIMIT) {
				match = OrExpr::create(
					match,
					EqExpr::create(
						cond,
						ConstantExpr::alloc(*vit,
						cond->getWidth())));
				continue;
			}

			// use run-length encoding
			ref<Expr>	rle_bounds;
			rle_bounds = AndExpr::create(
				UgeExpr::create(
					cond,
					ConstantExpr::alloc(
						*vit, cond->getWidth())),
				UltExpr::create(
					cond,
					ConstantExpr::alloc(
						*vit + runLen,
						cond->getWidth())));

			match = OrExpr::create(match, rle_bounds);

			vit = vit2;
			--vit;
		}

		targets.insert(std::make_pair(
			(minTargetValues.find(target))->second,
			std::make_pair(target, match)));

		// default case is the AND of all the complements
		defaultCase = AndExpr::create(
			defaultCase, Expr::createIsZero(match));
	}

	// include default case
	return std::make_pair(cases[0].second, defaultCase);
}

// Somewhat gross to create these all the time, but fine till we
// switch to an internal rep.
Executor::TargetTy Executor::getConstCondSwitchTargets(
	KInstruction	*ki,
	ConstantExpr	*CE,
	const std::vector<Val2TargetTy >& cases,
	const TargetValsTy	&minTargetValues,
	TargetsTy		&targets)
{
	SwitchInst		*si;
	llvm::IntegerType	*Ty;
	ConstantInt		*ci;
	unsigned		index;
	TargetTy		defaultTarget;

	targets.clear();

	si = cast<SwitchInst>(ki->getInst());
	Ty = cast<IntegerType>(si->getCondition()->getType());
	ci = ConstantInt::get(Ty, CE->getZExtValue());
	index = si->findCaseValue(ci);

	// We need to have the same set of targets to pass to fork() in case
	// toUnique fails/times out on replay (it's happened before...)
	defaultTarget = TargetTy(
		cases[0].second,
		ConstantExpr::alloc(0, Expr::Bool));
	if (index == 0)
		defaultTarget.second = ConstantExpr::alloc(1, Expr::Bool);

	for (unsigned i = 1; i < cases.size(); ++i) {
		// default to infeasible target
		TargetsTy::iterator	it;
		TargetTy		cur_target;

		cur_target = TargetTy(
			cases[i].second,
			ConstantExpr::alloc(0, Expr::Bool));
		it = targets.insert(
			std::make_pair(
				minTargetValues.find(cases[i].second)->second,
				cur_target)).first;

		// set unique target as feasible
		if (i == index) {
			it->second.second = ConstantExpr::alloc(1, Expr::Bool);
		}
	}

	return defaultTarget;
}


void Executor::instInsertElement(ExecutionState& state, KInstruction* ki)
{
	/* insert element has two parametres:
	 * 1. source vector (v)
	 * 2. element to insert
	 * 3. insertion index
	 * returns v[idx]
	 */
	ref<Expr> in_v = eval(ki, 0, state).value;
	ref<Expr> in_newelem = eval(ki, 1, state).value;
	ref<Expr> in_idx = eval(ki, 2, state).value;

	ConstantExpr* in_idx_ce = dynamic_cast<ConstantExpr*>(in_idx.get());
	assert (in_idx_ce && "NON-CONSTANT INSERT ELEMENT IDX. PUKE");
	uint64_t idx = in_idx_ce->getZExtValue();

	/* instruction has types of vectors embedded in its operands */
	InsertElementInst*	iei = cast<InsertElementInst>(ki->getInst());
	assert (iei != NULL);

	VectorType*	vt;
	vt = dyn_cast<VectorType>(iei->getOperand(0)->getType());
	unsigned int		v_elem_c = vt->getNumElements();
	unsigned int		v_elem_sz = vt->getBitWidth() / v_elem_c;

	assert (idx < v_elem_c && "InsertElement idx overflow");

	ref<Expr>	out_val;
	if (idx == (v_elem_c - 1)) {
		/* replace head */
		out_val = ExtractExpr::create(
			in_v,
			0,
			v_elem_sz*(v_elem_c - 1));

		out_val = ConcatExpr::create(
			in_newelem /* tail */,
			out_val /* head */);
	} else if (idx == 0) {
		/* replace tail */
		out_val = ExtractExpr::create(
			in_v,
			v_elem_sz,
			v_elem_sz*(v_elem_c - 1));
		out_val = ConcatExpr::create(
			out_val /* head */,
			in_newelem /* tail */);
	} else {
		/* replace mid */
		/* (v, off, width) */
		out_val = ExtractExpr::create(
			in_v, 0, v_elem_sz*(idx - 1));
		out_val = ConcatExpr::create(
			out_val /* head */,
			in_newelem /* mid */ );
		out_val = ConcatExpr::create(
			out_val,
			ExtractExpr::create(
				in_v,
				(idx+1)*v_elem_sz,
				(v_elem_c-(idx+1))*v_elem_sz) /* tail */);
	}

	state.bindLocal(ki, out_val);
}

void Executor::instExtractElement(ExecutionState& state, KInstruction* ki)
{
	/* extract element has two parametres:
	 * 1. source vector (v)
	 * 2. extraction index (idx)
	 * returns v[idx]
	 */
	ref<Expr> in_v = eval(ki, 0, state).value;
	ref<Expr> in_idx = eval(ki, 1, state).value;
	ConstantExpr* in_idx_ce = dynamic_cast<ConstantExpr*>(in_idx.get());
	assert (in_idx_ce && "NON-CONSTANT EXTRACT ELEMENT IDX. PUKE");
	uint64_t idx = in_idx_ce->getZExtValue();

	/* instruction has types of vectors embedded in its operands */
	ExtractElementInst*	eei = cast<ExtractElementInst>(ki->getInst());
	assert (eei != NULL);

	VectorType*	vt;
	vt = dyn_cast<VectorType>(eei->getOperand(0)->getType());
	unsigned int		v_elem_c = vt->getNumElements();
	unsigned int		v_elem_sz = vt->getBitWidth() / v_elem_c;

	assert (idx < v_elem_c && "ExtrctElement idx overflow");
	ref<Expr>		out_val;
	out_val = ExtractExpr::create(
		in_v,
		idx * v_elem_sz,
		v_elem_sz);
	state.bindLocal(ki, out_val);
}

void Executor::instShuffleVector(ExecutionState& state, KInstruction* ki)
{
	/* shuffle vector has three parameters:
	 * 1. < in_vector |
	 * 2.             | in_vector >
	 * 3. < perm vect >
	 * 	Permutation vector
	 */
	ref<Expr> in_v_lo = eval(ki, 0, state).value;
	ref<Expr> in_v_hi = eval(ki, 1, state).value;
	ref<Expr> in_v_perm = eval(ki, 2, state).value;
	ConstantExpr* in_v_perm_ce = dynamic_cast<ConstantExpr*>(in_v_perm.get());
	assert (in_v_perm_ce != NULL && "WE HAVE NON-CONST SHUFFLES?? UGH.");

	/* instruction has types of vectors embedded in its operands */
	ShuffleVectorInst*	si = cast<ShuffleVectorInst>(ki->getInst());
	assert (si != NULL);
	VectorType*	vt = si->getType();
	unsigned int		v_elem_c = vt->getNumElements();
	unsigned int		v_elem_sz = vt->getBitWidth() / v_elem_c;
	unsigned int		perm_sz = in_v_perm_ce->getWidth() / v_elem_c;
	ref<Expr>		out_val;

	for (unsigned int i = 0; i < v_elem_c; i++) {
		ref<ConstantExpr>	v_idx;
		ref<Expr>		ext;
		unsigned int		idx;

		v_idx = in_v_perm_ce->Extract(i*perm_sz, perm_sz);
		idx = v_idx->getZExtValue();
		assert (idx < 2*v_elem_c && "Shuffle permutation out of range");
		if (idx < v_elem_c) {
			ext = ExtractExpr::create(
				in_v_lo, v_elem_sz*idx, v_elem_sz);
		} else {
			idx -= v_elem_c;
			ext = ExtractExpr::create(
				in_v_hi, v_elem_sz*idx, v_elem_sz);
		}

		if (i == 0) out_val = ext;
		else out_val = ConcatExpr::create(out_val, ext);
	}

	state.bindLocal(ki, out_val);
}

void Executor::instUnwind(ExecutionState& state)
{
  while (1) {
    KInstruction *kcaller = state.getCaller();
    state.popFrame();

    if (statsTracker) statsTracker->framePopped(state);

    if (state.stack.empty()) {
      terminateStateOnExecError(state, "unwind from initial stack frame");
      return;
    }

    Instruction *caller = kcaller->getInst();
    if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
      state.transferToBasicBlock(ii->getUnwindDest(), caller->getParent());
      return;
    }
  }
}

bool Executor::isFPPredicateMatched(
  APFloat::cmpResult CmpRes, CmpInst::Predicate pred)
{
  switch(pred) {
  // Predicates which only care about whether or not the operands are NaNs.
  case FCmpInst::FCMP_ORD: return CmpRes != APFloat::cmpUnordered;
  case FCmpInst::FCMP_UNO: return CmpRes == APFloat::cmpUnordered;
  // Ordered comparisons return false if either operand is NaN.  Unordered
  // comparisons return true if either operand is NaN.
  case FCmpInst::FCMP_UEQ: return CmpRes == APFloat::cmpUnordered;
  case FCmpInst::FCMP_OEQ: return CmpRes == APFloat::cmpEqual;
  case FCmpInst::FCMP_UGT: return CmpRes == APFloat::cmpUnordered;
  case FCmpInst::FCMP_OGT: return CmpRes == APFloat::cmpGreaterThan;
  case FCmpInst::FCMP_UGE: return CmpRes == APFloat::cmpUnordered;
  case FCmpInst::FCMP_OGE:
    return CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual;
  case FCmpInst::FCMP_ULT: return CmpRes == APFloat::cmpUnordered;
  case FCmpInst::FCMP_OLT: return CmpRes == APFloat::cmpLessThan;
  case FCmpInst::FCMP_ULE: return CmpRes == APFloat::cmpUnordered;
  case FCmpInst::FCMP_OLE:
    return CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual;
  case FCmpInst::FCMP_UNE:
    return CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual;
  case FCmpInst::FCMP_ONE:
    return CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual;
  default: assert(0 && "Invalid FCMP predicate!");
  case FCmpInst::FCMP_FALSE: return false;
  case FCmpInst::FCMP_TRUE: return true;
  }
  return false;
}

void Executor::instAlloc(ExecutionState& state, KInstruction* ki)
{
	AllocaInst	*ai;
	Instruction	*i = ki->getInst();
	unsigned	elementSize;
	bool		isLocal;
	ref<Expr>	size;

	assert (!isMalloc(ki->getInst()) && "ANTHONY! FIX THIS");

	ai = cast<AllocaInst>(i);
	elementSize = target_data->getTypeStoreSize(ai->getAllocatedType());
	size = Expr::createPointer(elementSize);

	if (ai->isArrayAllocation()) {
		ref<Expr> count = eval(ki, 0, state).value;
		count = Expr::createCoerceToPointerType(count);
		size = MulExpr::create(size, count);
	}

	isLocal = i->getOpcode() == Instruction::Alloca;
	executeAlloc(state, size, isLocal, ki);
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki)
{
  Instruction *i = ki->getInst();

  switch (i->getOpcode()) {
  // Memory instructions...
  // case Instruction::Malloc:
  case Instruction::Alloca:
  	instAlloc(state, ki);
  	break;
   // Control flow
  case Instruction::Ret:
	if (WriteTraces) {
		state.exeTraceMgr.addEvent(
			new FunctionReturnTraceEvent(state, ki));
	}
	instRet(state, ki);
	break;

  case Instruction::Unwind:
    instUnwind(state);
    break;

  case Instruction::Br: instBranch(state, ki); break;
  case Instruction::Switch: instSwitch(state, ki); break;
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call:
    instCall(state, ki);
    break;
  case Instruction::PHI: {
    ref<Expr> result = eval(ki, state.getPHISlot(), state).value;
    state.bindLocal(ki, result);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(ki->getInst());
    assert(SI->getCondition() == SI->getOperand(0) &&
           "Wrong operand index!");
    ref<Expr> cond = eval(ki, 0, state).value;
    ref<Expr> tExpr = eval(ki, 1, state).value;
    ref<Expr> fExpr = eval(ki, 2, state).value;
    ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
    state.bindLocal(ki, result);
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

  // Arithmetic / logical
#define INST_ARITHOP(x,y)				\
  case Instruction::x : {				\
    VectorType*	vt;				\
    ref<Expr> left = eval(ki, 0, state).value;		\
    ref<Expr> right = eval(ki, 1, state).value;		\
    vt = dyn_cast<VectorType>(ki->getInst()->getOperand(0)->getType()); \
    if (vt) { 				\
	SETUP_VOP(vt);			\
	V_OP_PREPEND(x);		\
	state.bindLocal(ki, result);	\
	break;				\
    }					\
    state.bindLocal(ki, y::create(left, right));     \
    break; }

#define INST_DIVOP(x,y)						\
	case Instruction::x : {					\
	VectorType		*vt;				\
	ExecutionState	*ok_state, *bad_state;			\
	ref<Expr> left = eval(ki, 0, state).value;		\
	ref<Expr> right = eval(ki, 1, state).value;		\
	bad_state = ok_state = NULL;				\
	if (!isa<ConstantExpr>(right)) {			\
		StatePair	sp = fork(			\
			state,					\
			EqExpr::create(				\
				right,					\
				ConstantExpr::create(0, right->getWidth())), \
			true);					\
		bad_state = sp.first;				\
		ok_state = sp.second;				\
	} else if (right->isZero()) {				\
		bad_state = &state;				\
	} else {						\
		ok_state = &state;				\
	}							\
	if (bad_state != NULL) {				\
		terminateStateOnError(*bad_state, 		\
			"Tried to divide by zero!",		\
			"div.err");				\
	}							\
	if (ok_state == NULL) break;				\
	vt = dyn_cast<VectorType>(ki->getInst()->getOperand(0)->getType()); \
	if (vt) { 						\
		SETUP_VOP(vt);					\
		V_OP_PREPEND(x);				\
		ok_state->bindLocal(ki, result);		\
		break;						\
	}							\
	ok_state->bindLocal(ki, y::create(left, right));	\
	break; }

  INST_ARITHOP(Add,AddExpr)
  INST_ARITHOP(Sub,SubExpr)
  INST_ARITHOP(Mul,MulExpr)
  INST_DIVOP(UDiv,UDivExpr)
  INST_DIVOP(SDiv,SDivExpr)
  INST_DIVOP(URem,URemExpr)
  INST_DIVOP(SRem,SRemExpr)
  INST_ARITHOP(And,AndExpr)
  INST_ARITHOP(Or,OrExpr)
  INST_ARITHOP(Xor,XorExpr)
  INST_ARITHOP(Shl,ShlExpr)
  INST_ARITHOP(LShr,LShrExpr)
  INST_ARITHOP(AShr,AShrExpr)


  case Instruction::ICmp: instCmp(state, ki); break;

  case Instruction::Load: {
    ref<Expr> base = eval(ki, 0, state).value;
    mmu->exeMemOp(state, MMU::MemOp(false, base, 0, ki));
    break;
  }
  case Instruction::Store: {
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;

    mmu->exeMemOp(state, MMU::MemOp(true, base, value, 0));
    break;
  }

  case Instruction::GetElementPtr: {
    instGetElementPtr(state, ki);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ExtractExpr::create(
      eval(ki, 0, state).value,
      0,
      kmodule->getWidthForLLVMType(ci->getType()));

    state.bindLocal(ki, result);
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ZExtExpr::create(
      eval(ki, 0, state).value,
      kmodule->getWidthForLLVMType(ci->getType()));

    state.bindLocal(ki, result);
    break;
  }
  case Instruction::SExt: {
    CastInst 		*ci = cast<CastInst>(i);
    VectorType	*vt_src, *vt_dst;
    ref<Expr>		result, evaled;

    vt_src = dyn_cast<VectorType>(ci->getSrcTy());
    vt_dst = dyn_cast<VectorType>(ci->getDestTy());
    evaled =  eval(ki, 0, state).value;
    if (vt_src) {
      result = sextVector(state, evaled, vt_src, vt_dst);
    } else {
      result = SExtExpr::create(
        evaled,
        kmodule->getWidthForLLVMType(ci->getType()));
    }
    state.bindLocal(ki, result);
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = kmodule->getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    state.bindLocal(ki, ZExtExpr::create(arg, pType));
    break;
  }
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = kmodule->getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    state.bindLocal(ki, ZExtExpr::create(arg, iType));
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, state).value;
    state.bindLocal(ki, result);
    break;
  }

    // Floating point arith instructions
#define INST_FOP_ARITH(x,y)					\
  case Instruction::x: {					\
    ref<ConstantExpr> left, right;				\
    right = toConstant(state, eval(ki, 1, state).value, "floating point");	\
    left = toConstant(state, eval(ki, 0, state).value, "floating point");	\
    if (!fpWidthToSemantics(left->getWidth()) ||				\
        !fpWidthToSemantics(right->getWidth()))					\
      return terminateStateOnExecError(state, "Unsupported "#x" operation");	\
	\
    llvm::APFloat Res(left->getAPValue());					\
    Res.y(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);		\
    state.bindLocal(ki, ConstantExpr::alloc(Res.bitcastToAPInt()));		\
    break; }

INST_FOP_ARITH(FAdd, add)
INST_FOP_ARITH(FSub, subtract)
INST_FOP_ARITH(FMul, multiply)
INST_FOP_ARITH(FDiv, divide)
INST_FOP_ARITH(FRem, mod)

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = kmodule->getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

    llvm::APFloat Res(arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    state.bindLocal(ki, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = kmodule->getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");

    llvm::APFloat Res(arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    state.bindLocal(ki, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = kmodule->getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

    llvm::APFloat Arg(arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    state.bindLocal(ki, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = kmodule->getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");

    llvm::APFloat Arg(arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    state.bindLocal(ki, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = kmodule->getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    state.bindLocal(ki, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = kmodule->getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    state.bindLocal(ki, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

    APFloat LHS(left->getAPValue());
    APFloat RHS(right->getAPValue());
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    state.bindLocal(ki,
      ConstantExpr::alloc(
        isFPPredicateMatched(CmpRes, fi->getPredicate()),
        Expr::Bool));
    break;
  }

  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;
    ref<Expr> val = eval(ki, 1, state).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset, rOffset;

    lOffset = kgepi->getOffsetBits()*8;
    rOffset = kgepi->getOffsetBits()*8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    state.bindLocal(ki, result);
    break;
  }

  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    ref<Expr> agg, result;
    agg = eval(ki, 0, state).value;
    result = ExtractExpr::create(
      agg, kgepi->getOffsetBits()*8, kmodule->getWidthForLLVMType(i->getType()));

    state.bindLocal(ki, result);
    break;
  }


  // Vector instructions...
  case Instruction::ExtractElement:
    instExtractElement(state, ki);
    break;
  case Instruction::InsertElement:
    instInsertElement(state, ki);
    break;
  case Instruction::ShuffleVector:
    instShuffleVector(state, ki);
    break;

  default:
    if (isMalloc(i)) {
      instAlloc(state, ki);
      break;
    } else if (isFreeCall(i)) {
      executeFree(state, eval(ki, 0, state).value);
      break;
    }

    std::cerr << "OOPS! ";
    i->dump();
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::removePTreeState(
	ExecutionState* es,
	ExecutionState** root_to_be_removed)
{
	ExecutionState	*root;

	std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3;
	it3 = seedMap.find(es);
	if (it3 != seedMap.end()) seedMap.erase(it3);

	root = pathTree->removeState(stateManager, es);
	if (root != NULL) *root_to_be_removed = root;
}

void Executor::removeRoot(ExecutionState* es)
{ pathTree->removeRoot(stateManager, es); }

void Executor::killStates(ExecutionState* &state)
{
  // just guess at how many to kill
  uint64_t numStates = stateManager->size();
  uint64_t mbs = getMemUsageMB();
  unsigned toKill = std::max((uint64_t)1, numStates - (numStates*MaxMemory)/mbs);
  assert (mbs > MaxMemory);

  klee_warning(
    "killing %u states (over memory cap). Total states = %ld.",
    toKill, numStates);

  std::vector<ExecutionState*> arr(
    stateManager->begin(),
    stateManager->end());

  // use priority ordering for selecting which states to kill
  std::partial_sort(
    arr.begin(), arr.begin() + toKill, arr.end(), KillOrCompactOrdering());
  for (unsigned i = 0; i < toKill; ++i) {
    terminateStateEarly(*arr[i], "memory limit");
    if (state == arr[i]) state = NULL;
  }
  klee_message("Killed %u states.", toKill);
}

void Executor::stepStateInst(ExecutionState* &state)
{
	assert (state->checkCanary() && "Not a valid state");

	state->lastChosen = stats::instructions;

	KInstruction *ki = state->pc;
	assert(ki);

	stepInstruction(*state);
	executeInstruction(*state, ki);
	processTimers(state, MaxInstructionTime);
}

void Executor::handleMemoryUtilization(ExecutionState* &state)
{
	uint64_t mbs;

	if (!(MaxMemory && (stats::instructions & 0xFFFF) == 0))
		return;

	// We need to avoid calling GetMallocUsage() often because it
	// is O(elts on freelist). This is really bad since we start
	// to pummel the freelist once we hit the memory cap.
	if (UsePID && MaxMemory) {
		handleMemoryPID(state);
		return;
	}

	mbs = getMemUsageMB();
	if (mbs < 0.9*MaxMemory) {
		atMemoryLimit = false;
		return;
	}

	if (mbs <= MaxMemory) return;

	/*  (mbs > MaxMemory) */
	atMemoryLimit = true;
	onlyNonCompact = true;

	if (mbs <= MaxMemory + 100)
		return;

	/* Ran memory to the roof. FLIP OUT. */
	if 	(ReplayInhibitedForks == false ||
		/* resort to killing states if the recent compacting
		didn't help to reduce the memory usage */
		stats::instructions-
		lastMemoryLimitOperationInstructions <= 0x20000)
	{
		killStates(state);
	} else {
		stateManager->compactPressureStates(state, MaxMemory);
	}

	lastMemoryLimitOperationInstructions = stats::instructions;
}

void Executor::stepSeedInst(ExecutionState* &lastState)
{
	std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it;

	it = seedMap.upper_bound(lastState);
	if (it == seedMap.end()) it = seedMap.begin();
	lastState = it->first;

	unsigned numSeeds = it->second.size();
	ExecutionState &state = *lastState;
	KInstruction *ki = state.pc;

	stepInstruction(state);
	executeInstruction(state, ki);
	processTimers(&state, MaxInstructionTime * numSeeds);
	notifyCurrent(&state);
}

bool Executor::seedRun(ExecutionState& initialState)
{
  ExecutionState *lastState = 0;
  double lastTime, startTime = lastTime = util::estWallTime();
  std::vector<SeedInfo> &v = seedMap[&initialState];

  foreach (it, usingSeeds->begin(), usingSeeds->end()) {
    v.push_back(SeedInfo(*it));
  }

  int lastNumSeeds = usingSeeds->size()+10;
  while (!seedMap.empty() && !haltExecution) {
    double time;

    stepSeedInst(lastState);

    /* every 1000 instructions, check timeouts, seed counts */
    if ((stats::instructions % 1000) != 0) continue;

    unsigned numSeeds = 0;
    unsigned numStates = seedMap.size();
    foreach (it, seedMap.begin(), seedMap.end()) {
    	numSeeds += it->second.size();
    }

    time = util::estWallTime();
    if (SeedTime>0. && time > startTime + SeedTime) {
      klee_warning("seed time expired, %d seeds remain over %d states",
                   numSeeds, numStates);
      break;
    } else if ((int)numSeeds<=lastNumSeeds-10 || time >= lastTime+10) {
      lastTime = time;
      lastNumSeeds = numSeeds;
      klee_message("%d seeds remaining over: %d states", numSeeds, numStates);
    }
  }

  if (haltExecution) return false;

  klee_message("seeding done (%d states remain)", (int) stateManager->size());

  // XXX total hack, just because I like non uniform better but want
  // seed results to be equally weighted.
  stateManager->setWeights(1.0);
  return true;
}

unsigned Executor::getNumStates(void) const
{ return stateManager->size(); }

unsigned Executor::getNumFullStates(void) const
{ return stateManager->getNonCompactStateCount(); }


void Executor::replayPathsIntoStates(ExecutionState& initialState)
{
	assert (replayPaths);
	foreach (it, replayPaths->begin(), replayPaths->end()) {
		ExecutionState *newState;
		newState = ExecutionState::createReplay(initialState, (*it));
		pathTree->splitStates(
			newState->ptreeNode, &initialState, newState);
		stateManager->queueAdd(newState);
	}
}

void Executor::run(ExecutionState &initialState)
{
	// Delay init till now so that ticks don't accrue during
	// optimization and such.
	initTimers();

	initialStateCopy = (ReplayInhibitedForks) ? initialState.copy() : NULL;

	if (replayPaths != NULL)
		replayPathsIntoStates(initialState);

	stateManager->setInitialState(this, &initialState, replayPaths);

	if (usingSeeds) {
		if (!seedRun(initialState)) goto dump;
		if (OnlySeed) goto dump;
	}

	stateManager->setupSearcher(this);

	runLoop();

	stateManager->teardownUserSearcher();

dump:
	if (stateManager->empty()) goto done;
	std::cerr << "KLEE: halting execution, dumping remaining states\n";
	foreach (it, stateManager->begin(), stateManager->end()) {
		ExecutionState &state = **it;
		stepInstruction(state); // keep stats rolling
		if (DumpStatesOnHalt)
			terminateStateEarly(state, "execution halting");
		else
			terminateState(state);
	}
	notifyCurrent(0);

done:
	if (initialStateCopy) delete initialStateCopy;
}

void Executor::runLoop(void)
{
	ExecutionState* last_state;
	while (!stateManager->empty() && !haltExecution) {
		ExecutionState *state;

		state = stateManager->selectState(!onlyNonCompact);
		if (last_state != state && DumpSelectStack) {
			std::cerr << "StackTrace for st=" << (void*)state << '\n';
			printStackTrace(*state, std::cerr);
			std::cerr << "===================\n";
		}

		assert (state != NULL &&
			"State man not empty, but selectState is?");

		/* decompress state if compact */
		if (state->isCompact()) {
			ExecutionState* newState;

			assert (initialStateCopy != NULL);
			newState = state->reconstitute(*initialStateCopy);
			stateManager->replaceState(state, newState);

			notifyCurrent(state);
			state = newState;
		}

		stepStateInst(state);

		handleMemoryUtilization(state);
		notifyCurrent(state);
		last_state = state;
	}
}

void Executor::notifyCurrent(ExecutionState* current)
{
	stateManager->commitQueue(this, current);
	if (	stateManager->getNonCompactStateCount() == 0
		&& !stateManager->empty())
	{
		onlyNonCompact = false;
	}
}

std::string Executor::getAddressInfo(
	ExecutionState &state,
	ref<Expr> address) const
{
	std::ostringstream	info;
	uint64_t		example;

	info << "\taddress: ";
	if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
		example = CE->getZExtValue();
		info << (void*)example << "\n";
	} else {
		std::pair< ref<Expr>, ref<Expr> > res;
		ref<ConstantExpr>       value;
		bool                    ok;

		info << address << "\n";
		ok = solver->getValue(state, address, value);
		if (ok == false) {
			info << "\texample: ???\n";
			return info.str();
		}

		assert(ok && "FIXME: Unhandled solver failure");

		example = value->getZExtValue();
		info << "\texample: " << example << "\n";
		ok = solver->getRange(state, address, res);
		if (ok == false) {
			return info.str();
		}

		info << "\trange: [" << res.first << ", " << res.second <<"]\n";
	}

	state.addressSpace.printAddressInfo(info, example);

	return info.str();
}

void Executor::yield(ExecutionState& state) { stateManager->yield(&state); }

void Executor::terminateState(ExecutionState &state)
{
	if (replayOut && replayPosition!=replayOut->numObjects) {
		klee_warning_once(
			replayOut,
			"replay did not consume all objects in test input.");
	}

	interpreterHandler->incPathsExplored();

	if (!stateManager->isAddedState(&state)) {
		state.pc = state.prevPC;
		stateManager->queueRemove(&state);
		return;
	}

	// never reached searcher, just delete immediately
	std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3;
	it3 = seedMap.find(&state);
	if (it3 != seedMap.end()) seedMap.erase(it3);

	stateManager->dropAdded(&state);
	pathTree->remove(state.ptreeNode);
	delete &state;
}

void Executor::terminateStateEarly(
	ExecutionState &state,
	const Twine &message)
{
	if (	!OnlyOutputStatesCoveringNew ||
		state.coveredNew ||
		(AlwaysOutputSeeds && seedMap.count(&state)))
	{
		std::stringstream	ss;

		ss << message.str() << '\n';
		printStackTrace(state, ss);
		interpreterHandler->processTestCase(
			state, ss.str().c_str(), "early");
	}

	terminateState(state);
}

void Executor::terminateStateOnExit(ExecutionState &state)
{
	if (	!OnlyOutputStatesCoveringNew ||
		state.coveredNew ||
		(AlwaysOutputSeeds && seedMap.count(&state)))
	{
		interpreterHandler->processTestCase(state, 0, 0);
	}

	terminateState(state);
}

void Executor::terminateStateOnError(
	ExecutionState &state,
	const llvm::Twine &messaget,
	const char *suffix,
	const llvm::Twine &info)
{
	std::string message = messaget.str();
	static std::set< std::pair<Instruction*, std::string> > emittedErrors;

	if (!EmitAllErrors) {
		bool	new_err;
		new_err = emittedErrors.insert(
			std::make_pair(
				state.prevPC->getInst(), message)).second;
		if (new_err == false) {
			terminateState(state);
			return;
		}
	}

	std::ostringstream msg;
	printStateErrorMessage(state, message, msg);

	std::string info_str = info.str();
	if (info_str != "") msg << "Info: \n" << info_str;

	interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);

	terminateState(state);
}

void Executor::printStateErrorMessage(
	ExecutionState& state,
	const std::string& message,
	std::ostream& os)
{
	const InstructionInfo &ii = *state.prevPC->getInfo();
	if (ii.file != "") {
		klee_message("ERROR: %s:%d: %s",
			ii.file.c_str(),
			ii.line,
			message.c_str());
	} else {
		klee_message("ERROR: %s", message.c_str());
	}

	if (!EmitAllErrors)
		klee_message("NOTE: now ignoring this error at this location");

	os << "Error: " << message << "\n";
	if (ii.file != "") {
		os << "File: " << ii.file << "\n";
		os << "Line: " << ii.line << "\n";
	}

	os << "Stack: \n";
	printStackTrace(state, os);
}

void Executor::printStackTrace(ExecutionState& st, std::ostream& os) const
{ st.dumpStack(os); }

void Executor::resolveExact(ExecutionState &state,
                            ref<Expr> p,
                            ExactResolutionList &results,
                            const std::string &name)
{
	// XXX we may want to be capping this?
	ResolutionList rl;
	state.addressSpace.resolve(state, solver, p, rl);

	ExecutionState *unbound = &state;
	foreach (it, rl.begin(), rl.end()) {
		ref<Expr> inBounds;

		inBounds = EqExpr::create(p, it->first->getBaseExpr());

		StatePair branches = fork(*unbound, inBounds, true);

		if (branches.first)
			results.push_back(
				std::make_pair(*it, branches.first));

		unbound = branches.second;
		if (!unbound) // Fork failure
			break;
	}

	if (unbound) {
		terminateStateOnError(
			*unbound,
			"memory error: invalid pointer: " + name,
			"ptr.err",
			getAddressInfo(*unbound, p));
	}
}

ObjectState* Executor::executeMakeSymbolic(
  ExecutionState &state, const MemoryObject *mo, const char* arrName)
{
  return executeMakeSymbolic(state, mo, mo->getSizeExpr(), arrName);
}

ObjectState* Executor::executeMakeSymbolic(
  ExecutionState &state,
  const MemoryObject *mo,
  ref<Expr> len,
  const char* arrName)
{
	if (!replayOut) return makeSymbolic(state, mo, len, arrName);
	else return makeSymbolicReplay(state, mo, len);
}

ObjectState* Executor::makeSymbolic(
  ExecutionState& state,
  const MemoryObject* mo,
  ref<Expr> len,
  const char* arrPrefix)
{
	static unsigned	id = 0;
	ObjectState	*os;
	Array		*array;

	array = new Array(arrPrefix + llvm::utostr(++id), mo->mallocKey, 0, 0);
	array->initRef();
	os = state.bindMemObj(mo, array);
	state.addSymbolic(const_cast<MemoryObject*>(mo) /* yuck */, array);

	addSymbolicToSeeds(state, mo, array);

	return os;
}

void Executor::addSymbolicToSeeds(
	ExecutionState& state,
	const MemoryObject* mo,
	const  Array* array)
{
	std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it;
	it = seedMap.find(&state);
	if (it == seedMap.end()) return;

	// In seed mode we need to add this as a binding.
	foreach (siit, it->second.begin(), it->second.end()) {
		if (!seedObject(state, *siit, mo, array))
			break;
	}
}

// Create a new object state for the memory object (instead of a copy).
ObjectState* Executor::makeSymbolicReplay(
	ExecutionState& state, const MemoryObject* mo, ref<Expr> len)
{
	ObjectState *os = state.bindMemObj(mo);
	if (replayPosition >= replayOut->numObjects) {
		terminateStateOnError(
			state, "replay count mismatch", "user.err");
		return os;
	}

	KTestObject *obj = &replayOut->objects[replayPosition++];
	if (obj->numBytes != mo->size) {
		terminateStateOnError(
			state, "replay size mismatch", "user.err");
	} else {
		for (unsigned i=0; i<mo->size; i++) {
			state.write8(os, i, obj->bytes[i]);
		}
	}

	return os;
}

bool Executor::seedObject(
  ExecutionState& state, SeedInfo& si,
  const MemoryObject* mo, const Array* array)
{
  KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

  /* if no test objects, create zeroed array object */
  if (!obj) {
    if (ZeroSeedExtension) {
      std::vector<unsigned char> &values = si.assignment.bindings[array];
      values = std::vector<unsigned char>(mo->size, '\0');
    } else if (!AllowSeedExtension) {
      terminateStateOnError(state,
                            "ran out of inputs during seeding",
                            "user.err");
      return false;
    }
    return true;
  }

  /* resize permitted? */
  if (obj->numBytes != mo->size &&
      ((!(AllowSeedExtension || ZeroSeedExtension)
        && obj->numBytes < mo->size) ||
       (!AllowSeedTruncation && obj->numBytes > mo->size)))
  {
    std::stringstream msg;
    msg << "replace size mismatch: "
    << mo->name << "[" << mo->size << "]"
    << " vs " << obj->name << "[" << obj->numBytes << "]"
    << " in test\n";

    terminateStateOnError(state, msg.str(), "user.err");
    return false;
  }

  /* resize object to memory size */
  std::vector<unsigned char> &values = si.assignment.bindings[array];
  values.insert(values.begin(), obj->bytes,
                obj->bytes + std::min(obj->numBytes, mo->size));
  if (ZeroSeedExtension) {
    for (unsigned i=obj->numBytes; i<mo->size; ++i)
      values.push_back('\0');
  }

  return true;
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state)
{
	assert(symPathWriter != NULL);
	return state.symPathOS.getID();
}

void Executor::getConstraintLogCVC(
	const ExecutionState &state, std::string &res)
{
	Query		query(
		state.constraints,
		ConstantExpr::alloc(0, Expr::Bool));
	STPSolver	*stpSolver;
	char		*log;

	stpSolver = dynamic_cast<STPSolver*>(solver->timedSolver);
	if (stpSolver == NULL) {
		res = "Timed solver is not STP! Can't get CVC Log";
		return;
	}

	log = stpSolver->getConstraintLog(query);
	res = std::string(log);
	free(log);
}

void Executor::getConstraintLog(
	const ExecutionState &state,
	std::string &res,
	bool asCVC)
{
	if (asCVC) {
		getConstraintLogCVC(state, res);
		return;
	}

	std::ostringstream info;
	ExprPPrinter::printConstraints(info, state.constraints);
	res = info.str();
}

void Executor::getSymbolicSolutionCex(
  const ExecutionState& state, ExecutionState& tmp)
{
  foreach (sym_it, state.symbolicsBegin(), state.symbolicsEnd()) {
    const MemoryObject				*mo;
    std::vector< ref<Expr> >::const_iterator	pi, pie;

    mo = sym_it->getMemoryObject();
    pi = mo->cexPreferences.begin();
    pie = mo->cexPreferences.end();

    for (; pi != pie; ++pi) {
      bool mustBeTrue, success;
      success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi), mustBeTrue);
      if (!success) break;
      if (!mustBeTrue) tmp.addConstraint(*pi);
    }
    if (pi!=pie) break;
  }
}

bool Executor::getSymbolicSolution(
	const ExecutionState &state,
	std::vector<
		std::pair<std::string,
			std::vector<unsigned char> > > &res)
{
	ExecutionState		tmp(state);
	bool			success;

	if (PreferCex) {
		getSymbolicSolutionCex(state, tmp);
	}

	std::vector<const Array*> objects;
	foreach (it, state.symbolicsBegin(), state.symbolicsEnd())
		objects.push_back(it->getArray());

	Assignment a(objects);

	success = solver->getInitialValues(tmp, a);
	if (!success) {
		klee_warning(
			"unable to compute initial values "
			"(invalid constraints?)!");
		ExprPPrinter::printQuery(
			std::cerr,
			state.constraints,
			ConstantExpr::alloc(0, Expr::Bool));
		return false;
	}

	foreach (it, state.symbolicsBegin(), state.symbolicsEnd()) {
		const std::vector<unsigned char>	*v;

		v = a.getBinding(it->getArray());
		res.push_back(
			std::make_pair(
				it->getMemoryObject()->name, *v));
	}

	return true;
}

void Executor::doImpliedValueConcretization(
	ExecutionState &state,
	ref<Expr> e,
	ref<ConstantExpr> value)
{
	if (DebugCheckForImpliedValues)
		ImpliedValue::checkForImpliedValues(
			solver->solver, e, value);

	ImpliedValueList results;
	ImpliedValue::getImpliedValues(e, value, results);

	foreach (it, results.begin(), results.end()) {
		const MemoryObject	*mo;
		const ObjectState	*os;
		ObjectState		*wos;
		ReadExpr		*re = it->first.get();
		ConstantExpr		*off = dyn_cast<ConstantExpr>(re->index);

		if (off == NULL) continue;

		mo = state.findMemoryObject(re->updates.root);
		if (mo == NULL)
			continue;

		assert (mo != NULL && "Could not find MO?");
		os = state.addressSpace.findObject(mo);

		// os = 0 => obj has been free'd,
		// no need to concretize (although as in other cases we
		// would like to concretize the outstanding
		// reads, but we have no facility for that yet)
		if (os == NULL) continue;

		assert(	!os->readOnly &&
			"not possible? read only object with static read?");

		wos = state.addressSpace.getWriteable(mo, os);
		assert (wos != NULL && "Could not get writable ObjectState?");

		wos->writeIVC(off->getZExtValue(), it->second);
	}
}

void Executor::instGetElementPtr(ExecutionState& state, KInstruction *ki)
{
	KGEPInstruction		*kgepi;
	ref<Expr>		base;

	kgepi = static_cast<KGEPInstruction*>(ki);
	base = eval(ki, 0, state).value;

	foreach (it, kgepi->indices.begin(), kgepi->indices.end()) {
		uint64_t elementSize = it->second;
		ref<Expr> index = eval(ki, it->first, state).value;

		base = AddExpr::create(
			base,
			MulExpr::create(
				Expr::createCoerceToPointerType(index),
				Expr::createPointer(elementSize)));
	}

	if (kgepi->getOffsetBits())
		base = AddExpr::create(
			base,
			Expr::createPointer(kgepi->getOffsetBits()));

	state.bindLocal(ki, base);
}

void Executor::bindModuleConstants(void)
{
	foreach (it, kmodule->kfuncsBegin(), kmodule->kfuncsEnd()) {
		bindKFuncConstants(*it);
	}

	kmodule->bindModuleConstTable(this);
}

void Executor::bindKFuncConstants(KFunction* kf)
{
	for (unsigned i=0; i<kf->numInstructions; ++i)
		bindInstructionConstants(kf->instructions[i]);
}

void Executor::bindInstructionConstants(KInstruction *KI)
{
	KGEPInstruction* kgepi;

	kgepi = dynamic_cast<KGEPInstruction*>(KI);
	if (kgepi == NULL)
		return;

	kgepi->resolveConstants(kmodule, globals);
}

void Executor::executeAllocConst(
	ExecutionState &state,
	uint64_t sz,
	bool isLocal,
	KInstruction *target,
	bool zeroMemory,
	const ObjectState *reallocFrom)
{
	ObjectState *os;

	os = state.allocate(sz, isLocal, false, state.prevPC->getInst());
	if (os == NULL) {
		state.bindLocal(
			target,
			ConstantExpr::alloc(
				0, Context::get().getPointerWidth()));
		return;
	}

	if (zeroMemory)
		os->initializeToZero();
	else
		os->initializeToRandom();

	state.bindLocal(target, os->getObject()->getBaseExpr());

	if (reallocFrom) {
		unsigned count = std::min(reallocFrom->size, os->size);

		state.copy(os, reallocFrom, count);
		/*(for (unsigned i=0; i<count; i++) {
		//os->write(i, reallocFrom->read8(i));
		//state.write(os, i, state.read8(reallocFrom, i));
		}*/
		state.unbindObject(reallocFrom->getObject());
	}
}

klee::ref<klee::ConstantExpr> Executor::getSmallSymAllocSize(
	ExecutionState &state, ref<Expr>& size)
{
	Expr::Width		W;
	ref<ConstantExpr>	example;
	ref<ConstantExpr>	ce_128;
	bool			ok;

	ok = solver->getValue(state, size, example);
	assert(ok && "FIXME: Unhandled solver failure");

	// start with a small example
	W = example->getWidth();
	ce_128 = ConstantExpr::alloc(128, W);
	while (example->Ugt(ce_128)->isTrue()) {
		ref<ConstantExpr>	tmp;
		bool			res;

		tmp = example->LShr(ConstantExpr::alloc(1, W));
		ok = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
		assert(ok && "FIXME: Unhandled solver failure");
		if (!res)
			break;
		example = tmp;
	}

	return example;
}


void Executor::executeAllocSymbolic(
	ExecutionState &state,
	ref<Expr> size,
	bool isLocal,
	KInstruction *target,
	bool zeroMemory,
	const ObjectState *reallocFrom)
{
	// XXX For now we just pick a size. Ideally we would support
	// symbolic sizes fully but even if we don't it would be better to
	// "smartly" pick a value, for example we could fork and pick the
	// min and max values and perhaps some intermediate (reasonable
	// value).
	//
	// It would also be nice to recognize the case when size has
	// exactly two values and just fork (but we need to get rid of
	// return argument first). This shows up in pcre when llvm
	// collapses the size expression with a select.
	//
	ref<ConstantExpr>	example(getSmallSymAllocSize(state, size));
	Expr::Width		W = example->getWidth();

	StatePair fixedSize = fork(state, EqExpr::create(example, size), true);

	if (fixedSize.second) {
		// Check for exactly two values
		ExecutionState		*ne_state;
		ref<ConstantExpr>	tmp;
		bool			ok, res;

		ne_state = fixedSize.second;
		ok = solver->getValue(*ne_state, size, tmp);
		assert(ok && "FIXME: Unhandled solver failure");
		ok = solver->mustBeTrue(*ne_state, EqExpr::create(tmp, size), res);
		assert(ok && "FIXME: Unhandled solver failure");

		if (res) {
			executeAlloc(
				*ne_state,
				tmp,
				isLocal,
				target, zeroMemory, reallocFrom);
		} else {
		// See if a *really* big value is possible. If so assume
		// malloc will fail for it, so lets fork and return 0.
			StatePair hugeSize = fork(
				*ne_state,
				UltExpr::create(ConstantExpr::alloc(1<<31, W), size),
				true);
			if (hugeSize.first) {
				klee_message("NOTE: found huge malloc, returing 0");
				hugeSize.first->bindLocal(
					target,
					ConstantExpr::alloc(
						0,
						Context::get().getPointerWidth()));
			}

			if (hugeSize.second) {
				std::ostringstream info;
				ExprPPrinter::printOne(info, "  size expr", size);
				info << "  concretization : " << example << "\n";
				info << "  unbound example: " << tmp << "\n";
				terminateStateOnError(
					*hugeSize.second,
					"concretized symbolic size",
					"model.err",
					info.str());
			}
		}
	}

	// can be zero when fork fails
	if (fixedSize.first) {
		executeAlloc(
			*fixedSize.first,
			example,
			isLocal,
			target, zeroMemory, reallocFrom);
	}
}

void Executor::executeAlloc(
	ExecutionState &state,
	ref<Expr> size,
	bool isLocal,
	KInstruction *target,
	bool zeroMemory,
	const ObjectState *reallocFrom)
{
	size = toUnique(state, size);
	if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
		executeAllocConst(
			state, CE->getZExtValue(),
			isLocal, target, zeroMemory, reallocFrom);
	} else {
		executeAllocSymbolic(
			state, size, isLocal, target, zeroMemory, reallocFrom);
	}
}

void Executor::executeFree(
	ExecutionState &state,
	ref<Expr> address,
	KInstruction *target)
{
	StatePair zeroPointer = fork(state, Expr::createIsZero(address), true);

	if (zeroPointer.first && target) {
		zeroPointer.first->bindLocal(target, Expr::createPointer(0));
	}

	if (!zeroPointer.second)
		return;

	// address != 0
	ExactResolutionList rl;
	resolveExact(*zeroPointer.second, address, rl, "free");

	foreach (it, rl.begin(), rl.end()) {
		const MemoryObject *mo;

		mo = it->first.first;
		if (mo->isLocal()) {
			terminateStateOnError(
				*it->second,
				"free of alloca",
				"free.err",
				getAddressInfo(*it->second, address));
		} else if (mo->isGlobal()) {
			terminateStateOnError(
				*it->second,
				"free of global",
				"free.err",
				getAddressInfo(*it->second, address));
		} else {
			it->second->unbindObject(mo);
			if (target)
				it->second->bindLocal(
					target,
					Expr::createPointer(0));
		}
	}
}

MemoryObject* Executor::findGlobalObject(const llvm::GlobalValue* gv) const
{
	assert (globals != NULL);
	return globals->findObject(gv);
}

#include <malloc.h>
void Executor::handleMemoryPID(ExecutionState* &state)
{
	#define K_P    0.6
	#define K_D    0.1     /* damping factor-- damp changes in errors */
	#define K_I    0.0001  /* systematic error-- negative while ramping  */
	unsigned                nonCompact_c;
	int                     states_to_gen;
	int64_t                 err;
	uint64_t                mbs;
	static int64_t          err_sum = -(int64_t)MaxMemory;
	static int64_t          last_err = 0;

	nonCompact_c = stateManager->getNonCompactStateCount();

	mbs = mallinfo().uordblks/(1024*1024);
	err = MaxMemory - mbs;

	states_to_gen =
		K_P*err +
		K_D*(err - last_err) +
		K_I*(err_sum);

	err_sum += err;
	last_err = err;

	if (states_to_gen < 0) {
		onlyNonCompact = false;
		stateManager->compactStates(state, -states_to_gen);
	}
}

std::string Executor::getPrettyName(llvm::Function* f) const
{ return f->getNameStr(); }

ExeStateSet::const_iterator Executor::beginStates(void) const
{ return stateManager->begin(); }
ExeStateSet::const_iterator Executor::endStates(void) const
{ return stateManager->end(); }

void Executor::xferIterInit(
	struct XferStateIter& iter,
	ExecutionState* state,
	KInstruction* ki)
{
	iter.v = eval(ki, 0, *state).value;
	iter.free = state;
	iter.getval_c = 0;
	iter.state_c = 0;
	iter.badjmp_c = 0;
}

#define MAX_BADJMP	10

bool Executor::xferIterNext(struct XferStateIter& iter)
{
	Function		*iter_f;
	ref<ConstantExpr>	value;

	iter_f = NULL;
	while (iter.badjmp_c < MAX_BADJMP) {
		uint64_t	addr;
		bool		ok;

		if (iter.free == NULL) return false;

		ok = solver->getValue(*(iter.free), iter.v, value);
		if (ok == false) {
			terminateStateEarly(
				*(iter.free),
				"solver died on xferIterNext");
			return false;
		}

		iter.getval_c++;
		iter.res = fork(*(iter.free), EqExpr::create(iter.v, value), true);
		iter.free = iter.res.second;

		if (iter.res.first == NULL) continue;

		addr = value->getZExtValue();
		iter.state_c++;
		iter_f = getFuncByAddr(addr);

		if (iter_f == NULL) {
			if (iter.badjmp_c == 0) {
				klee_warning_once(
					(void*) (unsigned long) addr,
					"invalid function pointer: %p",
					(void*)addr);
			}

			terminateStateOnError(
				*(iter.res.first),
				"xfer iter error: bad pointer",
				"badjmp.err");
			iter.badjmp_c++;
			continue;
		}

		// Don't give warning on unique resolution
		if (iter.res.second && iter.getval_c == 1) {
			klee_warning_once(
				(void*) (unsigned long) addr,
				"resolved symbolic function pointer to: %s",
				iter_f->getName().data());
		}

		break;
	}

	if (iter.badjmp_c >= MAX_BADJMP) {
		terminateStateOnError(
			*(iter.free),
			"xfer iter erorr: too many bad jumps",
			"badjmp.err");
		iter.free = NULL;
		return false;
	}

	assert (iter_f != NULL && "BAD FUNCTION TO JUMP TO");
	iter.f = iter_f;

	return true;
}
