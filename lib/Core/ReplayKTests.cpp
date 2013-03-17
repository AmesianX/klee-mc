#include <llvm/Support/CommandLine.h>
#include "ForksKTest.h"
#include "Executor.h"
#include "ExeStateManager.h"
#include "PTree.h"
#include "static/Sugar.h"
#include <algorithm>

using namespace klee;

extern llvm::cl::opt<bool> FasterReplay;
namespace
{
	llvm::cl::opt<bool>
		ReplayKTestSort("replay-ktest-sort",
		llvm::cl::init(true));
}

bool ReplayKTests::replay(Executor* exe, ExecutionState* initSt)
{
	Forks		*old_f;
	bool		ret;

	std::cerr << "[KTest] Replaying " << kts.size() << " ktests.\n";

	old_f = exe->getForking();
	ret = (FasterReplay)
		? replayFast(exe, initSt)
		: replaySlow(exe, initSt);
	exe->setForking(old_f);

	std::cerr << "[KTest] All replays complete.\n";

	return ret;
}

#include "klee/Internal/ADT/KTest.h"

struct kts_sort {
bool operator()(const KTest* k1, const KTest* k2)
{
	int		diff;
	unsigned	min_v;

	min_v = (k2->numObjects > k1->numObjects)
		? k1->numObjects
		: k2->numObjects;

	for (unsigned i = 0; i < min_v; i++) {
		diff = strcmp(k1->objects[i].name, k2->objects[i].name);
		if (diff) return (diff < 0);
		diff = k2->objects[i].numBytes - k1->objects[i].numBytes;
		if (diff) return (diff < 0);
		diff = memcmp(
			k1->objects[i].bytes,
			k2->objects[i].bytes,
			k1->objects[i].numBytes);
		if (diff) return (diff < 0);
	}

//	if ((diff = (k2->numObjects - k1->numObjects)) != 0)
//		return (diff > 0);

	return false;
}};

bool ReplayKTests::replayFast(Executor* exe, ExecutionState* initSt)
{
	ForksKTestStateLogger	*f_ktest = new ForksKTestStateLogger(*exe);

	exe->setForking(f_ktest);

	if (ReplayKTestSort) {
		std::vector<KTest*>	kts_s;
		kts_s = kts;
		std::sort (kts_s.begin(), kts_s.end(), kts_sort());
		replayFast(exe, initSt, kts_s);
	} else
		replayFast(exe, initSt, kts);

	delete f_ktest;

	return true;
}

void ReplayKTests::replayFast(
	Executor* exe,
	ExecutionState* initSt,
	const std::vector<KTest*>& in_kts)
{
	ExeStateManager		*esm = exe->getStateManager();
	ForksKTestStateLogger	*f_ktest;

	f_ktest = (ForksKTestStateLogger*)exe->getForking();
	foreach (it, in_kts.begin(), in_kts.end()) {
		ExecutionState	*es;
		const KTest	*ktest(*it);

		es = f_ktest->getNearState(ktest);
		if (es == NULL) {
			std::cerr << "[ReplayKTest] No near state.\n";
			es = initSt->copy();
			es->ptreeNode->markReplay();
			esm->queueSplitAdd(es->ptreeNode, initSt, es);
		} else {
			std::cerr
				<< "[ReplayKTest] Got near state with "
				<< es->getNumSymbolics()
				<< " objects.\n";
		}

		f_ktest->setKTest(ktest);
		exe->exhaustState(es);

		std::cerr
			<< "[Replay] Replay KTest done st="
			<< es << ". Total="
			<< esm->numRunningStates() << "\n";
	}
}

bool ReplayKTests::replaySlow(Executor* exe, ExecutionState* initSt)
{
	ExeStateManager	*esm = exe->getStateManager();
	ForksKTest	*f_ktest = new ForksKTest(*exe);

	exe->setForking(f_ktest);
	foreach (it, kts.begin(), kts.end()) {
		ExecutionState	*es;
		const KTest	*ktest(*it);

		es = initSt->copy();
		es->ptreeNode->markReplay();
		esm->queueSplitAdd(es->ptreeNode, initSt, es);
		f_ktest->setKTest(ktest);
		exe->exhaustState(es);

		std::cerr
			<< "[Replay] Replay KTest done st="
			<< es << ". Total="
			<< esm->numRunningStates() << "\n";

	}

	delete f_ktest;

	return true;
}