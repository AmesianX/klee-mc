/*
 * Catches queries that killed us in the past so we don't try them again.
 *
 * 1. Hook SIGSEGV to catch crashes in solver
 * 2. Load cache into memory for quick access.
 * -  Check for query in cache before submitting to solver
 *    - if solver crashes, save query to cache, terminate
 *    - if solver does not crash, good times
 *
 * */
#ifndef POISON_CACHE_H
#define POISON_CACHE_H

#include "klee/Solver.h"
#include "QueryHash.h"
#include "SolverImplWrapper.h"

#include <set>
#include <signal.h>

#define POISON_DEFAULT_PATH	"poison.cache"

namespace klee
{

class PoisonCache : public SolverImplWrapper
{
public:
	PoisonCache(Solver* s, QueryHash* phash);
	virtual ~PoisonCache();

	static void sig_poison(int signum, siginfo_t*, void*);
	static void sigpoison_save(void);
private:
	bool			in_solver;
	unsigned		hash_last;
	std::set<unsigned>	poison_hashes;
	QueryHash		*phash;

	bool badQuery(const Query& q);
	void loadCacheFromDisk(const char* fname = POISON_DEFAULT_PATH);
public:

	bool computeSat(const Query&);
	Solver::Validity computeValidity(const Query&);
	ref<Expr> computeValue(const Query&);
	bool computeInitialValues(const Query&, Assignment& a);

	void printName(int level = 0) const {
		klee_message(
			((std::string("%*s PoisonCache (")+
				phash->getName())+
				") containing: ").c_str(),
			2*level,  "");
		wrappedSolver->printName(level + 1);
	}
};
}
#endif
