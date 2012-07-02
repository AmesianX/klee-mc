#include <llvm/Support/CommandLine.h>
#include <llvm/Target/TargetData.h>

#include "PTree.h"
#include "static/Sugar.h"
#include "TimingSolver.h"
#include "Executor.h"
#include "ExeStateManager.h"
#include "MMU.h"
#include "klee/SolverStats.h"
#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "ConstraintSeedCore.h"

using namespace llvm;

extern unsigned		MakeConcreteSymbolic;
uint64_t		klee::MMU::query_c = 0;

namespace {
	cl::opt<bool>
	SimplifySymIndices(
		"simplify-sym-indices",
		cl::desc("Simplify indicies/values on mem access."),
		cl::init(false));

	cl::opt<unsigned>
	MaxSymArraySize(
		"max-sym-array-size",
		cl::desc("Concretize accesses to large symbolic arrays"),
		cl::init(0));

	cl::opt<unsigned>
	MaxResolves(
		"max-err-resolves",
		cl::desc("Maximum number of states to fork on MemErr"),
		cl::init(0));
}

using namespace klee;

void MMU::writeToMemRes(
  	ExecutionState& state,
	const struct MemOpRes& res,
	const ref<Expr>& value)
{
	if (res.os->readOnly) {
		exe.terminateStateOnError(
			state,
			"memory error: object read only",
			"readonly.err");
	} else {
		ObjectState *wos;
		wos = state.addressSpace.getWriteable(res.mo, res.os);
		state.write(wos, res.offset, value);
	}
}

Expr::Width MMU::MemOp::getType(const KModule* m) const
{
	if (type_cache != -1)
		return type_cache;

	type_cache = (isWrite
		? value->getWidth()
		: m->targetData->getTypeSizeInBits(
			target->getInst()->getType()));

	return type_cache;
}

void MMU::MemOp::simplify(ExecutionState& state)
{
	if (!isa<ConstantExpr>(address))
		address = state.constraints.simplifyExpr(address);
	if (isWrite && !isa<ConstantExpr>(value))
		value = state.constraints.simplifyExpr(value);
}

/* handles a memop that can be immediately resolved */
bool MMU::memOpFast(ExecutionState& state, MemOp& mop)
{
	Expr::Width	type;
	MemOpRes	res;

	type = mop.getType(exe.getKModule());
	res = memOpResolve(state, mop.address, type);
	if (res.isBad())
		return false;

	commitMOP(state, mop, res);
	return true;
}

void MMU::analyzeOffset(ExecutionState& st, const MemOpRes& res)
{
	ConstraintSeedCore::logConstraint(
		&exe,
		SltExpr::create(
			res.offset,
			ConstantExpr::create(
				-((int64_t)(res.os->size + 1024*1024*16)),
				res.offset->getWidth())));

	ConstraintSeedCore::logConstraint(
		&exe,
		SgtExpr::create(
			res.offset,
			ConstantExpr::create(
				((int64_t)(res.os->size + 1024*1024*16)),
				res.offset->getWidth())));
}

void MMU::commitMOP(
	ExecutionState& state, const MemOp& mop, const MemOpRes& res)
{
	if (res.offset->getKind() != Expr::Constant)
		analyzeOffset(state, res);

	if (mop.isWrite) {
		writeToMemRes(state, res, mop.value);
	} else {
		ref<Expr> result = state.read(
			res.os, res.offset, mop.getType(exe.getKModule()));
		if (MakeConcreteSymbolic)
			result = replaceReadWithSymbolic(state, result);

		state.bindLocal(mop.target, result);
	}
}

ref<Expr> MMU::readDebug(ExecutionState& state, uint64_t addr)
{
	ObjectPair	op;
	uint64_t	off;
	bool		found;

	found = state.addressSpace.resolveOne(addr, op);
	if (found == false)
		return NULL;

	off = addr - op.first->address;
	return state.read(op.second, off, 64);
}

