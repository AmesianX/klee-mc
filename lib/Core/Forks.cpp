#include <llvm/Support/CommandLine.h>
#include <llvm/Instruction.h>
#include <iostream>
#include <sstream>

#include "klee/util/ExprVisitor.h"
#include "klee/util/ExprTimer.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Statistics.h"
#include "klee/Internal/ADT/RNG.h"
#include "StatsTracker.h"
#include "CoreStats.h"

#include "klee/TimerStatIncrementer.h"
#include "static/Sugar.h"

#include "CallPathManager.h"
#include "StateSolver.h"

#include "PTree.h"
#include "ExeStateManager.h"
#include "Forks.h"

/* XXX */
#include "../Solver/SMTPrinter.h"
#include "klee/Internal/ADT/LimitedStream.h"


extern bool		ReplayInhibitedForks;
namespace klee { extern RNG theRNG; }

namespace
{
	llvm::cl::opt<double>
		MaxStaticForkPct("max-static-fork-pct", llvm::cl::init(1.));
	llvm::cl::opt<double>
		MaxStaticSolvePct("max-static-solve-pct", llvm::cl::init(1.));
	llvm::cl::opt<double>
		MaxStaticCPForkPct("max-static-cpfork-pct", llvm::cl::init(1.));
	llvm::cl::opt<double>
	MaxStaticCPSolvePct("max-static-cpsolve-pct", llvm::cl::init(1.));
	llvm::cl::opt<unsigned>
		MaxDepth("max-depth",
			llvm::cl::desc("Only this many sym branches (0=off)"),
			llvm::cl::init(0));

	llvm::cl::opt<bool>
	ReplayPathOnly(
		"replay-path-only",
		llvm::cl::desc("On replay, kill states on branch exhaustion"),
		llvm::cl::init(false));

	llvm::cl::opt<bool>
	MaxMemoryInhibit(
		"max-memory-inhibit",
		llvm::cl::desc("Stop forking at memory cap (vs. random kill)"),
		llvm::cl::init(true));

	llvm::cl::opt<unsigned>
	MaxForks(
		"max-forks",
		llvm::cl::desc("Only fork this many times (-1=off)"),
		llvm::cl::init(~0u));

	llvm::cl::opt<bool>
	OnlyReplaySeeds(
		"only-replay-seeds", llvm::cl::desc("Drop seedless states."));

	llvm::cl::opt<bool>
	RandomizeFork("randomize-fork", llvm::cl::init(true));

	llvm::cl::opt<unsigned>
	ForkCondIdxMod("forkcond-idx-mod", llvm::cl::init(4));

	llvm::cl::opt<bool>
	QuenchRunaways(
		"quench-runaways",
		llvm::cl::desc("Drop states at heavily forking instructions."),
		llvm::cl::init(true));
}

using namespace klee;

unsigned Forks::quench_c = 0;
unsigned Forks::fork_c = 0;
unsigned Forks::fork_uniq_c = 0;


#define RUNAWAY_REFRESH	32
bool Forks::isRunawayBranch(KInstruction* ki)
{
	KBrInstruction	*kbr;
	double		stddevs;
	static int	count = 0;
	static double	stddev, mean, median;
	unsigned	forks, rand_mod;

	kbr = dynamic_cast<KBrInstruction*>(ki);
	if (kbr == NULL) {
		bool		is_likely;
		unsigned	mean;
		unsigned 	r;

		/* only rets and branches may be runaways */
		if (ki->getInst()->getOpcode() != llvm::Instruction::Ret)
			return false;

		/* XXX: FIXME. Need better scoring system. */
		//std::cerr << "RAND FOR (" << (void*)ki->getInst() << ") fc="
		//	<< ki->getForkCount() << ": ";
		// ki->getInst()->dump();

		mean = (fork_c+fork_uniq_c-1)/fork_uniq_c;
		r = rand() % (1 << (1+(10*ki->getForkCount())/mean));
		is_likely = (r != 0);
		return is_likely;
	}

	if ((count++ % RUNAWAY_REFRESH) == 0) {
		stddev = KBrInstruction::getForkStdDev();
		mean = KBrInstruction::getForkMean();
		median = KBrInstruction::getForkMedian();
	}

	if (stddev == 0)
		return false;

	forks = kbr->getForkHits();
	if (forks <= 5)
		return false;

	stddevs = ((double)(kbr->getForkHits() - mean))/stddev;
	if (stddevs < 1.0)
		return false;

	rand_mod = (1 << (1+(int)(((double)forks/(median)))));
	if ((theRNG.getInt31() % rand_mod) == 0)
		return false;

	return true;
}

