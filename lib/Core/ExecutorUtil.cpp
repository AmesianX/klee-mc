//===-- ExecutorUtil.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"

#include "Context.h"

#include "static/Sugar.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/Solver.h"

#include "klee/Internal/Module/KModule.h"

#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Target/TargetData.h"
#include <iostream>
#include <cassert>

using namespace klee;
using namespace llvm;

namespace klee {

  ref<klee::ConstantExpr>
  Executor::evalConstantExpr(
	const KModule* km,
	const globaladdr_map* gm,
	llvm::ConstantExpr *ce)
  {
    llvm::Type *type = ce->getType();

    ref<ConstantExpr> op1(0), op2(0), op3(0);
    int numOperands = ce->getNumOperands();

    if (numOperands > 0) op1 = evalConstant(km, gm, ce->getOperand(0));
    if (numOperands > 1) op2 = evalConstant(km, gm, ce->getOperand(1));
    if (numOperands > 2) op3 = evalConstant(km, gm, ce->getOperand(2));

    switch (ce->getOpcode()) {
    default :
      ce->dump();
      std::cerr << "error: unknown ConstantExpr type\n"
                << "opcode: " << ce->getOpcode() << "\n";
      abort();

    case Instruction::Trunc:
      return op1->Extract(0, km->getWidthForLLVMType(type));
    case Instruction::ZExt:  return op1->ZExt(km->getWidthForLLVMType(type));
    case Instruction::SExt:  return op1->SExt(km->getWidthForLLVMType(type));
    case Instruction::Add:   return op1->Add(op2);
    case Instruction::Sub:   return op1->Sub(op2);
    case Instruction::Mul:   return op1->Mul(op2);
    case Instruction::SDiv:  return op1->SDiv(op2);
    case Instruction::UDiv:  return op1->UDiv(op2);
    case Instruction::SRem:  return op1->SRem(op2);
    case Instruction::URem:  return op1->URem(op2);
    case Instruction::And:   return op1->And(op2);
    case Instruction::Or:    return op1->Or(op2);
    case Instruction::Xor:   return op1->Xor(op2);
    case Instruction::Shl:   return op1->Shl(op2);
    case Instruction::LShr:  return op1->LShr(op2);
    case Instruction::AShr:  return op1->AShr(op2);
    case Instruction::BitCast:  return op1;

    case Instruction::IntToPtr:
      return op1->ZExt(km->getWidthForLLVMType(type));

    case Instruction::PtrToInt:
      return op1->ZExt(km->getWidthForLLVMType(type));

    case Instruction::GetElementPtr: {
      ref<ConstantExpr> base = op1->ZExt(Context::get().getPointerWidth());

      foreach (ii, gep_type_begin(ce), gep_type_end(ce)) {
        ref<ConstantExpr> addend =
          ConstantExpr::alloc(0, Context::get().getPointerWidth());

        if (StructType *st = dyn_cast<StructType>(*ii)) {
          const StructLayout *sl = km->targetData->getStructLayout(st);
          ConstantInt *ci = cast<ConstantInt>(ii.getOperand());

          addend = ConstantExpr::alloc(
	    sl->getElementOffset(
	      (unsigned)ci->getZExtValue()),
              Context::get().getPointerWidth());
        } else {
          SequentialType *seq_t = cast<SequentialType>(*ii);
          ref<ConstantExpr> index =
            evalConstant(km, gm, cast<Constant>(ii.getOperand()));
          unsigned elementSize = km->targetData->getTypeStoreSize(
	  	seq_t->getElementType());

          index = index->ZExt(Context::get().getPointerWidth());
          addend = index->Mul(ConstantExpr::alloc(
	    elementSize,
            Context::get().getPointerWidth()));
        }

        base = base->Add(addend);
      }

      return base;
    }

    case Instruction::ICmp: {
      switch(ce->getPredicate()) {
      default: assert(0 && "unhandled ICmp predicate");
      case ICmpInst::ICMP_EQ:  return op1->Eq(op2);
      case ICmpInst::ICMP_NE:  return op1->Ne(op2);
      case ICmpInst::ICMP_UGT: return op1->Ugt(op2);
      case ICmpInst::ICMP_UGE: return op1->Uge(op2);
      case ICmpInst::ICMP_ULT: return op1->Ult(op2);
      case ICmpInst::ICMP_ULE: return op1->Ule(op2);
      case ICmpInst::ICMP_SGT: return op1->Sgt(op2);
      case ICmpInst::ICMP_SGE: return op1->Sge(op2);
      case ICmpInst::ICMP_SLT: return op1->Slt(op2);
      case ICmpInst::ICMP_SLE: return op1->Sle(op2);
      }
    }

    case Instruction::Select:
      return op1->isTrue() ? op2 : op3;

    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FCmp:
      assert(0 && "floating point ConstantExprs unsupported");
    }
  }

}
