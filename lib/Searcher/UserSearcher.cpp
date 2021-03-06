//===-- UserSearcher.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <llvm/Support/CommandLine.h>

#include "klee/Common.h"
#include "../Core/ExecutorBC.h"

#include "UserSearcher.h"
#include "CovSearcher.h"
#include "../Core/Searcher.h"

#include "SearchUpdater.h"
#include "DemotionSearcher.h"
#include "ConcretizingSearcher.h"
#include "SecondChanceSearcher.h"
#include "KillCovSearcher.h"
#include "RescanSearcher.h"
#include "TailPriority.h"
#include "BucketPriority.h"
#include "BatchingSearcher.h"
#include "CompleteSearcher.h"
#include "EpochSearcher.h"
#include "BFSSearcher.h"
#include "DFSSearcher.h"
#include "FilterSearcher.h"
#include "InterleavedSearcher.h"
#include "IterativeDeepeningTimeSearcher.h"
#include "PDFInterleavedSearcher.h"
#include "PhasedSearcher.h"
#include "StickySearcher.h"
#include "MergingSearcher.h"
#include "RandomPathSearcher.h"
#include "RRSearcher.h"
#include "RRPrSearcher.h"
#include "RandomSearcher.h"
#include "WeightedRandomSearcher.h"
#include "XChkSearcher.h"
#include "Weight2Prioritizer.h"
#include "PrioritySearcher.h"
#include "HistoPriority.h"
#include "OverrideSearcher.h"

using namespace llvm;
using namespace klee;

Prioritizer* UserSearcher::prFunc = NULL;
bool UserSearcher::useOverride = false;

bool UsePrioritySearcher;

#define DECL_SEARCH_OPT(x,y,z)	\
	namespace { \
	cl::opt<bool> Use##x##Search("use-" y "-search", cl::init(false)); \
	cl::opt<bool> UseInterleaved##x("use-interleaved-" z, cl::init(false)); }

DECL_SEARCH_OPT(FreshBranch, "fresh-branch", "fb");
DECL_SEARCH_OPT(Random, "random", "RS");
DECL_SEARCH_OPT(Constraint, "cons", "CONS");
DECL_SEARCH_OPT(MinConstraint, "mcons", "MCONS");
DECL_SEARCH_OPT(Bucket, "bucket", "BS");
DECL_SEARCH_OPT(Tail, "tail", "TS");
DECL_SEARCH_OPT(RR, "rr", "RR");
DECL_SEARCH_OPT(Markov, "markov", "MV");
DECL_SEARCH_OPT(NonUniformRandom, "non-uniform-random", "NURS");
DECL_SEARCH_OPT(MinInst, "mininst", "MI");
DECL_SEARCH_OPT(MaxInst, "maxinst", "MXI");
DECL_SEARCH_OPT(Trough, "trough", "TR");
DECL_SEARCH_OPT(FrontierTrough, "ftrough", "FTR");
DECL_SEARCH_OPT(CondSucc, "cond", "CD");
DECL_SEARCH_OPT(BranchIns, "brins", "BI");
DECL_SEARCH_OPT(UncommittedCov, "uccov", "UCCOV");
DECL_SEARCH_OPT(CovSetSize, "covsetsz", "COVSETSZ");
DECL_SEARCH_OPT(Uncov, "uncov", "UNC");
DECL_SEARCH_OPT(Stack, "stack", "STK");
DECL_SEARCH_OPT(StateInst, "stinst", "SI");
DECL_SEARCH_OPT(NewInst, "newinst", "NI");
DECL_SEARCH_OPT(UniqObj, "uniqobj", "UO");
DECL_SEARCH_OPT(BranchEntropy, "brentropy", "BE");

#define SEARCH_HISTO	new RescanSearcher(new HistoPrioritizer(executor))
DECL_SEARCH_OPT(Histo, "histo", "HS");

namespace {
  cl::opt<bool>
  UseFilterSearch(
  	"use-search-filter",
	cl::desc("Filter out unwanted functions from dispatch"),
	cl::init(false));

  cl::opt<unsigned> UseConcretizingSearch("use-concretizing-search");
  cl::opt<bool> UseDemotionSearch("use-demotion-search");
  cl::opt<bool> UseBreadthFirst("use-breadth-first");
  cl::opt<bool> UsePhasedSearch("use-phased-search");
  cl::opt<bool> UseRandomPathSearch("use-random-path");
  cl::opt<bool> UseStickySearch("use-sticky-search");

  cl::opt<bool> UseInterleavedTailRS("use-interleaved-TRS");
  cl::opt<bool> UseInterleavedDFS("use-interleaved-DFS");
  cl::opt<bool> UseInterleavedMD2UNURS("use-interleaved-MD2U-NURS");
  cl::opt<bool> UseInterleavedPerInstCountNURS("use-interleaved-icnt-NURS");
  cl::opt<bool> UseInterleavedCPInstCountNURS("use-interleaved-cpicnt-NURS");
  cl::opt<bool> UseInterleavedQueryCostNURS("use-interleaved-query-cost-NURS");
  cl::opt<bool> UseInterleavedCovNewNURS("use-interleaved-covnew-NURS");

  cl::opt<bool> DumpSelectStack("dump-select-stack", cl::init(false));

  cl::opt<bool> UseSecondChance(
  	"use-second-chance",
	cl::desc("Give states that find new instructions extra time."),
	cl::init(false));

  cl::opt<bool> UseKillCov(
  	"use-killcov",
	cl::desc("Kill forked states if no new coverage."),
	cl::init(false));


  cl::opt<std::string>
  WeightType(
    "weight-type",
    cl::desc(
    	"Set the weight type for --use-non-uniform-random-search.\n"
	"Weights: none, icnt, cpicnt, query-cost, md2u, covnew, markov"),
    cl::init("none"));
#if 0
      clEnumVal("none", "use (2^depth)"),
      clEnumVal("icnt", "use current pc exec count"),
      clEnumVal("cpicnt", "use current pc exec count"),
      clEnumVal("query-cost", "use query cost"),
      clEnumVal("md2u", "use min dist to uncovered"),
      clEnumVal("covnew", "use min dist to uncovered + coveringNew flag"),
      clEnumValEnd));
