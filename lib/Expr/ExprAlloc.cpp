#include <tr1/unordered_map>
#include "klee/Expr.h"
#include "ExprAlloc.h"

using namespace klee;

struct hashapint
{
unsigned operator()(const llvm::APInt& a) const { return a.getHashValue(); }
};

struct apinteq
{
bool operator()(const llvm::APInt& a, const llvm::APInt& b) const
{
	if (a.getBitWidth() != b.getBitWidth()) return false;
	return a == b;
}
};

/* important to use an unordered_map instead of a map so we get O(1) access. */
typedef std::tr1::unordered_map<
	llvm::APInt,
	ref<ConstantExpr>,
	hashapint,
	apinteq> ConstantExprTab;

ConstantExprTab			const_hashtab;
static ref<ConstantExpr>	ce_smallval_tab_1[2];
static ref<ConstantExpr>	ce_smallval_tab_8[256];
static ref<ConstantExpr>	ce_smallval_tab_16[256];
static ref<ConstantExpr>	ce_smallval_tab_32[256];
static ref<ConstantExpr>	ce_smallval_tab_64[256];

void initSmallValTab(void)
{
	ce_smallval_tab_1[0] = ref<ConstantExpr>(
		new ConstantExpr(llvm::APInt(1, 0)));
	ce_smallval_tab_1[1] = ref<ConstantExpr>(
		new ConstantExpr(llvm::APInt(1, 1)));

	ce_smallval_tab_1[0]->computeHash();
	ce_smallval_tab_1[1]->computeHash();

#define SET_SMALLTAB(w)	\
	for (unsigned int i = 0; i < 256; i++) {	\
		ref<ConstantExpr>	r(new ConstantExpr(llvm::APInt(w, i)));	\
		ce_smallval_tab_##w[i] = r;	\
		r->computeHash();		\
	}

	SET_SMALLTAB(8)
	SET_SMALLTAB(16)
	SET_SMALLTAB(32)
	SET_SMALLTAB(64)
}

static bool tab_ok = false;
unsigned long ExprAlloc::constantCount = 0;


ExprAlloc::~ExprAlloc() {}

ref<Expr> ExprAlloc::Constant(const llvm::APInt &v)
{
	ConstantExprTab::iterator	it;
	uint64_t			v_64;

	if (v.getBitWidth() <= 64 && (v_64 = v.getLimitedValue()) < 256) {
		if (tab_ok == false) {
			initSmallValTab();
			constantCount = 256*4+2;
			tab_ok = true;
		}

		switch (v.getBitWidth()) {
		case 1: return ce_smallval_tab_1[v_64];
		case 8: return ce_smallval_tab_8[v_64];
		case 16: return ce_smallval_tab_16[v_64];
		case 32: return ce_smallval_tab_32[v_64];
		case 64: return ce_smallval_tab_64[v_64];
		default: break;
		}
	}

	it = const_hashtab.find(v);
	if (it != const_hashtab.end()) {
		return it->second;
	}

	ref<ConstantExpr> r(new ConstantExpr(v));
	r->computeHash();
	const_hashtab.insert(std::make_pair(v, r));
	constantCount++;

	return r;
}

#define DECL_ALLOC_1(x)				\
ref<Expr> ExprAlloc::x(const ref<Expr>& src)	\
{	\
	ref<Expr> r(new x##Expr(src));	\
	r->computeHash();		\
	return r;			\
}

DECL_ALLOC_1(NotOptimized)
DECL_ALLOC_1(Not)

#define DECL_ALLOC_2(x)	\
ref<Expr> ExprAlloc::x(const ref<Expr>& lhs, const ref<Expr>& rhs)	\
{	\
	ref<Expr> r(new x##Expr(lhs, rhs));	\
	r->computeHash();		\
	return r;			\
}
DECL_ALLOC_2(Concat)
DECL_ALLOC_2(Add)
DECL_ALLOC_2(Sub)
DECL_ALLOC_2(Mul)
DECL_ALLOC_2(UDiv)

DECL_ALLOC_2(SDiv)
DECL_ALLOC_2(URem)
DECL_ALLOC_2(SRem)
DECL_ALLOC_2(And)
DECL_ALLOC_2(Or)
DECL_ALLOC_2(Xor)
DECL_ALLOC_2(Shl)
DECL_ALLOC_2(LShr)
DECL_ALLOC_2(AShr)
DECL_ALLOC_2(Eq)
DECL_ALLOC_2(Ne)
DECL_ALLOC_2(Ult)
DECL_ALLOC_2(Ule)

DECL_ALLOC_2(Ugt)
DECL_ALLOC_2(Uge)
DECL_ALLOC_2(Slt)
DECL_ALLOC_2(Sle)
DECL_ALLOC_2(Sgt)
DECL_ALLOC_2(Sge)

ref<Expr> ExprAlloc::Read(const UpdateList &updates, const ref<Expr> &idx)
{
	ref<Expr> r(new ReadExpr(updates, idx));
	r->computeHash();
	return r;
}

ref<Expr> ExprAlloc::Select(
	const ref<Expr> &c,
	const ref<Expr> &t, const ref<Expr> &f)
{
	ref<Expr> r(new SelectExpr(c, t, f));
	r->computeHash();
	return r;
}

ref<Expr> ExprAlloc::Extract(const ref<Expr> &e, unsigned o, Expr::Width w)
{
	ref<Expr> r(new ExtractExpr(e, o, w));
	r->computeHash();
	return r;
}

ref<Expr> ExprAlloc::ZExt(const ref<Expr> &e, Expr::Width w)
{
	ref<Expr> r(new ZExtExpr(e, w));
	r->computeHash();
	return r;
}

ref<Expr> ExprAlloc::SExt(const ref<Expr> &e, Expr::Width w)
{
	ref<Expr> r(new SExtExpr(e, w));
	r->computeHash();
	return r;
}
