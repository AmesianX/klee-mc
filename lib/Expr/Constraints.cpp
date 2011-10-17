//===-- Constraints.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "llvm/Support/CommandLine.h"
#include "klee/Constraints.h"
#include "klee/Common.h"

#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprVisitor.h"
#include "static/Sugar.h"

#include <stack>
#include <string.h>
#include <iostream>
#include <list>
#include <map>

using namespace klee;

namespace {
	llvm::cl::opt<bool>
	SimplifyUpdates(
		"simplify-updates",
		llvm::cl::desc(
		"Simplifies update list expressions in constraint manager."),
		llvm::cl::init(true));
}

namespace klee {

class ExprReplaceVisitor : public ExprVisitor
{
private:
	ref<Expr> src, dst;

public:
	ExprReplaceVisitor(ref<Expr> _src, ref<Expr> _dst)
	: src(_src), dst(_dst) {}

	Action visitExpr(const Expr &e) {
		return (e == *src.get())
			? Action::changeTo(dst)
			: Action::doChildren();
	}

	Action visitExprPost(const Expr &e) {
		return (e == *src.get())
			? Action::changeTo(dst)
			: Action::doChildren();
	}
};

class ExprReplaceVisitor2 : public ExprVisitor
{
public:
	ExprReplaceVisitor2(void) : ExprVisitor(true) {}

	ExprReplaceVisitor2(
		const std::map< ref<Expr>, ref<Expr> >& _replacements)
	: ExprVisitor(true)
	, replacements(_replacements) {}

	Action visitExprPost(const Expr &e)
	{
		std::map< ref<Expr>, ref<Expr> >::const_iterator it;
		it = replacements.find(ref<Expr>(const_cast<Expr*>(&e)));
		return (it != replacements.end())
			? Action::changeTo(it->second)
			: Action::doChildren();
	}

	std::map< ref<Expr>, ref<Expr> >& getReplacements(void)
	{
		return replacements;
	}

protected:
	virtual Action visitRead(const ReadExpr &re);

private:
	std::map< ref<Expr>, ref<Expr> > replacements;
};

}

ExprVisitor::Action ExprReplaceVisitor2::visitRead(const ReadExpr &re)
{
	if (!SimplifyUpdates)
		return Action::doChildren();

	std::stack<std::pair<ref<Expr>, ref<Expr> > > updateStack;
	ref<Expr>		uniformValue(0);
	bool			rebuild, rebuildUpdates;
	const UpdateList	&ul(re.updates);

	// fast path: no updates, reading from constant array
	// with a single value occupying all indices in the array
	if (!ul.head && ul.root->isSingleValue())
		return Action::changeTo(ul.root->getValue(0));

	ref<Expr> readIndex = isa<ConstantExpr>(re.index)
		? re.index
		: visit(re.index);

	// simplify case of a known read from a constant array
	if (	isa<ConstantExpr>(readIndex) &&
		!ul.head && ul.root->isConstantArray())
	{
		uint64_t idx = cast<ConstantExpr>(readIndex)->getZExtValue();
		if (idx < ul.root->mallocKey.size)
			return Action::changeTo(ul.root->getValue(idx));

		klee_warning_once(
			0,
			"out of bounds constant array read (possibly within "
			"an infeasible Select path?)");

		return Action::changeTo(ConstantExpr::alloc(0, re.getWidth()));
	}

	rebuild = rebuildUpdates = false;
	if (readIndex != re.index)
		rebuild = true;

	uniformValue = buildUpdateStack(
		re.updates, readIndex, updateStack, rebuildUpdates);
	if (!uniformValue.isNull())
		return Action::changeTo(uniformValue);

	// at least one update was simplified? rebuild
	if (rebuild || rebuildUpdates) {
		UpdateList	*newUpdates;
		ref<Expr>	new_re;

		if (!rebuildUpdates)
			return Action::changeTo(
				ReadExpr::create(re.updates, readIndex));

	
		newUpdates = UpdateList::fromUpdateStack(
			re.updates.root, updateStack);

		new_re = ReadExpr::create(*newUpdates, readIndex);
		delete newUpdates;

		return Action::changeTo(new_re);
	}


	return Action::doChildren();
}

bool ConstraintManager::rewriteConstraints(ExprVisitor &visitor)
{
	ConstraintManager::constraints_ty old;
	bool changed = false;

	constraints.swap(old);
	invalidateSimplifier();

	foreach (it, old.begin(), old.end()) {
		ref<Expr> &ce = *it;
		ref<Expr> e = visitor.visit(ce);

		if (e != ce) {
			// enable further reductions
			addConstraintInternal(e);
			changed = true;
			continue;
		}

		if (changed)
			e = simplifyExpr(ce);
		constraints.push_back(ce);
		invalidateSimplifier();
	}

	return changed;
}