#endif

  cl::opt<bool>
  UseMerge("use-merge",
           cl::desc("Enable support for klee_merge() (experimental)"));

  cl::opt<bool>
  UseIterativeDeepeningTimeSearch("use-iterative-deepening-time-search",
                                    cl::desc("(experimental)"));

  cl::opt<bool>
  UseBatchingSearch("use-batching-search",
           cl::desc("Batching searches by instructions and time"));

  cl::opt<bool>
  UseEpochSearch(
  	"use-epoch-search",
	cl::desc("Treat older/run states with respect."),
	cl::init(false));

  cl::opt<bool>
  UseXChkSearcher(
    "xchk-searcher",
    cl::desc("On reschedule, reaffirm validate address space is same as before"),
    cl::init(false));

  cl::opt<bool>
  UseCovSearcher(
  	"use-cov-search",
	cl::desc("Greedily execute uncovered instructions"),
	cl::init(false));

  cl::opt<bool>
  UsePDFInterleave(
  	"use-pdf-interleave",
	cl::desc("Use uncovered instruction PDF interleaver"),
	cl::init(false));

  cl::opt<bool, true>
  UsePrioritySearcherProxy(
  	"priority-search",
	cl::location(UsePrioritySearcher),
	cl::desc("Search with generic priority searcher"),
	cl::init(false));
}

static WeightFunc* getWeightFuncByName(const std::string& name)
{
	if (name == "none")
		return new DepthWeight();
	else if (name == "icnt")
		return new PerInstCountWeight();
	else if (name == "cpicnt")
		return new CPInstCountWeight();
	else if (name == "query-cost")
		return new QueryCostWeight();
	else if (name == "md2u")
		return new MinDistToUncoveredWeight();
	else if (name == "covnew")
		return new CoveringNewWeight();
	else if (name =="markov")
		return new MarkovPathWeight();


	assert (0 == 1 && "Unknown weight type given");
	return NULL;
}

bool UserSearcher::userSearcherRequiresMD2U() {
  return (WeightType=="md2u" || WeightType=="covnew" ||
          UseInterleavedMD2UNURS ||
          UseInterleavedCovNewNURS ||
          UseInterleavedPerInstCountNURS ||
          UseInterleavedCPInstCountNURS ||
          UseInterleavedQueryCostNURS);
}

#define PUSH_SEARCHER_IF_SET(x, y)	if (x) s.push_back(y)
#define PUSH_ILEAV_IF_SET(x,y)	PUSH_SEARCHER_IF_SET(UseInterleaved##x, y)

