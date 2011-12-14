//===-- KInstruction.h ------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_KINSTRUCTION_H
#define KLEE_KINSTRUCTION_H

#include <llvm/Support/DataTypes.h>
#include <vector>

namespace llvm {
	class Instruction;
}

namespace klee {
class Executor;
struct InstructionInfo;
class KModule;

/// KInstruction - Intermediate instruction representation used
/// during execution.
class KInstruction
{
public:
	virtual ~KInstruction();
	llvm::Instruction* getInst(void) const { return inst; }
	static KInstruction* create(
		KModule* km,
		llvm::Instruction* inst, unsigned dest);
	unsigned getNumArgs(void) const;
	bool isCall(void) const;
	void setOperand(unsigned op_num, int n) { operands[op_num] = n; }
	int getOperand(unsigned op_num) const { return operands[op_num]; }
	unsigned getDest(void) const { return dest; }
	void setInfo(const InstructionInfo* inf) { info = inf; }
	const InstructionInfo* getInfo(void) const { return info; }

protected:
	KInstruction() {}
	KInstruction(llvm::Instruction* inst, unsigned dest);

private:
	llvm::Instruction	*inst;
	const InstructionInfo	*info;

	/// Value numbers for each operand. -1 is an invalid value,
	/// otherwise negative numbers are indices (negated and offset by
	/// 2) into the module constant table and positive numbers are
	/// register indices.
	int			*operands;

	/// Destination register index.
	unsigned		dest;
};

class KGEPInstruction : public KInstruction
{
public:
	KGEPInstruction(
		KModule* km, llvm::Instruction* inst, unsigned dest);

	virtual ~KGEPInstruction() {}
	/// indices - The list of variable sized adjustments to add to the pointer
	/// operand to execute the instruction. The first element is the operand
	/// index into the GetElementPtr instruction, and the second element is the
	/// element size to multiple that index by.
	std::vector< std::pair<unsigned, uint64_t> > indices;

	uint64_t getOffsetBits(void) const { return offset; }
private:
	template <typename TypeIt>
	void computeOffsets(KModule* km, TypeIt ib, TypeIt ie);

	/// offset - A constant offset to add to the pointer operand to execute the
	/// insturction.
	uint64_t offset;
};
}

#endif
