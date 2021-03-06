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

#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprVisitor.h"
#include "klee/util/ExprHashMap.h"
#include "static/Sugar.h"
#include "ExprReplaceVisitor.h"

#include <stack>
#include <string.h>
#include <iostream>
#include <list>
#include <map>

using namespace klee;

#define MAX_UPDATE_TIME	1000
#define MAX_REPL_SIZE	1000

namespace {
	llvm::cl::opt<bool>
	SimplifyUpdates(
		"simplify-updates",
		llvm::cl::desc(
		"Simplifies update list expressions in constraint manager."),
		llvm::cl::init(true));
}

unsigned ConstraintManager::simplify_c = 0;
unsigned ConstraintManager::simplified_c = 0;
unsigned ConstraintManager::timeout_c = 0;

namespace klee
{

class ExprReplaceVisitor2 : public ExprVisitor
{
public:
	typedef ExprHashMap<ref<Expr>> replmap_ty;
	ExprReplaceVisitor2(void) : ExprVisitor(true)
	{ }

	ExprReplaceVisitor2(const replmap_ty& _replacements)
	: ExprVisitor(true)
	, replacements(_replacements) {}

	virtual ref<Expr> apply(const ref<Expr>& e)
	{
		if (replacements.size() > MAX_REPL_SIZE)
			replacements.clear();
		return ExprVisitor::apply(e);
	}

	Action visitExprPost(const Expr &e)
	{
		replmap_ty::const_iterator it;
		it = replacements.find(ref<Expr>(const_cast<Expr*>(&e)));
		if (it == replacements.end())
			return Action::doChildren();

		ConstraintManager::incReplacements();
		return Action::changeTo(it->second);
	}

	replmap_ty& getReplacements(void) { return replacements; }

protected:
	virtual Action visitRead(const ReadExpr &re);

private:
	replmap_ty replacements;
};

}

ExprVisitor::Action ExprReplaceVisitor2::visitRead(const ReadExpr &re)
{
	std::stack<std::pair<ref<Expr>, ref<Expr> > > updateStack;
	ref<Expr>		uniformValue(0);
	bool			rebuild, rebuildUpdates;
	const UpdateList	&ul(re.updates);

	// fast path: no updates, reading from constant array
	// with a single value occupying all indices in the array
	if (!ul.head && ul.getRoot()->isSingleValue()) {
		ConstraintManager::incReplacements();
		return Action::changeTo(ul.getRoot()->getValue(0));
	}

	ref<Expr> readIndex = isa<ConstantExpr>(re.index)
		? re.index
		: visit(re.index);

	// simplify case of a known read from a constant array
	if (	isa<ConstantExpr>(readIndex) &&
		!ul.head && ul.getRoot()->isConstantArray())
	{
		uint64_t idx;
		
		ConstraintManager::incReplacements();

		idx = cast<ConstantExpr>(readIndex)->getZExtValue();
		if (idx < ul.getRoot()->mallocKey.size)
			return Action::changeTo(ul.getRoot()->getValue(idx));

		std::cerr << "[Constraints] WTF: ARR=" << ul.getRoot()->name
			<< ". BAD IDX="
			<< idx << ". Size="
			<<  ul.getRoot()->mallocKey.size << '\n';
		klee_warning_once(
			0,
			"out of bounds constant array read (possibly within "
			"an infeasible Select path?)");

		return Action::changeTo(MK_CONST(0, re.getWidth()));
	}

	/* rebuilding fucks up symbolics on underconstrained exe */
	if (SimplifyUpdates == false)
		return Action::doChildren();

	rebuild = rebuildUpdates = false;
	if (readIndex != re.index)
		rebuild = true;

	uniformValue = buildUpdateStack(
		re.updates, readIndex, updateStack, rebuildUpdates);
	if (!uniformValue.isNull()) {
		ConstraintManager::incReplacements();
		return Action::changeTo(uniformValue);
	}

	if (rebuild && !rebuildUpdates) {
		ConstraintManager::incReplacements();
		return Action::changeTo(
			ReadExpr::create(re.updates, readIndex));
	}


	// at least one update was simplified? rebuild
	if (rebuildUpdates) {
		UpdateList	*newUpdates;
		ref<Expr>	new_re;

		newUpdates = UpdateList::fromUpdateStack(
			re.updates.getRoot().get(), updateStack);
		if (newUpdates == NULL)
			return Action::doChildren();

		new_re = ReadExpr::create(*newUpdates, readIndex);
		delete newUpdates;

		ConstraintManager::incReplacements();
		return Action::changeTo(new_re);
	}


	return Action::doChildren();
}

bool ConstraintManager::rewriteConstraints(ExprVisitor &visitor)
{
	constraints_t old_c;
	readsets_t old_rs;
	bool changed = false;

	constraints.swap(old_c);
	readsets.swap(old_rs);
	invalidateSimplifier();

	assert (!Expr::errors);

	for (unsigned i = 0; i < old_c.size(); i++) {
		ref<Expr> &constr_e = old_c[i];
		ref<Expr> e = visitor.apply(constr_e);

		/* ulp. */
		if (Expr::errors)
			break;

		if (!e.isNull() && e != constr_e) {
			// enable further reductions
			addConstraintInternal(e);
			changed = true;
			continue;
		}

		assert (!Expr::errors);
		if (changed)
			e = simplifyExpr(constr_e);

		if (!e.isNull()) {
			/* use new expression */
			if (e->getKind() != Expr::Constant) {
				constraints.push_back(e);
				readsets.push_back(ReadSet::get(e));
			}
		} else {
			/* use old expression */
			constraints.push_back(constr_e);
			readsets.push_back(old_rs[i]);
		}

		assert (!Expr::errors);
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

#define TRUE_EXPR	ConstantExpr::alloc(1, Expr::Bool)
#define FALSE_EXPR	ConstantExpr::alloc(0, Expr::Bool)

static void addEquality(
	ExprReplaceVisitor2::replmap_ty& equalities,
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
				MK_ULE(x->getKid(1), x->getKid(0)),
				FALSE_EXPR));
		return;
	}

	if (const UleExpr *x = dyn_cast<UleExpr>(e)) {
		equalities.insert(
			std::make_pair(
				MK_ULT(x->getKid(1), x->getKid(0)),
				FALSE_EXPR));

		// x <= 0 implies x == 0
		if (x->getKid(1)->isZero())
			equalities.insert(
				std::make_pair(x->getKid(0), x->getKid(1)));

		return;
	}
}

