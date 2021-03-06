//===- Local.cpp - Compute a local data structure graph for a function ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Compute the local version of the data structure graph for a function.  The
// external interface to this file is the DSGraph constructor.
//
//===----------------------------------------------------------------------===//

#include "static/dsa/DataStructure.h"
#include "static/dsa/DSGraph.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/GetElementPtrTypeIterator.h>
#include <llvm/InstVisitor.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/Timer.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseSet.h>

#include <iostream>

// FIXME: This should eventually be a FunctionPass that is automatically
// aggregated into a Pass.
//
#include <llvm/IR/Module.h>

using namespace llvm;

#define DOUT	llvm::raw_fd_ostream(0, false)

static RegisterPass<LocalDataStructures>
X("dsa-local", "Local Data Structure Analysis");

namespace {
  //===--------------------------------------------------------------------===//
  //  GraphBuilder Class
  //===--------------------------------------------------------------------===//
  //
  /// This class is the builder class that constructs the local data structure
  /// graph by performing a single pass over the function in question.
  ///
  class GraphBuilder : InstVisitor<GraphBuilder> {
    DSGraph &G;
    Function* FB;
    DataStructures* DS;

    ////////////////////////////////////////////////////////////////////////////
    // Helper functions used to implement the visitation functions...

    void MergeConstantInitIntoNode(DSNodeHandle &NH, Type* Ty, Constant *C);

    /// createNode - Create a new DSNode, ensuring that it is properly added to
    /// the graph.
    ///
    DSNode *createNode(Type *Ty = 0) 
    {   
      DSNode* ret = new DSNode(Ty, &G);
      assert(ret->getParentGraph() && "No parent?");
      return ret;
    }

    /// setDestTo - Set the ScalarMap entry for the specified value to point to
    /// the specified destination.  If the Value already points to a node, make
    /// sure to merge the two destinations together.
    ///
    void setDestTo(Value &V, const DSNodeHandle &NH);

    /// getValueDest - Return the DSNode that the actual value points to.
    ///
    DSNodeHandle getValueDest(Value &V);

    /// getLink - This method is used to return the specified link in the
    /// specified node if one exists.  If a link does not already exist (it's
    /// null), then we create a new node, link it, then return it.
    ///
    DSNodeHandle &getLink(const DSNodeHandle &Node, unsigned Link = 0);

    ////////////////////////////////////////////////////////////////////////////
    // Visitor functions, used to handle each instruction type we encounter...
    friend class InstVisitor<GraphBuilder>;

    void visitMallocInst(CallInst &MI)
    { setDestTo(MI, createNode()->setHeapMarker()); }

    void visitAllocaInst(AllocaInst &AI)
    { setDestTo(AI, createNode()->setAllocaMarker()); }

    void visitFreeInst(CallInst &FI)
    { if (DSNode *N = getValueDest(*FI.getOperand(0)).getNode())
        N->setHeapMarker();
    }

    //the simple ones
    void visitPHINode(PHINode &PN);
    void visitSelectInst(SelectInst &SI);
    void visitLoadInst(LoadInst &LI);
    void visitStoreInst(StoreInst &SI);
    void visitReturnInst(ReturnInst &RI);
    void visitVAArgInst(VAArgInst   &I);
    void visitIntToPtrInst(IntToPtrInst &I);
    void visitPtrToIntInst(PtrToIntInst &I);
    void visitBitCastInst(BitCastInst &I);
    void visitCmpInst(CmpInst &I);

    //the nasty ones
    void visitGetElementPtrInst(User &GEP);
    void visitCallInst(CallInst &CI);
    void visitInvokeInst(InvokeInst &II);
    void visitInstruction(Instruction &I);

    bool visitIntrinsic(CallSite CS, Function* F);
    void visitCallSite(CallSite CS);

