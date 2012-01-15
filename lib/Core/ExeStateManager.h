/**
 * Cleanly manage states generated by executor
 */
#ifndef EXESTATEMANAGER_H
#define EXESTATEMANAGER_H
#include "klee/ExecutionState.h"
#include "Searcher.h"

namespace klee
{
class EquivalentStateEliminator;

typedef std::map<ExecutionState*, ExecutionState*> ExeStateReplaceMap;

class ExeStateManager
{
private:
  ExeStateSet states;
  ExeStateSet::size_type nonCompactStateCount;

  /// Used to track states that have been added during the current
  /// instructions step. 
  /// \invariant \ref addedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  ExeStateSet addedStates;
  /// Used to track states that have been removed during the current
  /// instructions step. 
  /// \invariant \ref removedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  ExeStateSet removedStates;

  // used when we need to remove yielded states + normal removed states
  mutable ExeStateSet allRemovedStates;

  /// Used to track states that have been replaced during the current
  /// instructions step. 
  /// \invariant \ref replacedStates is a subset of \ref states U addedStates. 
  ExeStateReplaceMap replacedStates;
  
  /* states to yield */
  ExeStateSet yieldStates;

  /* all yielded states */
  ExeStateSet yieldedStates;

  Searcher *searcher;

  Searcher::States getStates(void) const;
public:
  ExeStateManager();
  virtual ~ExeStateManager();
  void commitQueue(Executor* exe, ExecutionState* current);

  ExeStateSet::const_iterator begin(void) { return states.begin(); }
  ExeStateSet::const_iterator end(void) { return states.end(); }

  void dropAdded(ExecutionState* es);
  void queueAdd(ExecutionState* es);
  void queueRemove(ExecutionState* s);
  void yield(ExecutionState* s);

  void setInitialState(
    Executor* exe, ExecutionState* initialState, bool replay);
  void setWeights(double weight);
  void replaceState(ExecutionState* old_s, ExecutionState* new_s);
  void replaceStateImmediate(ExecutionState* old_s, ExecutionState* new_s);
  ExecutionState* getReplacedState(ExecutionState* s) const;

  void compactPressureStates(ExecutionState* &state, uint64_t maxMem);
  void compactStates(ExecutionState* &state, unsigned numToCompact);
  ExecutionState* compactState(ExecutionState* state);


  bool empty(void) const { return size() == 0; }
  unsigned int size(void) const { return states.size() + yieldedStates.size(); }
  unsigned int numRemovedStates(void) const { return removedStates.size(); }
  bool isRemovedState(ExecutionState* s) const;
  bool isAddedState(ExecutionState* s) const;
  ExecutionState* selectState(bool allowCompact);

  void teardownUserSearcher(void);
  void setupSearcher(Executor* exe);

  unsigned int getNonCompactStateCount(void) const { return nonCompactStateCount; }
};

struct KillOrCompactOrdering
// the least important state (the first to kill or compact) first
{
  // Ordering:
  // 1. States with coveredNew has greater importance
  // 2. States with more recent use has greater importance
  bool operator()(
    const ExecutionState* a, const ExecutionState* b) const
  // returns true if a is less important than b
  {
    // replayed states have lower priority than non-replay
    if (!a->isReplayDone() && b->isReplayDone()) return true;

    // state that covered new code should stick around
    if (!a->coveredNew &&  b->coveredNew) return true;
    if ( a->coveredNew && !b->coveredNew) return false;
    return a->lastChosen < b->lastChosen;
  }
};

}

#endif