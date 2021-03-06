//===-- Constraints.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CONSTRAINTS_H
#define KLEE_CONSTRAINTS_H

#include "klee/Expr.h"
#include "klee/util/ExprTimer.h"
#include "klee/IndependentElementSet.h"

// FIXME: Currently we use ConstraintManager for two things: to pass
// sets of constraints around, and to optimize constraints. We should
// move the first usage into a separate data structure
// (ConstraintSet?) which ConstraintManager could embed if it likes.
namespace klee {

class ExprVisitor;
class ExprReplaceVisitor2;
class Assignment;

class ConstraintManager
{
public:
	typedef std::vector< ref<Expr> > constraints_t;
	// maps 1:1 to constraints_t
	typedef std::vector< ref<ReadSet> > readsets_t;

	typedef constraints_t::iterator iterator;
	typedef constraints_t::const_iterator const_iterator;
	typedef std::vector< ref<Expr> >::const_iterator constraint_iterator;



	ConstraintManager() : simplifier(NULL) {}
	virtual ~ConstraintManager();

	// create from constraints with no optimization
	explicit
	ConstraintManager(const std::vector< ref<Expr> > &_constraints);

	ConstraintManager(
		const constraints_t &_constraints,
		const readsets_t &readsets);


	ConstraintManager(const ConstraintManager &cs)
	: constraints(cs.constraints)
	, readsets(cs.readsets)
	, simplifier(NULL) {}

	ConstraintManager& operator=(const ConstraintManager &cs)
	{
		if (&cs == this)
			return *this;

		constraints = cs.constraints;
		readsets = cs.readsets;
		invalidateSimplifier();
		return *this;
	}

	ref<Expr> simplifyExpr(ref<Expr> e) const;

	bool addConstraint(ref<Expr> e);

	bool empty() const { return constraints.empty(); }
	ref<Expr> back() const { return constraints.back(); }
	constraint_iterator begin() const { return constraints.begin(); }
	constraint_iterator end() const { return constraints.end(); }
	size_t size() const { return constraints.size(); }

	/* this interface forces an array, but doing it with iterators
	 * means changing constraitns_ty to include readsets which 
	 * bakes readsets into the implementation more than I'd like */
	ref<ReadSet> getReadset(unsigned i) const { return readsets[i]; }
	ref<Expr> getConstraint(unsigned i) const { return constraints[i]; }
	ref<Expr> getConjunction(void) const;

	bool operator==(const ConstraintManager &other) const
	{ return constraints == other.constraints; }

	void print(std::ostream& os) const;

	bool isValid(const Assignment& a) const;

	/* return false if not a valid assignment */
	bool apply(const Assignment& a);

	static unsigned getReplacements(void) { return simplify_c; }
	static unsigned getTimeouts(void) { return timeout_c; }
	static void incReplacements(void) { simplify_c++; }
	ConstraintManager operator -(const ConstraintManager& other) const;
private:
	constraints_t constraints;
	readsets_t readsets;
	mutable ExprTimer<ExprReplaceVisitor2>* simplifier;

	// returns true iff the constraints were modified
	bool rewriteConstraints(ExprVisitor &visitor);

	bool addConstraintInternal(ref<Expr> e);
	void invalidateSimplifier(void) const;
	void setupSimplifier(void) const;

	static unsigned simplify_c;	/* number of simplify calls */
	static unsigned simplified_c;	/* number of expressions simplified */
	static unsigned timeout_c;
};

}

#endif /* KLEE_CONSTRAINTS_H */