  public:
    GraphBuilder(Function &f, DSGraph &g, DataStructures& DSi)
      : G(g), FB(&f), DS(&DSi) {
      // Create scalar nodes for all pointer arguments...
      for (Function::arg_iterator I = f.arg_begin(), E = f.arg_end();
           I != E; ++I) {
        if (isa<PointerType>(I->getType())) {
          DSNode * Node = getValueDest(*I).getNode();

          if (!f.hasInternalLinkage())
            Node->setExternalMarker();

        }
      }

      // Create an entry for the return, which tracks which functions are in
      // the graph
      g.getOrCreateReturnNodeFor(f);

      visit(f);  // Single pass over the function

      // If there are any constant globals referenced in this function, merge
      // their initializers into the local graph from the globals graph.
      if (g.getScalarMap().global_begin() != g.getScalarMap().global_end()) {
        ReachabilityCloner RC(&g, g.getGlobalsGraph(), 0);
        
        for (DSScalarMap::global_iterator I = g.getScalarMap().global_begin();
             I != g.getScalarMap().global_end(); ++I)
          if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(*I))
            if (!GV->isDeclaration() && GV->isConstant())
              RC.merge(g.getNodeForValue(GV), g.getGlobalsGraph()->getNodeForValue(GV));
      }
      
      g.markIncompleteNodes(DSGraph::MarkFormalArgs);
      
      // Remove any nodes made dead due to merging...
      g.removeDeadNodes(DSGraph::KeepUnreachableGlobals);
    }

    // GraphBuilder ctor for working on the globals graph
    explicit GraphBuilder(DSGraph& g)
      :G(g), FB(0)
    {}

    void mergeInGlobalInitializer(GlobalVariable *GV);
    void mergeFunction(Function& F) { getValueDest(F); }
  };

  /// Traverse the whole DSGraph, and propagate the unknown flags through all 
  /// out edges.
  static void propagateUnknownFlag(DSGraph * G) {
    std::vector<DSNode *> workList;
    DenseSet<DSNode *> visited;
    for (DSGraph::node_iterator I = G->node_begin(), E = G->node_end(); I != E; ++I)
      if (I->isUnknownNode()) 
        workList.push_back(&*I);
  
    while (!workList.empty()) {
      DSNode * N = workList.back();
      workList.pop_back();
      if (visited.count(N) != 0) continue;
      visited.insert(N);
      N->setUnknownMarker();
      for (DSNode::edge_iterator I = N->edge_begin(), E = N->edge_end(); I != E; ++I)
        if (!I->isNull())
          workList.push_back(I->getNode());
    }
  }
}

//===----------------------------------------------------------------------===//
// Helper method implementations...
//


///
/// getValueDest - Return the DSNode that the actual value points to.
///
DSNodeHandle GraphBuilder::getValueDest(Value &Val) {
  Value *V = &Val;
  if (isa<Constant>(V) && cast<Constant>(V)->isNullValue()) 
    return 0;  // Null doesn't point to anything, don't add to ScalarMap!

  DSNodeHandle &NH = G.getNodeForValue(V);
  if (!NH.isNull())
    return NH;     // Already have a node?  Just return it...

  // Otherwise we need to create a new node to point to.
  // Check first for constant expressions that must be traversed to
  // extract the actual value.
  DSNode* N;
  if (GlobalVariable* GV = dyn_cast<GlobalVariable>(V)) {
    // Create a new global node for this global variable.
    N = createNode(GV->getType()->getElementType());
    N->addGlobal(GV);
  } else if (Function* GV = dyn_cast<Function>(V)) {
    // Create a new global node for this global variable.
    N = createNode();
    N->addGlobal(GV);
  } else if (Constant *C = dyn_cast<Constant>(V)) {
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
      if (CE->isCast()) {
        if (isa<PointerType>(CE->getOperand(0)->getType()))
          NH = getValueDest(*CE->getOperand(0));
        else
          NH = createNode()->setUnknownMarker();
      } else if (CE->getOpcode() == Instruction::GetElementPtr) {
        visitGetElementPtrInst(*CE);
        assert(G.hasNodeForValue(CE) && "GEP didn't get processed right?");
        NH = G.getNodeForValue(CE);
      } else {
        // This returns a conservative unknown node for any unhandled ConstExpr
        return NH = createNode()->setUnknownMarker();
      }
      if (NH.isNull()) {  // (getelementptr null, X) returns null
        G.eraseNodeForValue(V);
        return 0;
      }
      return NH;
    } else if (isa<UndefValue>(C)) {
      G.eraseNodeForValue(V);
      return 0;
    } else if (isa<GlobalAlias>(C)) {
      // XXX: Need more investigation
      // According to Andrew, DSA is broken on global aliasing, since it does
      // not handle the aliases of parameters correctly. Here is only a quick
      // fix for some special cases.
      NH = getValueDest(*(cast<GlobalAlias>(C)->getAliasee()));
      return 0;
    } else {
      DEBUG(errs() << "Unknown constant: " << *C << "\n");
      assert(0 && "Unknown constant type!");
    }
    N = createNode(); // just create a shadow node
  } else {
    // Otherwise just create a shadow node
    N = createNode();
  }

  NH.setTo(N, 0);      // Remember that we are pointing to it...
  return NH;
}