#define DEFAULT_PR_SEARCHER	new RandomSearcher()
#define TAIL_RESCAN_SEARCHER	\
	new RescanSearcher(new Weight2Prioritizer<TailWeight>(1.0))

#define UNCOMMITTEDCOV_SEARCHER						\
	new RRPrSearcher(						\
		new Weight2Prioritizer<UncommittedCoverageWeight>(	\
			new UncommittedCoverageWeight(),		\
			1.0),						\
		0)

#define COVSETSIZE_SEARCHER					\
	new RRPrSearcher(					\
		new Weight2Prioritizer<CovSetSizeWeight>(	\
			new CovSetSizeWeight(),	1.0))
#define FRESH_BRANCH_SEARCHER	\
	new RRPrSearcher(new Weight2Prioritizer<FreshBranchWeight>(1), 0)
#define BRANCHINS_SEARCHER	\
	new WeightedRandomSearcher(executor, new BranchWeight(&executor))
#define UNCOV_SEARCHER	\
	new WeightedRandomSearcher(executor, new UncovWeight())
#define STACK_SEARCHER	\
	new RescanSearcher(	\
		new Weight2Prioritizer<StackWeight>(	\
				new StackWeight(), 1.0))
#define STINST_SEARCHER	\
	new RescanSearcher(	\
		new Weight2Prioritizer<StateInstWeight>(	\
			new StateInstWeight(), 1.0))

#if 1
#define NEWINST_SEARCHER	\
	new RescanSearcher(	\
		new Weight2Prioritizer<BinaryWeight>(	\
			new BinaryWeight(new NewInstsWeight(), 1.0)))
#else
#define  NEWINST_SEARCHER	\
	new WeightedRandomSearcher(executor, new NewInstsWeight())
#endif

#define UNIQOBJ_SEARCHER	\
	new RescanSearcher(new Weight2Prioritizer<UniqObjWeight>(	\
		new UniqObjWeight(), 1.0))

#define	BRENTROPY_SEARCHER	\
	new RescanSearcher(new Weight2Prioritizer<BranchEntropyWeight>(	\
		new BranchEntropyWeight(), 10000.0))

#define CONDSUCC_SEARCHER	\
	new RRPrSearcher(	\
		new Weight2Prioritizer<CondSuccWeight>(	\
			new CondSuccWeight(&executor),	\
			1.0),				\
		0)

