//===-- Cell.h --------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CELL_H
#define KLEE_CELL_H

#include <klee/Expr.h>
#include "../lib/Core/DependenceNode.h"

namespace klee {
  class MemoryObject;

  class Cell {
  public:
    ref<Expr> value;
    StackWrite* stackWrite;

    Cell(): stackWrite(0) {}
  };
}

#endif