/// getLink - This method is used to return the specified link in the
/// specified node if one exists.  If a link does not already exist (it's
/// null), then we create a new node, link it, then return it.  We must
/// specify the type of the Node field we are accessing so that we know what
/// type should be linked to if we need to create a new node.
///
DSNodeHandle &GraphBuilder::getLink(const DSNodeHandle &node, unsigned LinkNo) {
  DSNodeHandle &Node = const_cast<DSNodeHandle&>(node);
  DSNodeHandle &Link = Node.getLink(LinkNo);
  if (Link.isNull()) {
    // If the link hasn't been created yet, make and return a new shadow node
    Link = createNode();
  }
  return Link;
}


/// setDestTo - Set the ScalarMap entry for the specified value to point to the
/// specified destination.  If the Value already points to a node, make sure to
/// merge the two destinations together.
///
void GraphBuilder::setDestTo(Value &V, const DSNodeHandle &NH) {
  G.getNodeForValue(&V).mergeWith(NH);
}


//===----------------------------------------------------------------------===//
// Specific instruction type handler implementations...
//

// PHINode - Make the scalar for the PHI node point to all of the things the
// incoming values point to... which effectively causes them to be merged.
//
void GraphBuilder::visitPHINode(PHINode &PN) {
  if (!isa<PointerType>(PN.getType())) return; // Only pointer PHIs

  DSNodeHandle &PNDest = G.getNodeForValue(&PN);
  for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i)
    PNDest.mergeWith(getValueDest(*PN.getIncomingValue(i)));
}

void GraphBuilder::visitSelectInst(SelectInst &SI) {
  if (!isa<PointerType>(SI.getType()))
    return; // Only pointer Selects

  DSNodeHandle &Dest = G.getNodeForValue(&SI);
  DSNodeHandle S1 = getValueDest(*SI.getOperand(1));
  DSNodeHandle S2 = getValueDest(*SI.getOperand(2));
  Dest.mergeWith(S1);
  Dest.mergeWith(S2);
}

void GraphBuilder::visitLoadInst(LoadInst &LI) {
  DSNodeHandle Ptr = getValueDest(*LI.getOperand(0));

  if (Ptr.isNull()) return; // Load from null

  // Make that the node is read from...
  Ptr.getNode()->setReadMarker();

  // Ensure a typerecord exists...
  Ptr.getNode()->mergeTypeInfo(LI.getType(), Ptr.getOffset(), false);

  if (isa<PointerType>(LI.getType()))
    setDestTo(LI, getLink(Ptr));
}

void GraphBuilder::visitStoreInst(StoreInst &SI) {
  Type *StoredTy = SI.getOperand(0)->getType();
  DSNodeHandle Dest = getValueDest(*SI.getOperand(1));
  if (Dest.isNull()) return;

  // Mark that the node is written to...
  Dest.getNode()->setModifiedMarker();

  // Ensure a type-record exists...
  Dest.getNode()->mergeTypeInfo(StoredTy, Dest.getOffset());

  // Avoid adding edges from null, or processing non-"pointer" stores
  if (isa<PointerType>(StoredTy))
    Dest.addEdgeTo(getValueDest(*SI.getOperand(0)));
}