/* Research quality */
Searcher* UserSearcher::setupInterleavedSearcher(
	Executor& executor, Searcher* searcher)
{
	std::vector<Searcher *> s;

	s.push_back(searcher);
	PUSH_ILEAV_IF_SET(FreshBranch, FRESH_BRANCH_SEARCHER);

	PUSH_ILEAV_IF_SET(Histo, SEARCH_HISTO);

	PUSH_ILEAV_IF_SET(BranchIns, BRANCHINS_SEARCHER);

	PUSH_ILEAV_IF_SET(UncommittedCov, UNCOMMITTEDCOV_SEARCHER);

	PUSH_ILEAV_IF_SET(CovSetSize, COVSETSIZE_SEARCHER);

	PUSH_ILEAV_IF_SET(Uncov, UNCOV_SEARCHER);

	PUSH_ILEAV_IF_SET(StateInst, STINST_SEARCHER);

	PUSH_ILEAV_IF_SET(CondSucc, CONDSUCC_SEARCHER);

	PUSH_ILEAV_IF_SET(Stack, STACK_SEARCHER);

	PUSH_ILEAV_IF_SET(
		NonUniformRandom,
		new WeightedRandomSearcher(executor, new DepthWeight()));

	PUSH_ILEAV_IF_SET(
		MinInst,
		new RescanSearcher(
			new Weight2Prioritizer<StateInstCountWeight>(-1.0)));

	PUSH_ILEAV_IF_SET(
		MaxInst,
		new RescanSearcher(
			new Weight2Prioritizer<StateInstCountWeight>(1.0)));

	PUSH_ILEAV_IF_SET(DFS, new DFSSearcher());
	PUSH_ILEAV_IF_SET(RR, new RRSearcher());

	PUSH_ILEAV_IF_SET(NewInst, NEWINST_SEARCHER);

	PUSH_ILEAV_IF_SET(UniqObj, UNIQOBJ_SEARCHER);
	
	PUSH_ILEAV_IF_SET(BranchEntropy, BRENTROPY_SEARCHER);

	PUSH_ILEAV_IF_SET(Trough, new RescanSearcher(
		new Weight2Prioritizer<TroughWeight>(
			new TroughWeight(&executor), -1.0)));

	PUSH_ILEAV_IF_SET(FrontierTrough, new RescanSearcher(
		new Weight2Prioritizer<FrontierTroughWeight>(
			new FrontierTroughWeight(&executor), -1.0)));


	PUSH_ILEAV_IF_SET(
		Constraint,
		new RescanSearcher(
			new Weight2Prioritizer<ConstraintWeight>(1.0)));

	PUSH_ILEAV_IF_SET(
		MinConstraint,
		new RescanSearcher(
			new Weight2Prioritizer<ConstraintWeight>(-1.0)));


	PUSH_ILEAV_IF_SET(TailRS, TAIL_RESCAN_SEARCHER);

	PUSH_ILEAV_IF_SET(
		Markov,
		new PrioritySearcher(
			new Weight2Prioritizer<MarkovPathWeight>(100),
			DEFAULT_PR_SEARCHER));

	PUSH_ILEAV_IF_SET(
		Bucket,
		new PrioritySearcher(
			new BucketPrioritizer(), DEFAULT_PR_SEARCHER));

	PUSH_ILEAV_IF_SET(
		Tail,
		new PrioritySearcher(
			new TailPrioritizer(), DEFAULT_PR_SEARCHER));

	PUSH_ILEAV_IF_SET(
		MD2UNURS,
		new WeightedRandomSearcher(
			executor, new MinDistToUncoveredWeight()));

	PUSH_ILEAV_IF_SET(
		CovNewNURS,
		new WeightedRandomSearcher(executor, new CoveringNewWeight()));

	PUSH_ILEAV_IF_SET(
		PerInstCountNURS,
		new WeightedRandomSearcher(
			executor, new PerInstCountWeight()));

	PUSH_ILEAV_IF_SET(
		CPInstCountNURS,
		new WeightedRandomSearcher(executor, new CPInstCountWeight()));

	PUSH_ILEAV_IF_SET(
		QueryCostNURS,
		new WeightedRandomSearcher(executor, new QueryCostWeight()));

	PUSH_ILEAV_IF_SET(Random, new RandomSearcher());

	if (s.size() != 1) {
		if (UsePDFInterleave)
			return new PDFInterleavedSearcher(s);
		else
			return new InterleavedSearcher(s);
	}

	/* No interleaved searchers defined. Don't bother with interleave obj */
	return searcher;
}

Searcher* UserSearcher::setupBaseSearcher(Executor& executor)
{
	Searcher* searcher;

	if (UseFreshBranchSearch) {
		searcher = FRESH_BRANCH_SEARCHER;
	} else if (UseBranchInsSearch) {
		searcher = BRANCHINS_SEARCHER;
	} else if (UseUncovSearch) {
		searcher = UNCOV_SEARCHER;
	} else if (UseCondSuccSearch) {
		searcher = CONDSUCC_SEARCHER;
	} else if (UseStateInstSearch) {
		searcher = STINST_SEARCHER;
	} else if (UseHistoSearch) {
		searcher = SEARCH_HISTO;
	} else if (UseMarkovSearch) {
		searcher = new RescanSearcher(
			new Weight2Prioritizer<MarkovPathWeight>(100));
	} else if (UseMaxInstSearch) {
		searcher = new RescanSearcher(
			new Weight2Prioritizer<StateInstCountWeight>(1.0));
	} else if (UseMinInstSearch) {
		searcher = new RescanSearcher(
			new Weight2Prioritizer<StateInstCountWeight>(-1.0));
	} else if (UseConstraintSearch) {
		searcher =new RescanSearcher(
			new Weight2Prioritizer<ConstraintWeight>(1.0));
	} else if (UseTailSearch) {
		searcher = new PrioritySearcher(
			new TailPrioritizer(), DEFAULT_PR_SEARCHER);
	} else if (UseBucketSearch) {
		searcher = new PrioritySearcher(
			new BucketPrioritizer(), DEFAULT_PR_SEARCHER);
	} else if (UseStackSearch) {
		searcher = STACK_SEARCHER;
	} else if (UseCovSearcher) {
		searcher = new PrioritySearcher(
			new CovPrioritizer(executor.getKModule()),
			DEFAULT_PR_SEARCHER);
	} else if (UsePrioritySearcher) {
		assert (prFunc != NULL);
		searcher = new PrioritySearcher(
			prFunc,
//			new PrioritySearcher(
//				new BucketPrioritizer(),
//				DEFAULT_PR_SEARCHER));
			DEFAULT_PR_SEARCHER);
		prFunc = NULL;
	} else if (UsePhasedSearch) {
		searcher = new PhasedSearcher();
	} else if (UseRRSearch) {
		searcher = new RRSearcher();
	} else if (UseTroughSearch) {
		searcher = new RescanSearcher(
			new Weight2Prioritizer<TroughWeight>(
				new TroughWeight(&executor),
				-1.0));
 	} else if (UseTroughSearch) {
		searcher = new RescanSearcher(
			new Weight2Prioritizer<FrontierTroughWeight>(
				new FrontierTroughWeight(&executor),
				-1.0));
	} else if (UseRandomPathSearch) {
		searcher = new RandomPathSearcher(executor);
	} else if (UseNonUniformRandomSearch) {
		searcher = new WeightedRandomSearcher(
			executor,
			getWeightFuncByName(WeightType));
	} else if (UseRandomSearch) {
		searcher = new RandomSearcher();
	} else if (UseBreadthFirst) {
		searcher = new BFSSearcher();
	} else {
		searcher = new DFSSearcher();
	}

	return searcher;
}