ExecutionState* MMU::getUnboundAddressState(
	ExecutionState	*unbound,
	MemOp		&mop,
	ObjectPair	&resolution,
	unsigned	bytes,
	Expr::Width	type)
{
	MemOpRes	res;
	ExecutionState	*bound;
	ref<Expr>	inBoundPtr;

	res.op = resolution;
	res.mo = res.op.first;
	res.os = res.op.second;
	inBoundPtr = res.mo->getBoundsCheckPointer(mop.address, bytes);

	Executor::StatePair branches(exe.fork(*unbound, inBoundPtr, true));
	unbound = NULL;	/* pointer now invalid */

	bound = branches.first;

	// bound can be 0 on failure or overlapped
	if (bound == NULL) {
		return branches.second;
	}

	res.offset = res.mo->getOffsetExpr(mop.address);
	commitMOP(*bound, mop, res);

	return branches.second;
}

void MMU::memOpError(ExecutionState& state, MemOp& mop)
{
	Expr::Width	type;
	unsigned	bytes;
	ResolutionList	rl;
	ExecutionState	*unbound;
	bool		incomplete;

	type = mop.getType(exe.getKModule());
	bytes = Expr::getMinBytesForWidth(type);

	incomplete = state.addressSpace.resolve(
		state, exe.getSolver(), mop.address, rl, MaxResolves);

	// XXX there is some query wasteage here. who cares?
	unbound = &state;
	foreach (it, rl.begin(), rl.end()) {
		ObjectPair	res(*it);

		unbound = getUnboundAddressState(
			unbound, mop, res, bytes, type);

		/* bad unbound state.. terminate */
		if (unbound == NULL)
			return;
	}

	if (incomplete) {
		/* Did not resolve everything we could.
		 * Let's check for an invalid pointer.. */
		ref<Expr>	oob_cond;

		oob_cond = state.addressSpace.getOOBCond(mop.address);

		Executor::StatePair branches(
			exe.fork(*unbound, oob_cond, true));

		/* ptr maps to OOB region */
		if (branches.first) {
			exe.terminateStateOnError(
				*branches.first,
				"memory error: out of bound pointer",
				"ptr.err",
				exe.getAddressInfo(*branches.first, mop.address));
		}

		/* ptr does not map to OOB region */
		if (branches.second)
			exe.terminateStateEarly(
				*branches.second,
				"query timed out (memOpError)");

		if (!branches.first && !branches.second) {
			klee_warning(
				"MMU query total timeout: may miss OOB ptr");
		}
		return;
	}

	exe.terminateStateOnError(
		*unbound,
		"memory error: out of bound pointer",
		"ptr.err",
		exe.getAddressInfo(*unbound, mop.address));
}

void MMU::exeMemOp(ExecutionState &state, MemOp mop)
{
	uint64_t	q_start;

	q_start = stats::queries;

	if (SimplifySymIndices) mop.simplify(state);

	if (memOpFast(state, mop))
		goto done;

	// handle straddled accesses
	if (memOpByByte(state, mop))
		goto done;

	// we are on an error path
	// Possible reasons:
	// 	* no resolution
	// 	* multiple resolution
	// 	* one resolution with out of bounds
	memOpError(state, mop);

done:
	query_c += stats::queries - q_start;
}

ref<Expr> MMU::replaceReadWithSymbolic(
	ExecutionState &state, ref<Expr> e)
{
	static unsigned	id;
	ref<Array>	array;
	unsigned	n;

	n = MakeConcreteSymbolic;
	if (!n || exe.isReplayOut() || exe.isReplayPaths()) return e;

	// right now, we don't replace symbolics (is there any reason too?)
	if (!isa<ConstantExpr>(e)) return e;

	/* XXX why random? */
	if (n != 1 && random() % n) return e;

	// create a new fresh location, assert it is equal to
	// concrete value in e and return it.

	array = Array::create(
		"rrws_arr" + llvm::utostr(++id),
		MallocKey(Expr::getMinBytesForWidth(e->getWidth())));

	ref<Expr> res = Expr::createTempRead(array.get(), e->getWidth());
	ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
	std::cerr << "Making symbolic: " << eq << "\n";
	exe.addConstraint(state, eq);
	return res;
}