void GraphBuilder::visitReturnInst(ReturnInst &RI) {
  if (RI.getNumOperands() && isa<PointerType>(RI.getOperand(0)->getType()))
    G.getOrCreateReturnNodeFor(*FB).mergeWith(getValueDest(*RI.getOperand(0)));
}

void GraphBuilder::visitVAArgInst(VAArgInst &I) {
  //FIXME: also updates the argument
  DSNodeHandle Ptr = getValueDest(*I.getOperand(0));
  if (Ptr.isNull()) return;

  // Make that the node is read and written
  Ptr.getNode()->setReadMarker()->setModifiedMarker();

  // Ensure a type record exists.
  DSNode *PtrN = Ptr.getNode();
  PtrN->mergeTypeInfo(I.getType(), Ptr.getOffset(), false);

  if (isa<PointerType>(I.getType()))
    setDestTo(I, getLink(Ptr));
}

void GraphBuilder::visitIntToPtrInst(IntToPtrInst &I) {
//  std::cerr << "cast in " << I.getParent()->getParent()->getName() << "\n";
//  I.dump();
  setDestTo(I, createNode()->setUnknownMarker()->setIntToPtrMarker()); 
}

void GraphBuilder::visitPtrToIntInst(PtrToIntInst& I) {
  if (DSNode* N = getValueDest(*I.getOperand(0)).getNode())
    N->setPtrToIntMarker();
}


void GraphBuilder::visitBitCastInst(BitCastInst &I) {
  if (!isa<PointerType>(I.getType())) return; // Only pointers
  DSNodeHandle Ptr = getValueDest(*I.getOperand(0));
  if (Ptr.isNull()) return;
  setDestTo(I, Ptr);
}

void GraphBuilder::visitCmpInst(CmpInst &I) {
  //Should this merge or not?  I don't think so.
}