class StackDumpUpdater : public UpdateAction
{
public:
	StackDumpUpdater(Executor& _exe) : exe(_exe) {}
	virtual ~StackDumpUpdater() {}
	virtual UpdateAction* copy(void) const { return new StackDumpUpdater(exe); }
	virtual void selectUpdate(ExecutionState* es)
	{
		std::cerr << "StackTrace for st="
			<< (void*)es
			<< ". Insts=" <<es->totalInsts
			<< ". SInsts=" << es->personalInsts
			<< ". NewInsts=" << es->newInsts
			<< ". Constrs=" << es->constraints.size()
			<< '\n';
		exe.printStackTrace(*es, std::cerr);
		std::cerr << "===================\n";
	}
private:
	Executor	&exe;
};

Searcher* UserSearcher::setupConfigSearcher(Executor& executor)
{
	Searcher	*searcher;

	searcher = setupBaseSearcher(executor);
	searcher = setupInterleavedSearcher(executor, searcher);

	/* xchk searcher should probably always be at the top */
	if (UseXChkSearcher)
		searcher = new XChkSearcher(searcher);

	if (UseDemotionSearch)
		searcher = new DemotionSearcher(searcher);

	if (UseFilterSearch)
		searcher = new FilterSearcher(executor, searcher);

	if (UseEpochSearch)
		searcher = new EpochSearcher(
			executor, new RRSearcher(), searcher);

	if (UseConcretizingSearch == 1)
		searcher = new ConcretizingSearcher(executor, searcher);

	if (UseSecondChance)
		searcher = new SecondChanceSearcher(searcher);

	if (UseKillCov)
		searcher = new KillCovSearcher(executor, searcher);

	if (UseConcretizingSearch == 2)
		searcher = new ConcretizingSearcher(executor, searcher);

	if (UseStickySearch)
		searcher = new StickySearcher(searcher);

	if (DumpSelectStack) {
		searcher = new SearchUpdater(
			searcher, new StackDumpUpdater(executor));
	}

	if (UseBatchingSearch)
		searcher = new BatchingSearcher(
			new CompleteSearcher(
				searcher,
				new RRSearcher()));

	if (UseMerge)
		searcher = new MergingSearcher(
			static_cast<ExecutorBC&>(executor), searcher);

	if (UseIterativeDeepeningTimeSearch)
		searcher = new IterativeDeepeningTimeSearcher(searcher);

	return searcher;
}

usearcher_t UserSearcher::constructUserSearcher(Executor &exe)
{
	Searcher *searcher;

	searcher = setupConfigSearcher(exe);

	if (useOverride)
		searcher = new OverrideSearcher(searcher);

	std::ostream &os = exe.getHandler().getInfoStream();

	os << "BEGIN searcher description\n";
	searcher->printName(os);
	os << "\nEND searcher description\n";
	os.flush();

	if (userSearcherRequiresMD2U())
		exe.getStatsTracker()->setUpdateMinDist();

	return usearcher_t(searcher);
}
