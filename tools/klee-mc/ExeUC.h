#ifndef KLEE_EXEUC_H
#define KLEE_EXEUC_H

#include "ExecutorVex.h"
#include "guestcpustate.h"

namespace klee
{
class ObjectState;
class Array;

typedef std::pair<unsigned /* off */, unsigned /* len */> Exemptent;
typedef std::vector<Exemptent> Exempts;

struct UCPtr
{
	static UCPtr failure(void)
	{
		UCPtr	ucp;
		ucp.base_arr = 0;
		ucp.ptrtab_idx = 0;
		ucp.base_off = 0;
		ucp.depth = ~0;
		return ucp;
	}
	const Array	*base_arr;
	int		ptrtab_idx;
	ref<Expr>	base_off;
	unsigned	depth;
};

#pragma pack(1)
struct UCTabEnt
{
	uint32_t	len;
	void		*sym_ptr;
	void		*real_ptr;
};
#pragma pack()

class ExeUC : public ExecutorVex
{
public:
class UCPtrFork
{
public:
	UCPtrFork(StatePair& in_sp, ref<Expr>& in_real)
	: sp(in_sp), real_ptr(in_real) {}

	virtual ~UCPtrFork() {}

	ExecutionState* getState(bool t) const
	{ return (t) ? sp.first : sp.second; }

	ref<Expr> getRealPtr(void) const { return real_ptr; }

private:
	StatePair	sp;
	ref<Expr>	real_ptr;
};
	ExeUC(InterpreterHandler *ie, Guest* gs);
	virtual ~ExeUC();

	virtual void runImage(void);
	void setupUCEntry(
		ExecutionState* start_state,
		const char *xchk_fn);
	const Array* getRootArray(void) const { return root_reg_arr; }
	const Array* getPtrTabArray(void) const { return lentab_arr; }

	UCPtrFork initUCPtr(ExecutionState& st, unsigned idx, unsigned min_sz);
	UCPtrFork forkUCPtr(
		ExecutionState	&es,
		MemoryObject	*new_mo,
		unsigned	idx);
	ExecutionState* forkNullPtr(ExecutionState& es, unsigned pt_idx);

	ref<Expr> getUCRealPtr(ExecutionState& es, unsigned idx);
	ref<Expr> getUCSymPtr(ExecutionState& es, unsigned idx);
	uint64_t getUCSym2Real(ExecutionState& es, ref<Expr> sym_ptr);

	unsigned sym2idx(const Expr* sym_ptr) const;
	unsigned tabOff2Idx(unsigned n) const { return n / lentab_elem_len; }
	bool isRegIdx(int idx) const { return idx < (int)lentab_reg_ptrs; }


	unsigned getPtrBytes(void) const;

	/* inlined so that kmc-replay will work */
	static Exempts getRegExempts(const Guest* gs)
	{
		Exempts	ret;

		assert (gs->getArch() == Arch::X86_64 && "STUPID DFLAG");

		ret.push_back(
			Exemptent(
				gs->getCPUState()->getStackRegOff(),
				(gs->getMem()->is32Bit()) ? 4 : 8));
		ret.push_back(Exemptent(160 /* guest_DFLAG */, 8));
		ret.push_back(Exemptent(192 /* guest_FS_ZERO */, 8));

		return ret;
	}

	void finalizeBuffers(ExecutionState& es);

protected:
	void runSym(const char* sym_name);

private:
	/* format:
	 * 0		  			n
	 * [ length  | sym_ptr | real_ptr	]
	 */
#define LEN_OFF		0
#define SYMPTR_OFF	(4)
#define REALPTR_OFF	(4+getPtrBytes())
	MemoryObject	*lentab_mo;
	const Array	*lentab_arr;
	unsigned int	lentab_reg_ptrs;
	unsigned int	lentab_elem_len;
	unsigned int	lentab_max;

	const Array	*root_reg_arr;
};
}

#endif