void ConstraintManager::setupSimplifier(void) const
{
	assert (simplifier == NULL);
	simplifier = new ExprTimer<ExprReplaceVisitor2>(MAX_UPDATE_TIME);
	ExprReplaceVisitor2::replmap_ty &equalities(simplifier->getReplacements());

	foreach (it, constraints.begin(), constraints.end())
		addEquality(equalities, *it);
}

ref<Expr> ConstraintManager::simplifyExpr(ref<Expr> e) const
{
	ref<Expr>	ret;

	if (isa<ConstantExpr>(e)) return e;

	if (simplifier == NULL)
		setupSimplifier();

	assert (!Expr::errors);
	simplify_c++;
	ret = simplifier->apply(e);
	if (ret.isNull()) {
		timeout_c++;
		return e;
	}

	if (ret->hash() != e->hash())
		simplified_c++;

	if (Expr::errors) {
		std::cerr << "CONSTRAINT MANAGER RUINED EXPR!!!!!!\n";
		Expr::resetErrors();
		return e;
	}

	return ret;
}

bool ConstraintManager::addConstraintInternal(ref<Expr> e)
{
	assert (!Expr::errors);

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
		break;

	// split to enable finer grained independence and other optimizations
	case Expr::And: {
		BinaryExpr *be = cast<BinaryExpr>(e);
		if (!addConstraintInternal(be->left)) return false;
		if (!addConstraintInternal(be->right)) return false;
		break;
	}

	case Expr::Eq: {
		BinaryExpr *be = cast<BinaryExpr>(e);
		if (isa<ConstantExpr>(be->left)) {
			ExprTimer<ExprReplaceVisitor> visitor(
				be->right, be->left, MAX_UPDATE_TIME);
			rewriteConstraints(visitor);
		}
		constraints.push_back(e);
		readsets.push_back(ReadSet::get(e));
		invalidateSimplifier();
		break;
	}

	default:
		constraints.push_back(e);
		readsets.push_back(ReadSet::get(e));
		invalidateSimplifier();
		break;
	}

	return true;
}

bool ConstraintManager::addConstraint(ref<Expr> e)
{
	bool	added;

	e = simplifyExpr(e);
	assert(!Expr::errors);

	assert (readsets.size() == constraints.size());
	added = addConstraintInternal(e);
	assert (readsets.size() == constraints.size());

	if (Expr::errors) {
		Expr::resetErrors();
		return false;
	}

	return added;
}

void ConstraintManager::print(std::ostream& os) const
{
	for (unsigned int i = 0; i < constraints.size(); i++) {
		constraints[i]->print(os);
		os << "\n";
	}
}

ConstraintManager::~ConstraintManager(void)
{ if (simplifier) delete simplifier; }

bool ConstraintManager::isValid(const Assignment& a) const
{ return a.satisfies(begin(), end()); }

bool ConstraintManager::apply(const Assignment& a)
{
	constraints_t	new_constrs;
	bool		updated = false;

	for (auto& old_e : constraints) {
		ref<Expr> e(a.evaluate(old_e));

		if (e->hash() == old_e->hash()) {
			new_constrs.push_back(old_e);
			continue;
		}

		updated = true;

		/* constraint subsumed? */
		if (e->getKind() == Expr::Constant) {
			const ConstantExpr *ce = dyn_cast<ConstantExpr>(e);
			if (ce->isFalse())
				return false;
			continue;
		}

		/* save updated constraint */
		new_constrs.push_back(e);
	}

	/* it worked, but nothing happened */
	if (!updated) return true;

	/* write back new constraint set */
	invalidateSimplifier();
	constraints.clear();
	readsets.clear();
	for (auto& e : new_constrs) addConstraint(e);

	return true;
}

ConstraintManager ConstraintManager::operator -(
	const ConstraintManager& other) const
{
	ConstraintManager	ret;
	std::set< ref<Expr> >	lhs(constraints.begin(), constraints.end());
	std::set< ref<Expr> >	rhs(
		other.constraints.begin(), other.constraints.end());

	for (auto& e : lhs) {
		if (rhs.count(e) == 0)
			ret.addConstraint(e);
	}

	return ret;
}

ConstraintManager::ConstraintManager(const constraints_t &_constraints)
: constraints(_constraints)
, simplifier(NULL)
{
	for (unsigned i = 0; i < constraints.size(); i++) {
		readsets.push_back(ReadSet::get(constraints[i]));
	}
}

ConstraintManager::ConstraintManager(
	const constraints_t &_constraints,
	const readsets_t &_readsets)
: constraints(_constraints)
, readsets(_readsets)
, simplifier(NULL)
{
#if 0
	for (int i = 0; i < constraints.size(); i++) {
		assert (readsets[i] == ReadSet::get(constraints[i]));
	}
#endif
}

ref<Expr> ConstraintManager::getConjunction(void) const
{
	ref<Expr> e(MK_CONST(1, 1));
	foreach (it, constraints.begin(), constraints.end()) {
		e = MK_AND(e, *it);
	}
	return e;
}
