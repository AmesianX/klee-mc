#include "Executor.h"
#include "ExeStateManager.h"
#include "Sugar.h"

#include "InterleavedSearcher.h"

using namespace klee;

InterleavedSearcher::InterleavedSearcher(const std::vector<Searcher*> &_searchers)
: searchers(_searchers),
  index(1)
{
}

InterleavedSearcher::~InterleavedSearcher()
{
  foreach (it, searchers.begin(), searchers.end())
    delete *it;
}

ExecutionState &InterleavedSearcher::selectState(bool allowCompact)
{
  Searcher *s = searchers[--index];
  if (index == 0) index = searchers.size();
  ExecutionState* es = &s->selectState(allowCompact);
  return *es;
}

void InterleavedSearcher::update(ExecutionState *current,
        const ExeStateSet &addedStates,
        const ExeStateSet &removedStates,
        const ExeStateSet &ignoreStates,
        const ExeStateSet &unignoreStates)
{
  foreach(it, searchers.begin(), searchers.end()) 
    (*it)->update(current, 
      addedStates, removedStates, ignoreStates, unignoreStates);
}