void GraphBuilder::visitGetElementPtrInst(User &GEP) {
  DSNodeHandle Value = getValueDest(*GEP.getOperand(0));
  if (Value.isNull())
    Value = createNode();

  // As a special case, if all of the index operands of GEP are constant zeros,
  // handle this just like we handle casts (ie, don't do much).
  bool AllZeros = true;
  for (unsigned i = 1, e = GEP.getNumOperands(); i != e; ++i) {
    if (ConstantInt * CI = dyn_cast<ConstantInt>(GEP.getOperand(i)))
      if (CI->isZero()) {
        continue;
      }
    AllZeros = false;
    break;
  }

  // If all of the indices are zero, the result points to the operand without
  // applying the type.
  if (AllZeros || (!Value.isNull() &&
                   Value.getNode()->isNodeCompletelyFolded())) {
    setDestTo(GEP, Value);
    return;
  }


  const PointerType *PTy = cast<PointerType>(GEP.getOperand(0)->getType());
  Type *CurTy = PTy->getElementType();

  if (Value.getNode()->mergeTypeInfo(CurTy, Value.getOffset())) {
    // If the node had to be folded... exit quickly
    setDestTo(GEP, Value);  // GEP result points to folded node

    return;
  }

  const DataLayout &TD = Value.getNode()->getDataLayout();

#if 0
  // Handle the pointer index specially...
  if (GEP.getNumOperands() > 1 &&
      (!isa<Constant>(GEP.getOperand(1)) ||
       !cast<Constant>(GEP.getOperand(1))->isNullValue())) {

    // If we already know this is an array being accessed, don't do anything...
    if (!TopTypeRec.isArray) {
      TopTypeRec.isArray = true;

      // If we are treating some inner field pointer as an array, fold the node
      // up because we cannot handle it right.  This can come because of
      // something like this:  &((&Pt->X)[1]) == &Pt->Y
      //
      if (Value.getOffset()) {
        // Value is now the pointer we want to GEP to be...
        Value.getNode()->foldNodeCompletely();
        setDestTo(GEP, Value);  // GEP result points to folded node
        return;
      } else {
        // This is a pointer to the first byte of the node.  Make sure that we
        // are pointing to the outter most type in the node.
        // FIXME: We need to check one more case here...
      }
    }
  }
#endif

  // All of these subscripts are indexing INTO the elements we have...
  unsigned Offset = 0;
  for (gep_type_iterator I = gep_type_begin(GEP), E = gep_type_end(GEP);
       I != E; ++I)
    if (StructType *STy = dyn_cast<StructType>(*I)) {
      const ConstantInt* CUI = cast<ConstantInt>(I.getOperand());
#if 0
      unsigned FieldNo = 
        CUI->getType()->isSigned() ? CUI->getSExtValue() : CUI->getZExtValue();
#else
      int FieldNo = CUI->getSExtValue();
#endif
      Offset += (unsigned)TD.getStructLayout(STy)->getElementOffset(FieldNo);
    } else if (isa<PointerType>(*I)) {
      if (!isa<Constant>(I.getOperand()) ||
          !cast<Constant>(I.getOperand())->isNullValue())
        Value.getNode()->setArrayMarker();
    }


#if 0
    if (const SequentialType *STy = cast<SequentialType>(*I)) {
      CurTy = STy->getElementType();
      if (ConstantInt *CS = dyn_cast<ConstantInt>(GEP.getOperand(i))) {
        Offset += 
          (CS->getType()->isSigned() ? CS->getSExtValue() : CS->getZExtValue())
          * TD.getTypeAllocSize(CurTy);
      } else {
        // Variable index into a node.  We must merge all of the elements of the
        // sequential type here.
        if (isa<PointerType>(STy)) {
          DEBUG(errs() << "Pointer indexing not handled yet!\n");
	} else {
          const ArrayType *ATy = cast<ArrayType>(STy);
          unsigned ElSize = TD.getTypeAllocSize(CurTy);
          DSNode *N = Value.getNode();
          assert(N && "Value must have a node!");
          unsigned RawOffset = Offset+Value.getOffset();

          // Loop over all of the elements of the array, merging them into the
          // zeroth element.
          for (unsigned i = 1, e = ATy->getNumElements(); i != e; ++i)
            // Merge all of the byte components of this array element
            for (unsigned j = 0; j != ElSize; ++j)
              N->mergeIndexes(RawOffset+j, RawOffset+i*ElSize+j);
        }
      }
    }
#endif

  // Add in the offset calculated...
  Value.setOffset(Value.getOffset()+Offset);

  // Check the offset
  DSNode *N = Value.getNode();
  if (N &&
      !N->isNodeCompletelyFolded() &&
      (N->getSize() != 0 || Offset != 0) &&
      !N->isForwarding()) {
    if ((Offset >= N->getSize()) || int(Offset) < 0) {
      // Accessing offsets out of node size range
      // This is seen in the "magic" struct in named (from bind), where the
      // fourth field is an array of length 0, presumably used to create struct
      // instances of different sizes

      // Collapse the node since its size is now variable
      N->foldNodeCompletely();
    }
  }

  // Value is now the pointer we want to GEP to be...  
  setDestTo(GEP, Value);
#if 0
  if (debug && (isa<Instruction>(GEP))) {
    Instruction * IGEP = (Instruction *)(&GEP);
    DSNode * N = Value.getNode();
    if (IGEP->getParent()->getParent()->getName() == "alloc_vfsmnt")
    {
      if (G.getPoolDescriptorsMap().count(N) != 0)
        if (G.getPoolDescriptorsMap()[N]) {
	  DEBUG(errs() << "LLVA: GEP[" << 0 << "]: Pool for " << GEP.getName() << " is " << G.getPoolDescriptorsMap()[N]->getName() << "\n");
	}
    }
  }
#endif

}


void GraphBuilder::visitCallInst(CallInst &CI) {
  visitCallSite(&CI);
}

void GraphBuilder::visitInvokeInst(InvokeInst &II) {
  visitCallSite(&II);
}