/* TODO: understand this */
bool Forks::isForkingCallPath(CallPathNode* cpn)
{
	StatisticManager &sm = *theStatisticManager;
	if (	MaxStaticForkPct<1. &&
		sm.getIndexedValue(
			stats::forks, sm.getIndex()) >
				stats::forks*MaxStaticForkPct)
	{
		return true;
	}

	if (	MaxStaticSolvePct<1 &&
		sm.getIndexedValue(
			stats::solverTime, sm.getIndex()) >
				stats::solverTime*MaxStaticSolvePct)
	{
		return true;
	}

	/* next conditions require cpn anyway.. */
	if (cpn == NULL) return false;

	if (MaxStaticCPForkPct<1. &&
		cpn->statistics.getValue(stats::forks) >
			stats::forks*MaxStaticCPForkPct)
	{
		return true;
	}

	if (MaxStaticCPForkPct<1. &&
		(cpn->statistics.getValue(stats::solverTime) >
			stats::solverTime*MaxStaticCPSolvePct))
	{
		return true;
	}

	return false;
}

bool Forks::isForkingCondition(ExecutionState& current, ref<Expr> condition)
{
	if (exe.isStateSeeding(&current)) return false;

	if (isa<ConstantExpr>(condition)) return false;

	if (	!(MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
		MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.))
	{
		return false;
	}

	if (exe.getStatsTracker()->elapsed() > 60.) return false;

	return true;
}


Executor::StatePair
Forks::fork(ExecutionState &s, ref<Expr> cond, bool isInternal)
{
	ref<Expr>	conds[2];

	// !!! is this the correct behavior?
	if (isForkingCondition(s, cond)) {
		CallPathNode		*cpn;
		bool			ok;
		ref<ConstantExpr>	value;

		cpn = s.stack.back().callPathNode;
		if (isForkingCallPath(cpn)) {
			ok = exe.getSolver()->getValue(s, cond, value);
			assert(ok && "FIXME: Unhandled solver failure");

			exe.addConstrOrDie(s, EqExpr::create(value, cond));
			cond = value;
		}
	}

	// set in forkSetupNoSeeding, if possible
	//  conditions[0] = Expr::createIsZero(condition);
	conds[1] = cond;

	Executor::StateVector results;

	results = fork(s, 2, conds, isInternal, true);
	lastFork = std::make_pair(
		results[1] /* first label in br => true */,
		results[0] /* second label in br => false */);

	return lastFork;
}

Executor::StatePair Forks::forkUnconditional(
	ExecutionState &s, bool isInternal)
{
	ref<Expr>		conds[2];
	Executor::StateVector	results;

	conds[0] = ConstantExpr::create(1, 1);
	conds[1] = ConstantExpr::create(1, 1);

	/* NOTE: not marked as branch so that branch optimization is not
	 * applied-- giving it (true, true) would result in only one branch! */
	results = fork(s, 2, conds, isInternal, false);
	lastFork = std::make_pair(
		results[1] /* first label in br => true */,
		results[0] /* second label in br => false */);
	return lastFork;
}

bool Forks::forkFollowReplay(ExecutionState& current, struct ForkInfo& fi)
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
		<< ", prior-path target=" << targetIndex
		<< ", replay targets=";

	bool first = true;
	for(unsigned i = 0; i < fi.N; i++) {
		if (!fi.res[i]) continue;
		if (!first) ss << ",";
		ss << i;
		first = false;
	}
	ss << ")";
	exe.terminateOnError(current, ss.str().c_str(), "branch.err");

	for (unsigned i = 0; i < fi.N; i++) {
		if (fi.conditions[i].isNull())
			continue;
	}

	klee_warning("hit invalid branch in replay path mode");
	return false;
}

