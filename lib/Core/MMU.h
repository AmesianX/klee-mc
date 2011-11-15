#ifndef MMU_H
#define MMU_H

#include "Memory.h"

namespace klee
{

class Executor;
class ExecutionState;

class MMU
{
public:
	MMU(Executor& e) : exe(e) {}
	virtual ~MMU() {}

	struct MemOp
	{
	public:
		Expr::Width getType(const KModule* m) const;
		void simplify(ExecutionState& es);
		MemOp(	bool _isWrite, ref<Expr> _addr, ref<Expr> _value,
			KInstruction* _target)
		: isWrite(_isWrite)
		, address(_addr)
		, value(_value)
		, target(_target)
		{}

		bool		isWrite;
		ref<Expr>	address;
		ref<Expr>	value;		/* undef if read */
		KInstruction	*target;	/* undef if write */
	};

	// do address resolution / object binding / out of bounds checking
	// and perform the operation
	void exeMemOp(ExecutionState &state, MemOp mop);

private:
	struct MemOpRes
	{
		ObjectPair		op;
		ref<Expr>		offset;
		const MemoryObject	*mo;
		const ObjectState	*os;
		bool			usable;
		bool			rc;	/* false => solver failure */
	};

protected:

	MemOpRes memOpResolve(
		ExecutionState& state,
		ref<Expr> address,
		Expr::Width type);

	bool memOpFast(ExecutionState& state, MemOp& mop);

	void writeToMemRes(
		ExecutionState& state,
		struct MemOpRes& res,
		ref<Expr> value);

	bool memOpByByte(ExecutionState& state, MemOp& mop);

	ExecutionState* getUnboundAddressState(
		ExecutionState	*unbound,
		MemOp&		mop,
		ObjectPair	&resolution,
		unsigned	bytes,
		Expr::Width	type);

	void memOpError(ExecutionState& state, MemOp& mop);

	// Called on [for now] concrete reads, replaces constant with a symbolic
	// Used for testing.
	ref<Expr> replaceReadWithSymbolic(ExecutionState &state, ref<Expr> e);


private:
	Executor& exe;
};

}

#endif