/// returns true if the intrinsic is handled
bool GraphBuilder::visitIntrinsic(CallSite CS, Function *F) {
  switch (F->getIntrinsicID()) {
  case Intrinsic::vastart: {
    // Mark the memory written by the vastart intrinsic as incomplete
    DSNodeHandle RetNH = getValueDest(**CS.arg_begin());
    if (DSNode *N = RetNH.getNode()) {
      N->setModifiedMarker()->setAllocaMarker()->setIncompleteMarker()
       ->setVAStartMarker()->setUnknownMarker()->foldNodeCompletely();
    }

    if (RetNH.hasLink(0)) {
      DSNodeHandle Link = RetNH.getLink(0);
      if (DSNode *N = Link.getNode()) {
        N->setModifiedMarker()->setAllocaMarker()->setIncompleteMarker()
         ->setVAStartMarker()->setUnknownMarker()->foldNodeCompletely();
      }
    } else {
      //
      // Sometimes the argument to the vastart is casted and has no DSNode.
      // Peer past the cast.
      //
      Value * Operand = CS.getInstruction()->getOperand(1);
      if (CastInst * CI = dyn_cast<CastInst>(Operand))
        Operand = CI->getOperand (0);
      RetNH = getValueDest(*Operand);
      if (DSNode *N = RetNH.getNode()) {
        N->setModifiedMarker()->setAllocaMarker()->setIncompleteMarker()
         ->setVAStartMarker()->setUnknownMarker()->foldNodeCompletely();
      }
    }

    return true;
  }
  case Intrinsic::vacopy:
    getValueDest(*CS.getInstruction()).
      mergeWith(getValueDest(**(CS.arg_begin())));
    return true;
  case Intrinsic::stacksave: {
    DSNode * Node = createNode();
    Node->setAllocaMarker()->setIncompleteMarker()->setUnknownMarker();
    Node->foldNodeCompletely();
    setDestTo (*(CS.getInstruction()), Node);
    return true;
  }
  case Intrinsic::stackrestore:
    getValueDest(*CS.getInstruction()).getNode()->setAllocaMarker()
                                                ->setIncompleteMarker()
                                                ->setUnknownMarker()
                                                ->foldNodeCompletely();
    return true;
  case Intrinsic::vaend:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_declare:
    return true;  // noop
  case Intrinsic::memcpy: 
  case Intrinsic::memmove: {
    // Merge the first & second arguments, and mark the memory read and
    // modified.
    DSNodeHandle RetNH = getValueDest(**CS.arg_begin());
    RetNH.mergeWith(getValueDest(**(CS.arg_begin()+1)));
    if (DSNode *N = RetNH.getNode())
      N->setModifiedMarker()->setReadMarker();
    return true;
  }
  case Intrinsic::memset:
    // Mark the memory modified.
    if (DSNode *N = getValueDest(**CS.arg_begin()).getNode())
      N->setModifiedMarker();
    return true;

#if 0
  case Intrinsic::eh_exception: {
    DSNode * Node = createNode();
    Node->setIncompleteMarker();
    Node->foldNodeCompletely();
    setDestTo (*(CS.getInstruction()), Node);
    return true;
  }

  case Intrinsic::atomic_cmp_swap: {
    DSNodeHandle Ptr = getValueDest(**CS.arg_begin());
    Ptr.getNode()->setReadMarker();
    Ptr.getNode()->setModifiedMarker();
    if (isa<PointerType>(F->getReturnType())) {
      setDestTo(*(CS.getInstruction()), getValueDest(**(CS.arg_begin() + 1)));
      getValueDest(**(CS.arg_begin() + 1))
        .mergeWith(getValueDest(**(CS.arg_begin() + 2)));
    }
  }
  case Intrinsic::atomic_swap:
  case Intrinsic::atomic_load_add:
  case Intrinsic::atomic_load_sub:
  case Intrinsic::atomic_load_and:
  case Intrinsic::atomic_load_nand:
  case Intrinsic::atomic_load_or:
  case Intrinsic::atomic_load_xor:
  case Intrinsic::atomic_load_max:
  case Intrinsic::atomic_load_min:
  case Intrinsic::atomic_load_umax:
  case Intrinsic::atomic_load_umin:
    {
      DSNodeHandle Ptr = getValueDest(**CS.arg_begin());
      Ptr.getNode()->setReadMarker();
      Ptr.getNode()->setModifiedMarker();
      if (isa<PointerType>(F->getReturnType()))
        setDestTo(*(CS.getInstruction()), getValueDest(**(CS.arg_begin() + 1)));
    }
  case Intrinsic::eh_selector:
#endif
  case Intrinsic::eh_typeid_for:
  case Intrinsic::prefetch:
    return true;

  //
  // The return address aliases with the stack, is type-unknown, and should
  // have the unknown flag set since we don't know where it goes.
  //
  case Intrinsic::returnaddress: {
    DSNode * Node = createNode();
    Node->setAllocaMarker()->setIncompleteMarker()->setUnknownMarker();
    Node->foldNodeCompletely();
    setDestTo (*(CS.getInstruction()), Node);
    return true;
  }

  default: {
    //ignore pointer free intrinsics
    if (!isa<PointerType>(F->getReturnType())) {
      bool hasPtr = false;
      for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
           I != E && !hasPtr; ++I)
        if (isa<PointerType>(I->getType()))
          hasPtr = true;
      if (!hasPtr)
        return true;
    }

    DEBUG(errs() << "[dsa:local] Unhandled intrinsic: " << F->getName() << "\n");
    assert(0 && "Unhandled intrinsic");
    return false;
  }
  }
}

