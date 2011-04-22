//===-- MemoryManager.h -----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_MEMORYMANAGER_H
#define KLEE_MEMORYMANAGER_H

#include "Memory.h"

#include "klee/util/Ref.h"

#include <vector>
#include <map>
#include <set>
#include <list>
#include <stdint.h>

namespace llvm {
  class Value;
}

namespace klee {
  class MemoryObject;
  class MemoryManager;
  class ExecutionState;

  class MemoryManager {
    friend class HeapObject;
    friend class MemoryObject;

  public:
    typedef std::multimap<MallocKey,HeapObject*> HeapMap;

  private:
    typedef std::set<MemoryObject*> objects_ty;
    // pointers to all allocated MemoryObjects
    objects_ty objects;
    // references to anonymous MemoryObjects (ones not tied to a state)
    std::list<ref<MemoryObject> > anonMemObjs;
    // references to anonymous HeapObjects (ones not tied to a state)
    std::list<ref<HeapObject> > anonHeapObjs;
    // map containing all allocated heap objects
    HeapMap heapObjects;

  public:
    MemoryManager() {
      HeapObject::memoryManager = this;
      MemoryObject::memoryManager = this;
    }
    ~MemoryManager();

    MemoryObject *allocate(uint64_t size, bool isLocal, bool isGlobal,
                           const llvm::Value *allocSite, ExecutionState *state);
    MemoryObject *allocateFixed(uint64_t address, uint64_t size,
                                const llvm::Value *allocSite,
                                ExecutionState *state);
    void deallocate(const MemoryObject *mo);
    template <typename F>
    void deallocateAllExcept(F f)
    {
      std::set<MemoryObject*> od;
      for (objects_ty::iterator i = objects.begin(); i != objects.end();
           ++i) {
        MemoryObject* mo = *i;
        if (!f(mo))
          od.insert(mo);
      }
      for(std::set<MemoryObject*>::iterator i = od.begin(); i != od.end(); ++i)
        deallocate(*i);
    }
  };

} // End klee namespace

#endif