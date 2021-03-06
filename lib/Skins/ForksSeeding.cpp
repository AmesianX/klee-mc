#include <llvm/Support/CommandLine.h>
#include "klee/Internal/ADT/RNG.h"
#include "../Core/StateSolver.h"
#include "ForksSeeding.h"

using namespace klee;
using namespace llvm;

namespace klee { extern RNG theRNG; }

bool ForksSeeding::forkSetup(ExecutionState& current, struct ForkInfo& fi)
{

	SeedMapType		&seedMap(exe.getSeedMap());
	SeedMapType::iterator	it(seedMap.find(&current));

	fi.isSeeding = exe.isStateSeeding(&current);
	assert (fi.isSeeding);

	assert (it != seedMap.end());

	// Fix branch in only-replay-seed mode, if we don't have both true
	// and false seeds.

	// Assume each seed only satisfies one condition (necessarily true
	// when conditions are mutually exclusive and their conjunction is
	// a tautology).
	// This partitions the seed set for the current state
	for (auto const &seed : it->second) {
		unsigned i;

		assert (fi.size());
		for (i = 0; i < fi.size(); ++i) {
			ref<ConstantExpr>	seedCondRes;
			bool			ok;
			ok = exe.getSolver()->getValue(
				current,
				seed.assignment.evaluate(fi.conditions[i]),
				seedCondRes);
			assert(ok && "FIXME: Unhandled solver failure");
			if (seedCondRes->isTrue())
				break;
		}

		// If we didn't find a satisfying condition, randomly pick one
		// (the seed will be patched).
		if (i == fi.size()) i = theRNG.getInt32() % fi.size();

		fi.getResSeeds()[i].push_back(seed);
	}

	// Clear any valid conditions at seeding rejects
	if ((fi.forkDisabled || Replay::isReplayOnly()) && fi.validTargets > 1) {
		fi.validTargets = 0;
		for (unsigned i = 0; i < fi.size(); i++) {
			if (fi.getResSeeds()[i].empty()) fi.res[i] = false;
			if (fi.res[i]) fi.validTargets++;
		}
		assert (fi.validTargets &&
			"seed must result in at least one valid target");
	}

	// Remove seeds corresponding to current state
	seedMap.erase(it);

	// !!! it's possible for the current state to end up with no seeds. Does
	// this matter? Old fork() used to handle it but branch() didn't.
	//
	return true;
}

bool ForksSeeding::evalForkBranch(ExecutionState& s, struct ForkInfo& fi)
{
	fi.isSeeding = exe.isStateSeeding(&s);
	assert (fi.isSeeding);

	if (Forks::evalForkBranch(s, fi) == false)
		return false;

	/* branch on both true and false conditions */
	fi.conditions[0] = Expr::createIsZero(fi.conditions[1]);
	return true;
}

bool ForksSeeding::constrainFork(
	ExecutionState& current,
	struct ForkInfo& fi,
	unsigned int condIndex)
{
	ExecutionState	*curState;

	if (Forks::constrainFork(current, fi, condIndex) == false)
		return false;

	curState = fi.resStates[condIndex];
	assert(curState);

	(exe.getSeedMap())[curState].insert(
		(exe.getSeedMap())[curState].end(),
		fi.getResSeeds()[condIndex].begin(),
		fi.getResSeeds()[condIndex].end());

	return true;
}