void GraphBuilder::visitCallSite(CallSite CS) {
  Value *Callee = CS.getCalledValue();

  // Special case handling of certain libc allocation functions here.
  if (Function *F = dyn_cast<Function>(Callee))
    if (F->isDeclaration())
      if (F->isIntrinsic() && visitIntrinsic(CS, F))
        return;

  // Set up the return value...
  DSNodeHandle RetVal;
  Instruction *I = CS.getInstruction();
  if (isa<PointerType>(I->getType()))
    RetVal = getValueDest(*I);

  if (!isa<Function>(Callee))
    if (ConstantExpr* EX = dyn_cast<ConstantExpr>(Callee))
      if (EX->isCast() && isa<Function>(EX->getOperand(0)))
        Callee = cast<Function>(EX->getOperand(0));

  DSNode *CalleeNode = 0;
  if (!isa<Function>(Callee)) {
    CalleeNode = getValueDest(*Callee).getNode();
    if (CalleeNode == 0) {
      DEBUG(errs() << "WARNING: Program is calling through a null pointer?\n" << *I);
      return;  // Calling a null pointer?
    }
  }

  std::vector<DSNodeHandle> Args;
  Args.reserve(CS.arg_end()-CS.arg_begin());

  // Calculate the arguments vector...
  for (CallSite::arg_iterator I = CS.arg_begin(), E = CS.arg_end(); I != E; ++I)
    if (isa<PointerType>((*I)->getType()))
      Args.push_back(getValueDest(**I));

  // Add a new function call entry...
  if (CalleeNode) {
    G.getFunctionCalls().push_back(DSCallSite(CS, RetVal, CalleeNode, Args));
    DS->callee_site(CS.getInstruction());
  }else
    G.getFunctionCalls().push_back(DSCallSite(CS, RetVal, cast<Function>(Callee),
                                              Args));
}

// visitInstruction - For all other instruction types, if we have any arguments
// that are of pointer type, make them have unknown composition bits, and merge
// the nodes together.
void GraphBuilder::visitInstruction(Instruction &Inst) {
  DSNodeHandle CurNode;
  if (isa<PointerType>(Inst.getType()))
    CurNode = getValueDest(Inst);
  for (User::op_iterator I = Inst.op_begin(), E = Inst.op_end(); I != E; ++I)
    if (isa<PointerType>((*I)->getType()))
      CurNode.mergeWith(getValueDest(**I));

  if (DSNode *N = CurNode.getNode())
    N->setUnknownMarker();
}



//===----------------------------------------------------------------------===//
// LocalDataStructures Implementation
//===----------------------------------------------------------------------===//