/* handles a memop that can be immediately resolved */
bool MMU::memOpByByte(ExecutionState& state, MemOp& mop)
{
	Expr::Width	type;
	unsigned	bytes;
	ref<Expr>	read_expr;

	type = mop.getType(exe.getKModule());
	if ((type % 8) != 0) return false;

	bytes = Expr::getMinBytesForWidth(type);
	for (unsigned i = 0; i < bytes; i++) {
		ref<Expr>	byte_addr;
		ref<Expr>	byte_val;
		MemOpRes	res;

		byte_addr = AddExpr::create(
			mop.address,
			ConstantExpr::create(i, mop.address->getWidth()));

		res = memOpResolve(state, byte_addr, 8);
		if (res.isBad())
			return false;
		if (mop.isWrite) {
			byte_val = ExtractExpr::create(mop.value, 8*i, 8);
			writeToMemRes(state, res, byte_val);
		} else {
			ref<Expr> result = state.read(res.os, res.offset, 8);
			if (read_expr.isNull())
				read_expr = result;
			else
				read_expr = ConcatExpr::create(
					result, read_expr);
		}
	}

	// we delayed setting the local variable on the read because
	// we need to respect the original type width-- result is concatenated
	if (!mop.isWrite) {
		state.bindLocal(mop.target, read_expr);
	}

	return true;
}

MMU::MemOpRes MMU::memOpResolveConst(
	ExecutionState& state,
	uint64_t addr,
	Expr::Width type)
{
	MemOpRes	ret;
	unsigned	bytes;
	bool		tlb_hit;

	bytes = Expr::getMinBytesForWidth(type);
	ret.rc = true;
	ret.usable = false;
	ret.op.first = NULL;

	tlb_hit = tlb.get(state, addr, ret.op);
	if (!tlb_hit || !ret.op.first->isInBounds(addr, bytes)) {
		bool	in_bounds;

		if (state.addressSpace.resolveOne(addr, ret.op) == false) {
			/* no feasible objects */
			return ret;
		}
		tlb.put(state, ret.op);

		in_bounds = ret.op.first->isInBounds(addr, bytes);
		if (in_bounds == false) {
			/* not in bounds */
			return ret;
		}
	}

	ret.mo = ret.op.first;
	ret.os = ret.op.second;
	ret.offset = ConstantExpr::create(
		ret.mo->getOffset(addr),
		Context::get().getPointerWidth());
	ret.usable = true;
	return ret;
}

MMU::MemOpRes MMU::memOpResolve(
	ExecutionState& state,
	ref<Expr> addr,
	Expr::Width type)
{
	if (const ConstantExpr* CE = dyn_cast<ConstantExpr>(addr))
		return memOpResolveConst(state, CE->getZExtValue(), type);

	return memOpResolveExpr(state, addr, type);
}

MMU::MemOpRes MMU::memOpResolveExpr(
	ExecutionState& state,
	ref<Expr> addr,
	Expr::Width type)
{
	MemOpRes	ret;
	unsigned	bytes;
	bool		alwaysInBounds;

	bytes = Expr::getMinBytesForWidth(type);
	ret.usable = false;

	ret.rc = state.addressSpace.getFeasibleObject(
		state, exe.getSolver(), addr, ret.op);
	if (!ret.rc) {
		/* solver failed in GFO, force addr to be concrete */
		addr = exe.toConstant(state, addr, "resolveOne failure");
		ret.op.first = NULL;
		ret.rc = state.addressSpace.resolveOne(
			cast<ConstantExpr>(addr), ret.op);
		if (!ret.rc)
			return MemOpRes::failure();
	}

	if (ret.op.first == NULL) {
		/* no feasible objects exist */
		return ret;
	}

	// fast path: single in-bounds resolution.
	assert (ret.op.first != NULL);
	ret.mo = ret.op.first;
	ret.os = ret.op.second;

	if (MaxSymArraySize && ret.mo->size >= MaxSymArraySize) {
		addr = exe.toConstant(state, addr, "max-sym-array-size");
	}

	ret.offset = ret.mo->getOffsetExpr(addr);

	/* verify access is in bounds */
	ret.rc = exe.getSolver()->mustBeTrue(
		state,
		ret.mo->getBoundsCheckOffset(ret.offset, bytes),
		alwaysInBounds);
	if (!ret.rc) {
		state.pc = state.prevPC;
		exe.terminateStateEarly(
			state, "query timed out (memOpResolve)");
		return MemOpRes::failure();
	}

	if (!alwaysInBounds)
		return ret;

	ret.usable = true;
	return ret;
}