bool Forks::forkSetupNoSeeding(ExecutionState& current, struct ForkInfo& fi)
{
	if (!fi.isInternal && current.isCompact()) {
		// Can't fork compact states; sanity check
		assert(false && "invalid state");
	}

	if (!fi.isInternal && ReplayPathOnly &&
		current.isReplay && current.isReplayDone())
	{
		// Done replaying this state, so kill it (if -replay-path-only)
		exe.terminateEarly(current, "replay path exhausted");
		return false;
	}

	if (!fi.isInternal && current.isReplayDone() == false)
		return forkFollowReplay(current, fi);

	if (fi.validTargets <= 1)  return true;

	// Multiple branch directions are possible; check for flags that
	// prevent us from forking here
	assert(	!exe.getReplayOut() &&
		"in replay mode, only one branch can be true.");

	if (fi.isInternal) return true;

	const char* reason = 0;
	if (MaxMemoryInhibit && exe.isAtMemoryLimit())
		reason = "memory cap exceeded";
//	if (current.forkDisabled)
//		reason = "fork disabled on current path";
	if (exe.getInhibitForking())
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

void Forks::skipAndRandomPrune(struct ForkInfo& fi, const char* reason)
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

void Forks::forkSetupSeeding(ExecutionState& current, struct ForkInfo& fi)
{
	SeedMapType		&seedMap(exe.getSeedMap());
	SeedMapType::iterator	it(seedMap.find(&current));

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
			ref<ConstantExpr>	seedCondRes;
			bool			ok;
			ok = exe.getSolver()->getValue(
				current,
				siit->assignment.evaluate(fi.conditions[i]),
				seedCondRes);
			assert(ok && "FIXME: Unhandled solver failure");
			if (seedCondRes->isTrue())
				break;
		}

		// If we didn't find a satisfying condition, randomly pick one
		// (the seed will be patched).
		if (i == fi.N) i = theRNG.getInt32() % fi.N;

		fi.resSeeds[i].push_back(*siit);
	}

	// Clear any valid conditions that seeding rejects
	if ((fi.forkDisabled || OnlyReplaySeeds) && fi.validTargets > 1) {
		fi.validTargets = 0;
		for (unsigned i = 0; i < fi.N; i++) {
			if (fi.resSeeds[i].empty()) fi.res[i] = false;
			if (fi.res[i]) fi.validTargets++;
		}
		assert (fi.validTargets &&
			"seed must result in at least one valid target");
	}

	// Remove seeds corresponding to current state
	seedMap.erase(it);

	// !!! it's possible for the current state to end up with no seeds. Does
	// this matter? Old fork() used to handle it but branch() didn't.
}

// !!! for normal branch, conditions = {false,true} so that replay 0,1 reflects
// index
Executor::StateVector
Forks::fork(
	ExecutionState &current,
        unsigned N,
	ref<Expr> conditions[],
        bool isInternal,
	bool isBranch)
{
	ForkInfo			fi(conditions, N);


	fi.forkDisabled = current.forkDisabled;
	if (	QuenchRunaways &&
		!fi.forkDisabled &&
		(*current.prevPC).getForkCount() > 10)
	{
		fi.forkDisabled = isRunawayBranch(current.prevPC);
	}

	fi.isInternal = isInternal;
	fi.isBranch = isBranch;
	fi.isSeeding = exe.isStateSeeding(&current);

	if (evalForks(current, fi) == false) {
		exe.terminateEarly(current, "fork query timed out");
		return Executor::StateVector(N, NULL);
	}

	// need a copy telling us whether or not we need to add
	// constraints later; especially important if we skip a fork for
	// whatever reason
	fi.feasibleTargets = fi.validTargets;
	assert(fi.validTargets && "invalid set of fork conditions");

	fi.wasReplayed = false;
	if (fi.isSeeding) {
		forkSetupSeeding(current, fi);
	} else {
		if (!forkSetupNoSeeding(current, fi))
			return Executor::StateVector(N, NULL);
	}

	makeForks(current, fi);
	constrainForks(current, fi);


	if (fi.forkedTargets) {
		if (current.prevPC->getForkCount() == 0)
			fork_uniq_c++;
		current.prevPC->forked();
		fork_c += fi.forkedTargets;
	} else if (fi.validTargets > 1 && fi.forkDisabled) {
		quench_c++;
	}

	return fi.resStates;
}