// MergeConstantInitIntoNode - Merge the specified constant into the node
// pointed to by NH.
void GraphBuilder::MergeConstantInitIntoNode(DSNodeHandle &NH, Type* Ty, Constant *C) {
  // Ensure a type-record exists...
  DSNode *NHN = NH.getNode();
  NHN->mergeTypeInfo(Ty, NH.getOffset());

  if (isa<PointerType>(Ty)) {
    // Avoid adding edges from null, or processing non-"pointer" stores
    NH.addEdgeTo(getValueDest(*C));
    return;
  }

  if (Ty->isIntOrIntVectorTy() || Ty->isFPOrFPVectorTy()) return;

  const DataLayout &TD = NH.getNode()->getDataLayout();

  if (ConstantArray *CA = dyn_cast<ConstantArray>(C)) {
    for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i)
      // We don't currently do any indexing for arrays...
      MergeConstantInitIntoNode(NH, cast<ArrayType>(Ty)->getElementType(), cast<Constant>(CA->getOperand(i)));
  } else if (ConstantStruct *CS = dyn_cast<ConstantStruct>(C)) {
    const StructLayout *SL = TD.getStructLayout(cast<StructType>(Ty));
    for (unsigned i = 0, e = CS->getNumOperands(); i != e; ++i) {
      DSNode *NHN = NH.getNode();
      //Some programmers think ending a structure with a [0 x sbyte] is cute
      if (SL->getElementOffset(i) < SL->getSizeInBytes()) {
        DSNodeHandle NewNH(NHN, NH.getOffset()+(unsigned)SL->getElementOffset(i));
        MergeConstantInitIntoNode(NewNH, cast<StructType>(Ty)->getElementType(i), cast<Constant>(CS->getOperand(i)));
      } else if (SL->getElementOffset(i) == SL->getSizeInBytes()) {
        DOUT << "Zero size element at end of struct\n";
        NHN->foldNodeCompletely();
      } else {
        assert(0 && "type was smaller than offsets of of struct layout indicate");
      }
    }
  } else if (isa<ConstantAggregateZero>(C) || isa<UndefValue>(C)) {
    // Noop
  } else {
    assert(0 && "Unknown constant type!");
  }
}

void GraphBuilder::mergeInGlobalInitializer(GlobalVariable *GV) {
  assert(!GV->isDeclaration() && "Cannot merge in external global!");
  // Get a node handle to the global node and merge the initializer into it.
  DSNodeHandle NH = getValueDest(*GV);
  MergeConstantInitIntoNode(NH, GV->getType()->getElementType(), GV->getInitializer());
}

char LocalDataStructures::ID;

bool LocalDataStructures::runOnModule(Module &M) {
  init(&getAnalysis<DataLayout>());

  // First step, build the globals graph.
  {
    GraphBuilder GGB(*GlobalsGraph);

    // Add initializers for all of the globals to the globals graph.
    for (Module::global_iterator I = M.global_begin(), E = M.global_end();
         I != E; ++I)
      if (!I->isDeclaration())
        GGB.mergeInGlobalInitializer(I);
    // Add Functions to the globals graph.
    for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
      if (!I->isDeclaration())
        GGB.mergeFunction(*I);
  }

  // Next step, iterate through the nodes in the globals graph, unioning
  // together the globals into equivalence classes.
  formGlobalECs();

  // Calculate all of the graphs...
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isDeclaration()) {
      DSGraph* G = new DSGraph(GlobalECs, getDataLayout(), GlobalsGraph);
      GraphBuilder GGB(*I, *G, *this);
      G->getAuxFunctionCalls() = G->getFunctionCalls();
      setDSGraph(*I, G);
      propagateUnknownFlag(G);
    }

  GlobalsGraph->removeTriviallyDeadNodes();
  GlobalsGraph->markIncompleteNodes(DSGraph::MarkFormalArgs);

  // Now that we've computed all of the graphs, and merged all of the info into
  // the globals graph, see if we have further constrained the globals in the
  // program if so, update GlobalECs and remove the extraneous globals from the
  // program.
  formGlobalECs();

  propagateUnknownFlag(GlobalsGraph);
  return false;
}