void ConstraintManager::invalidateSimplifier(void) const
{
	if (!simplifier) return;
	delete simplifier;
	simplifier = NULL;
}

void ConstraintManager::simplifyForValidConstraint(ref<Expr> e)
{
	assert (0 == 1 && "STUB");
}

#define TRUE_EXPR	ConstantExpr::alloc(1, Expr::Bool)
#define FALSE_EXPR	ConstantExpr::alloc(0, Expr::Bool)

static void addEquality(
	std::map< ref<Expr>, ref<Expr> > &equalities,
	const ref<Expr>& e)
{
	if (const EqExpr *ee = dyn_cast<EqExpr>(e)) {
		if (isa<ConstantExpr>(ee->left)) {
			equalities.insert(std::make_pair(ee->right, ee->left));
		} else {
			equalities.insert(std::make_pair(e, TRUE_EXPR));
		}
		return;
	}

	equalities.insert(std::make_pair(e, TRUE_EXPR));

	// DAR: common simplifications that make referent tracking on symbolics
	// more efficient. Collapses the constraints created by
	// compareOpReferents into simpler constraints. This is needed because
	// expression canonicalization turns everything into < or <=
	if (const UltExpr *x = dyn_cast<UltExpr>(e)) {
		equalities.insert(
			std::make_pair(
				UleExpr::create(x->getKid(1), x->getKid(0)),
				FALSE_EXPR));
		return;
	}
	
	if (const UleExpr *x = dyn_cast<UleExpr>(e)) {
		equalities.insert(
			std::make_pair(
				UltExpr::create(x->getKid(1), x->getKid(0)),
				FALSE_EXPR));

		// x <= 0 implies x == 0
		if (x->getKid(1)->isZero())
			equalities.insert(
				std::make_pair(x->getKid(0), x->getKid(1)));

		return;
	}
}

ref<Expr> ConstraintManager::simplifyExpr(ref<Expr> e) const
{
	if (isa<ConstantExpr>(e)) return e;

	if (simplifier != NULL)
		return simplifier->visit(e);

	simplifier = new ExprReplaceVisitor2();
	std::map< ref<Expr>, ref<Expr> >& equalities(
		simplifier->getReplacements());

	foreach (it, constraints.begin(), constraints.end()) {
		addEquality(equalities, *it);
	}

	return simplifier->visit(e);
}

bool ConstraintManager::addConstraintInternal(ref<Expr> e)
{
	// rewrite any known equalities, return false if
	// we find ourselves with a contradiction. This means that
	// the constraint we're adding can't happen!

	// XXX should profile the effects of this and the overhead.
	// traversing the constraints looking for equalities is hardly the
	// slowest thing we do, but it is probably nicer to have a
	// ConstraintSet ADT which efficiently remembers obvious patterns
	// (byte-constant comparison).
	switch (e->getKind()) {
	case Expr::Constant:
	//	assert(cast<ConstantExpr>(e)->isTrue() &&
	//	"attempt to add invalid (false) constraint");

		if (!cast<ConstantExpr>(e)->isTrue())
			return false;
		return true;

	// split to enable finer grained independence and other optimizations
	case Expr::And: {
		BinaryExpr *be = cast<BinaryExpr>(e);
		if (!addConstraintInternal(be->left)) return false;
		if (!addConstraintInternal(be->right)) return false;
		return true;
	}

	case Expr::Eq: {
		BinaryExpr *be = cast<BinaryExpr>(e);
		if (isa<ConstantExpr>(be->left)) {
			ExprReplaceVisitor visitor(be->right, be->left);
			rewriteConstraints(visitor);
		}
		constraints.push_back(e);
		invalidateSimplifier();
		return true;
	}

	default:
		constraints.push_back(e);
		invalidateSimplifier();
		return true;
	}

	return true;
}

bool ConstraintManager::addConstraint(ref<Expr> e)
{
	e = simplifyExpr(e);
	return addConstraintInternal(e);
}

void ConstraintManager::print(std::ostream& os) const
{
	for (unsigned int i = 0; i < constraints.size(); i++) {
		constraints[i]->print(os);
		os << "\n";
	}
}

ConstraintManager::~ConstraintManager(void)
{
	if (simplifier) delete simplifier;
}