bool Forks::evalForkBranch(ExecutionState& s, struct ForkInfo& fi)
{
	Solver::Validity	result;
	bool			ok;

	assert (fi.isBranch);

	ok = exe.getSolver()->evaluate(s, fi.conditions[1], result);
	if (!ok)
		return false;

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
bool Forks::evalForks(ExecutionState& current, struct ForkInfo& fi)
{
	if (fi.isBranch) {
		return evalForkBranch(current, fi);
	}

	assert (fi.isBranch == false);

	for (unsigned int condIndex = 0; condIndex < fi.N; condIndex++) {
		ConstantExpr	*CE;
		bool		result;

		CE = dyn_cast<ConstantExpr>(fi.conditions[condIndex]);
		// If condition is a constant
		// (e.g., from constant switch statement),
		// don't generate a query
		if (CE != NULL) {
			if (CE->isFalse()) result = false;
			else if (CE->isTrue()) result = true;
			else assert(false && "Invalid constant fork condition");
		} else {
			bool	ok;
			ok = exe.getSolver()->mayBeTrue(
				current,
				fi.conditions[condIndex],
				result);
			if (!ok)
				return false;
		}

		fi.res[condIndex] = result;
		if (result)
			fi.validTargets++;
	}

	return true;
}

ExecutionState* Forks::pureFork(ExecutionState& es, bool compact)
{
	// Update stats
	TimerStatIncrementer	timer(stats::forkTime);
	ExecutionState		*newState;

	++stats::forks;

	// Do actual state forking
	newState = es.branch(compact);
	es.ptreeNode->markReplay();

	// Update path tree with new states
	exe.getStateManager()->queueSplitAdd(es.ptreeNode, &es, newState);
	return newState;
}


void Forks::makeForks(ExecutionState& current, struct ForkInfo& fi)
{
	ExecutionState	**curStateUsed = NULL;
	unsigned	cond_idx_map[fi.N];

	for (unsigned i = 0; i < fi.N; i++)
		cond_idx_map[i] = i;

	if (preferFalseState) {
		/* swap true with false */
		cond_idx_map[0] = 1;
		cond_idx_map[1] = 0;
	} else if (preferTrueState) {
		/* nothing-- default prefers true */
	} else if (RandomizeFork) {
		for (unsigned i = 0; i < fi.N; i++) {
			unsigned	swap_idx, swap_val;

			swap_idx = theRNG.getInt32() % fi.N;
			swap_val = cond_idx_map[swap_idx];
			cond_idx_map[swap_idx] = cond_idx_map[i];
			cond_idx_map[i] = swap_val;
		}
	}

	for(unsigned int i = 0; i < fi.N; i++) {
		ExecutionState	*newState, *baseState;
		unsigned	condIndex;

		condIndex = cond_idx_map[i];

		// Process each valid target and generate a state
		if (!fi.res[condIndex]) continue;

		if (!curStateUsed) {
			/* reuse current state, when possible */
			fi.resStates[condIndex] = &current;
			curStateUsed = &fi.resStates[condIndex];
			continue;
		}

		assert (!fi.forkCompact || ReplayInhibitedForks);

		if (fi.forkDisabled) {
			fi.res[condIndex] = false;
			continue;
		}


		// Do actual state forking
		baseState = &current;
		newState = pureFork(current, fi.forkCompact);
		fi.forkedTargets++;
		fi.resStates[condIndex] = newState;

		// Split pathWriter stream
		if (!fi.isInternal) {
			TreeStreamWriter	*tsw;

			tsw = exe.getSymbolicPathWriter();
			if (tsw != NULL && newState != baseState) {
				newState->symPathOS = tsw->open(
					current.symPathOS);
			}
		}

	}

	if (fi.validTargets < 2 || fi.forkDisabled)
		return;

	/* Track forking condition transitions */
	for (unsigned i = 0; i < fi.N; i++) {
		ref<Expr>	new_cond;
		ExecutionState	*cur_st;

		if (!fi.res[i]) continue;

		cur_st = fi.resStates[i];
		new_cond = condFilter->apply(fi.conditions[i]);
		if (new_cond.isNull())
			continue;

		if (cur_st->prevForkCond.isNull() == false) {
			condXfer.insert(
				std::make_pair(
					cur_st->prevForkCond,
					new_cond));
			hasSucc.insert(cur_st->prevForkCond->hash());
		}

		cur_st->prevForkCond = new_cond;
	}
}

bool Forks::hasSuccessor(ExecutionState& st) const
{
	return hasSucc.count(st.prevForkCond->hash()) != 0;
}

/* XXX memoize? */
bool Forks::hasSuccessor(const ref<Expr>& cond) const
{
	ref<Expr>	e(condFilter->apply(cond));
	if (e.isNull())
		return true;
	return hasSucc.count(e->hash());
}


void Forks::constrainFork(
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

		constraint_added = exe.addConstraint(
			*curState, fi.conditions[condIndex]);
		if (constraint_added == false) {
			exe.terminateEarly(*curState, "branch contradiction");
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
		exe.terminateEarly(*curState, "max-depth exceeded");
		fi.resStates[condIndex] = NULL;
		return;
	}

	// Auxiliary bookkeeping
	if (!fi.isInternal) {
		if (exe.getSymbolicPathWriter() && fi.validTargets > 1) {
			std::stringstream ssPath;
			ssPath << condIndex << "\n";
			curState->symPathOS << ssPath.str();
		}

		// only track NON-internal branches
		if (!fi.wasReplayed)
			curState->trackBranch(condIndex, current.prevPC);
	}

	if (fi.isSeeding) {
		(exe.getSeedMap())[curState].insert(
			(exe.getSeedMap())[curState].end(),
			fi.resSeeds[condIndex].begin(),
			fi.resSeeds[condIndex].end());
	}
}

void Forks::constrainForks(ExecutionState& current, struct ForkInfo& fi)
{
	// Loop for bookkeeping
	// (loops must be separate since states are forked from each other)
	for (unsigned int condIndex = 0; condIndex < fi.N; condIndex++) {
		constrainFork(current, fi, condIndex);
	}
}

class MergeArrays : public ExprVisitor
{
public:
	MergeArrays(void) : ExprVisitor(false, true) { use_hashcons = false; }
	virtual ~MergeArrays(void) {}

	virtual ref<Expr> apply(const ref<Expr>& e)
	{
		ref<Expr>	ret;
		assert (!Expr::errors);

		ret = ExprVisitor::apply(e);

		/* must have changed a zero inside of a divide. oops */
		if (Expr::errors) {
			Expr::errors = 0;
			return ConstantExpr::create(0xff, 8);
		}

		return ret;
	}
protected:
	virtual Action visitConstant(const ConstantExpr& ce)
	{
		if (ce.getWidth() > 64)
			return Action::skipChildren();

		return Action::changeTo(
			ConstantExpr::create(
				ce.getZExtValue() & 0x800000000000001f,
				ce.getWidth()));
	}

	virtual Action visitRead(const ReadExpr& r)
	{
		mergearr_ty::iterator	it;
		ref<Array>		arr = r.getArray();
		ref<Array>		repl_arr;
		std::string		merge_name;
		const ConstantExpr	*ce_re_idx;
		unsigned		num_run, new_re_idx;

		for (num_run = 0; arr->name[num_run]; num_run++) {
			if (	arr->name[num_run] >= '0' &&
				arr->name[num_run] <= '9')
			{
				break;
			}
		}

		if (num_run == arr->name.size())
			return Action::skipChildren();

		merge_name = arr->name.substr(0, num_run);
		it = merge_arrs.find(merge_name);
		if (it != merge_arrs.end()) {
			repl_arr = it->second;
		} else {
			repl_arr = Array::create(merge_name, 1024);
			merge_arrs.insert(std::make_pair(merge_name, repl_arr));
		}

		new_re_idx = 0;
		ce_re_idx = dyn_cast<ConstantExpr>(r.index);
		if (ce_re_idx != NULL) {
			/* XXX: this loses information about structures
			 * since fields will alias but it is good for strings
			 * since it catches all idxmod-graphs */
			new_re_idx = ce_re_idx->getZExtValue() % ForkCondIdxMod;
		}

		return Action::changeTo(
			ReadExpr::create(
				UpdateList(repl_arr, NULL),
				ConstantExpr::create(new_re_idx, 32)));
	}
private:
	typedef std::map<std::string, ref<Array> > mergearr_ty;
	mergearr_ty	merge_arrs;
};

Forks::~Forks(void) { delete condFilter; }

Forks::Forks(Executor& _exe)
: exe(_exe)
, preferTrueState(false)
, preferFalseState(false)
, lastFork(0,0)
{
	condFilter = new ExprTimer<MergeArrays>(1000);
}

