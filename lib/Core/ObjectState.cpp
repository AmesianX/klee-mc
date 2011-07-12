#include "Context.h"
#include "klee/Common.h"
#include "klee/Expr.h"
#include "klee/Solver.h"
#include "klee/util/BitArray.h"

#include "CoreStats.h"
#include "Memory.h"
#include "MemoryManager.h"

#include "static/Sugar.h"
#include "klee/Common.h"
#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Solver.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Streams.h"

using namespace klee;
using namespace llvm;

namespace {
  cl::opt<bool>
  UseConstantArrays("use-constant-arrays",
                    cl::init(true));
}

ObjectState::ObjectState(const MemoryObject *mo)
: copyOnWriteOwner(0)
, refCount(0)
, object(mo)
, concreteStore(new uint8_t[mo->size])
, concreteMask(0)
, flushMask(0)
, knownSymbolics(0)
, updates(0, 0)
, size(mo->size)
, readOnly(false)
, wrseqno(0)
, wasSymOffObjectWrite(false)
{
	memset(concreteStore, 0, mo->size);

	if (UseConstantArrays) return;

	// FIXME: Leaked.
	static unsigned id = 0;
	const Array *array = new Array(
		"objst_tmp_arr" + llvm::utostr(++id),
		mo->mallocKey,
		0,
		0);
	src_array = array;
	updates = UpdateList(array, 0);
}


ObjectState::ObjectState(const MemoryObject *mo, const Array *array)
: src_array(array)
, copyOnWriteOwner(0)
, refCount(0)
, object(mo)
, concreteStore(new uint8_t[mo->size])
, concreteMask(0)
, flushMask(0)
, knownSymbolics(0)
, updates(array, 0)
, size(mo->size)
, readOnly(false)
, wrseqno(0)
, wasSymOffObjectWrite(false)
{
	memset(concreteStore, 0, mo->size);
	makeSymbolic();
}

ObjectState::ObjectState(const ObjectState &os)
: copyOnWriteOwner(0)
, refCount(0)
, object(os.object)
, concreteStore(new uint8_t[os.size])
, concreteMask(os.concreteMask ? new BitArray(*os.concreteMask, os.size) : 0)
, flushMask(os.flushMask ? new BitArray(*os.flushMask, os.size) : 0)
, knownSymbolics(0)
, updates(os.updates)
, size(os.size)
, readOnly(false)
, wrseqno(os.wrseqno)
, wasSymOffObjectWrite(os.wasSymOffObjectWrite)
{
  assert(!os.readOnly && "no need to copy read only object?");

  if (os.knownSymbolics) {
    knownSymbolics = new ref<Expr>[size];
    for (unsigned i=0; i<size; i++)
      knownSymbolics[i] = os.knownSymbolics[i];
  }

  memcpy(concreteStore, os.concreteStore, size*sizeof(*concreteStore));
}

ObjectState::~ObjectState()
{
  if (concreteMask) delete concreteMask;
  if (flushMask) delete flushMask;
  if (knownSymbolics) delete[] knownSymbolics;
  delete[] concreteStore;
}

/***/

const UpdateList &ObjectState::getUpdates() const
{
  // Constant arrays are created lazily.
  if (updates.root != NULL) return updates;

  // Collect the list of writes, with the oldest writes first.
   
  // FIXME: We should be able to do this more efficiently, we just need to be
  // careful to get the interaction with the cache right. In particular we
  // should avoid creating UpdateNode instances we never use.
  unsigned NumWrites = updates.head ? updates.head->getSize() : 0;
  std::vector< std::pair< ref<Expr>, ref<Expr> > > Writes(NumWrites);
  const UpdateNode *un = updates.head;
  for (unsigned i = NumWrites; i != 0; un = un->next) {
    --i;
    Writes[i] = std::make_pair(un->index, un->value);
  }

  std::vector< ref<ConstantExpr> > Contents(size);

  // Initialize to zeros.
  for (unsigned i = 0, e = size; i != e; ++i)
    Contents[i] = ConstantExpr::create(0, Expr::Int8);

  // Pull off as many concrete writes as we can.
  unsigned Begin = 0, End = Writes.size();
  for (; Begin != End; ++Begin) {
    // Push concrete writes into the constant array.
    ConstantExpr *Index = dyn_cast<ConstantExpr>(Writes[Begin].first);
    if (!Index)
      break;

    ConstantExpr *Value = dyn_cast<ConstantExpr>(Writes[Begin].second);
    if (!Value)
      break;

    Contents[Index->getZExtValue()] = Value;
  }

    // FIXME: We should unique these, there is no good reason to create multiple
    // ones.

    // Start a new update list.
    // FIXME: Leaked.
    static unsigned id = 0;
    const Array *array = new Array("const_arr" + llvm::utostr(++id),
                                   object->mallocKey, &Contents[0],
                                   &Contents[0] + Contents.size());
    array->initRef();
    updates = UpdateList(array, 0);

    // Apply the remaining (non-constant) writes.
    for (; Begin != End; ++Begin)
      updates.extend(Writes[Begin].first, Writes[Begin].second);

  return updates;
}

void ObjectState::makeConcrete() {
  if (concreteMask) delete concreteMask;
  if (flushMask) delete flushMask;
  if (knownSymbolics) delete[] knownSymbolics;
  concreteMask = 0;
  flushMask = 0;
  knownSymbolics = 0;
}

void ObjectState::markRangeSymbolic(unsigned offset, unsigned len)
{
  assert (len+offset < size && "Bad range");
  for (unsigned i=0; i<len; i++) {
    markByteSymbolic(i+offset);
    setKnownSymbolic(i+offset, 0);
    markByteFlushed(i+offset);
  }
}

void ObjectState::makeSymbolic() {
  assert(!updates.head &&
         "XXX makeSymbolic of objects with symbolic values is unsupported");

  // XXX simplify this, can just delete various arrays I guess
  for (unsigned i=0; i<size; i++) {
    markByteSymbolic(i);
    setKnownSymbolic(i, 0);
    markByteFlushed(i);
  }
}

void ObjectState::initializeToZero()
{
  makeConcrete();
  memset(concreteStore, 0, size);
}

void ObjectState::initializeToRandom()
{
  makeConcrete();
  for (unsigned i=0; i<size; i++) {
    // randomly selected by 256 sided die
    concreteStore[i] = 0xAB;
  }
}

/*
Cache Invariants
--
isByteKnownSymbolic(i) => !isByteConcrete(i)
isByteConcrete(i) => !isByteKnownSymbolic(i)
!isByteFlushed(i) => (isByteConcrete(i) || isByteKnownSymbolic(i))
 */

void ObjectState::fastRangeCheckOffset(ref<Expr> offset,
                                       unsigned *base_r,
                                       unsigned *size_r) const {
  *base_r = 0;
  *size_r = size;
}

void ObjectState::flushRangeForRead(unsigned rangeBase,
                                    unsigned rangeSize) const
{
  if (!flushMask) flushMask = new BitArray(size, true);

  for (unsigned offset=rangeBase; offset<rangeBase+rangeSize; offset++) {
    if (isByteFlushed(offset)) continue;

    if (isByteConcrete(offset)) {
      updates.extend(ConstantExpr::create(offset, Expr::Int32),
                     ConstantExpr::create(concreteStore[offset], Expr::Int8));
    } else {
      assert(isByteKnownSymbolic(offset) && "invalid bit set in flushMask");
      updates.extend(ConstantExpr::create(offset, Expr::Int32),
                     knownSymbolics[offset]);
    }
    flushMask->unset(offset);
  }
}

void ObjectState::flushRangeForWrite(unsigned rangeBase,
                                     unsigned rangeSize) {
  if (!flushMask) flushMask = new BitArray(size, true);

  for (unsigned offset=rangeBase; offset<rangeBase+rangeSize; offset++) {
    if (!isByteFlushed(offset)) {
      if (isByteConcrete(offset)) {
        updates.extend(ConstantExpr::create(offset, Expr::Int32),
                       ConstantExpr::create(concreteStore[offset], Expr::Int8));
        markByteSymbolic(offset);
      } else {
        assert(isByteKnownSymbolic(offset) && "invalid bit set in flushMask");
        updates.extend(ConstantExpr::create(offset, Expr::Int32),
                       knownSymbolics[offset]);
        setKnownSymbolic(offset, 0);
      }

      flushMask->unset(offset);
    } else {
      // flushed bytes that are written over still need
      // to be marked out
      if (isByteConcrete(offset)) {
        markByteSymbolic(offset);
      } else if (isByteKnownSymbolic(offset)) {
        setKnownSymbolic(offset, 0);
      }
    }
  }
}

bool ObjectState::isByteConcrete(unsigned offset) const {
  return !concreteMask || concreteMask->get(offset);
}

bool ObjectState::isByteFlushed(unsigned offset) const {
  return flushMask && !flushMask->get(offset);
}

bool ObjectState::isByteKnownSymbolic(unsigned offset) const {
  return knownSymbolics && knownSymbolics[offset].get();
}

void ObjectState::markByteConcrete(unsigned offset) {
  if (concreteMask)
    concreteMask->set(offset);
}

void ObjectState::markByteSymbolic(unsigned offset) {
  if (!concreteMask)
    concreteMask = new BitArray(size, true);
  concreteMask->unset(offset);
}

void ObjectState::markByteUnflushed(unsigned offset) {
  if (flushMask)
    flushMask->set(offset);
}

void ObjectState::markByteFlushed(unsigned offset) {
  if (!flushMask) {
    flushMask = new BitArray(size, false);
  } else {
    flushMask->unset(offset);
  }
}

void ObjectState::setKnownSymbolic(unsigned offset,
                                   Expr *value /* can be null */) {
  if (knownSymbolics) {
    knownSymbolics[offset] = value;
  } else {
    if (value) {
      knownSymbolics = new ref<Expr>[size];
      knownSymbolics[offset] = value;
    }
  }
}


static bool expequals(const ref<Expr>& exp1, const ref<Expr>& exp2) {

    if ((exp1.isNull() && !exp2.isNull()) || (!exp1.isNull() && exp2.isNull())) {
        return false;
    }

    if (exp1.isNull() && exp2.isNull()) {
        return true;
    }

    return exp1->compare(*exp2.get()) == 0;
}

bool ObjectState::equals(const ObjectState* o1) const {
  //TODO: get rid of this function. it's wrong. should only be neccessary when sym offs
    const ObjectState* o2 = this;
assert(o1);
  assert(o2);
    unsigned o1size = o1->size;
    unsigned o2size = o2->size;
    assert(o1size);
    assert(o2size);
    if (o1size != o2size) {
        return false;
    }

    bool fullcheck = false;
    unsigned i;
    for (i = 0; i < o1->size; i++) {
        if (o1->isByteConcrete(i) && o2->isByteConcrete(i)) {
            if (o1->concreteStore[i] != o2->concreteStore[i]) {
                return false;
            }
        } else if (o1->isByteKnownSymbolic(i) && o2->isByteKnownSymbolic(i)) {
            ref<Expr> exp1 = o1->read8(i);
            ref<Expr> exp2 = o2->read8(i);

            if (!expequals(exp1, exp2)) {
                return false;
            }
        } else {
            fullcheck = true;
            break;
        }
    }

    if (o1->getUpdates().getSize() == 0 && o2->getUpdates().getSize() == 0 && !fullcheck) {
        return true;
    }

    ref<Expr> exp1 = o1->read(0, o1->size * 8);
    ref<Expr> exp2 = o2->read(0, o2->size * 8);

    return expequals(exp1, exp2);
}

/***/

ref<Expr> ObjectState::read8(unsigned offset) const
{

  if (isByteConcrete(offset)) {
    return ConstantExpr::create(concreteStore[offset], Expr::Int8);
  }
  if (isByteKnownSymbolic(offset)) {
    return knownSymbolics[offset];
  }


  assert(isByteFlushed(offset) && "unflushed byte without cache value");
  return ReadExpr::create(
    getUpdates(), ConstantExpr::create(offset, Expr::Int32));
}

ref<Expr> ObjectState::read8(ref<Expr> offset) const
{
  assert(!isa<ConstantExpr>(offset) && "constant offset passed to symbolic read8");

  unsigned base, size;
  fastRangeCheckOffset(offset, &base, &size);
  flushRangeForRead(base, size);

  if (size>4096) {
    std::string allocInfo;
    object->getAllocInfo(allocInfo);
    klee_warning_once(0, "flushing %d bytes on read, may be slow and/or crash: %s",
                      size,
                      allocInfo.c_str());
  }
 
  return ReadExpr::create(getUpdates(), ZExtExpr::create(offset, Expr::Int32));
}

void ObjectState::write8(unsigned offset, uint8_t value) {

  //assert(read_only == false && "writing to read-only object!");
  concreteStore[offset] = value;
  setKnownSymbolic(offset, 0);

  markByteConcrete(offset);
  markByteUnflushed(offset);
}

void ObjectState::write8(unsigned offset, ref<Expr> value) {
  // can happen when ExtractExpr special cases 
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(value)) {
    write8(offset, (uint8_t) CE->getZExtValue(8));
  } else {
    setKnownSymbolic(offset, value.get());
     
    markByteSymbolic(offset);
    markByteUnflushed(offset);   
  }
}

void ObjectState::write8(ref<Expr> offset, ref<Expr> value) {
  assert(!isa<ConstantExpr>(offset) && "constant offset passed to symbolic write8");
  unsigned base, size;
  fastRangeCheckOffset(offset, &base, &size);
  flushRangeForWrite(base, size);

  if (size>4096) {
    std::string allocInfo;
    object->getAllocInfo(allocInfo);
    klee_warning_once(0, "flushing %d bytes on write, may be slow and/or crash: %s",
                      size,
                      allocInfo.c_str());
  }

  wasSymOffObjectWrite = true;
  updates.extend(ZExtExpr::create(offset, Expr::Int32), value);
}

/***/

ref<Expr> ObjectState::read(ref<Expr> offset, Expr::Width width) const
{
  // Truncate offset to 32-bits.
  offset = ZExtExpr::create(offset, Expr::Int32);

  // Check for reads at constant offsets.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset))
    return read(CE->getZExtValue(32), width);

  // Treat bool specially, it is the only non-byte sized write we allow.
  if (width == Expr::Bool)
    return ExtractExpr::create(read8(offset), 0, Expr::Bool);

  // Otherwise, follow the slow general case.
  unsigned NumBytes = width / 8;
  assert(width == NumBytes * 8 && "Invalid write size!");
  ref<Expr> Res(0);
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    ref<Expr> Byte = read8(
        AddExpr::create(
          offset,
          ConstantExpr::create(
            idx, Expr::Int32)));
    Res = i ? ConcatExpr::create(Byte, Res) : Byte;
  }

  return Res;
}

ref<Expr> ObjectState::read(unsigned offset, Expr::Width width) const
{
  // Treat bool specially, it is the only non-byte sized write we allow.
  if (width == Expr::Bool)
    return ExtractExpr::create(read8(offset), 0, Expr::Bool);

  // Otherwise, follow the slow general case.
  unsigned NumBytes = width / 8;
  assert(width == NumBytes * 8 && "Invalid write size!");
  ref<Expr> Res(0);
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    ref<Expr> Byte = read8(offset + idx);
    Res = i ? ConcatExpr::create(Byte, Res) : Byte;
  }

  return Res;
}

void ObjectState::write(ref<Expr> offset, ref<Expr> value) {
  // Truncate offset to 32-bits.
  offset = ZExtExpr::create(offset, Expr::Int32);

  // Check for writes at constant offsets.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset)) {
    write(CE->getZExtValue(32), value);
    return;
  }

  // Treat bool specially, it is the only non-byte sized write we allow.
  Expr::Width w = value->getWidth();
  if (w == Expr::Bool) {
    write8(offset, ZExtExpr::create(value, Expr::Int8));
    return;
  }

  // Otherwise, follow the slow general case.
  unsigned NumBytes = w / 8;
  assert(w == NumBytes * 8 && "Invalid write size!");
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(AddExpr::create(offset, ConstantExpr::create(idx, Expr::Int32)),
           ExtractExpr::create(value, 8 * i, Expr::Int8));
  }
}

void ObjectState::write(unsigned offset, ref<Expr> value) {
  // Check for writes of constant values.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(value)) {
    Expr::Width w = CE->getWidth();
    if (w <= 64) {
      uint64_t val = CE->getZExtValue();
      switch (w) {
      default: assert(0 && "Invalid write size!");
      case  Expr::Bool:
      case  Expr::Int8: write8(offset, val); return;
      case Expr::Int16: write16(offset, val); return;
      case Expr::Int32: write32(offset, val); return;
      case Expr::Int64: write64(offset, val); return;
      }
    }
  }

  // Treat bool specially, it is the only non-byte sized write we allow.
  Expr::Width w = value->getWidth();
  if (w == Expr::Bool) {
    write8(offset, ZExtExpr::create(value, Expr::Int8));
    return;
  }

  // Otherwise, follow the slow general case.
  unsigned NumBytes = w / 8;
  assert(w == NumBytes * 8 && "Invalid write size!");
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, ExtractExpr::create(value, 8 * i, Expr::Int8));
  }
}

void ObjectState::write16(unsigned offset, uint16_t value) {
  unsigned NumBytes = 2;
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, (uint8_t) (value >> (8 * i)));
  }
}

void ObjectState::write32(unsigned offset, uint32_t value) {
  unsigned NumBytes = 4;
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, (uint8_t) (value >> (8 * i)));
  }
}

void ObjectState::write64(unsigned offset, uint64_t value) {
  unsigned NumBytes = 8;
  for (unsigned i = 0; i != NumBytes; ++i) {
    unsigned idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
    write8(offset + idx, (uint8_t) (value >> (8 * i)));
  }
}

void ObjectState::print(unsigned int begin, int end) {
  unsigned int real_end;
  std::cerr << "-- ObjectState --\n";
  std::cerr << "\tMemoryObject ID: " << object->id << "\n";
  std::cerr << "\tRoot Object: " << updates.root << "\n";
  std::cerr << "\tSize: " << size << "\n";

  std::cerr << "\tBytes:\n";
  real_end = (end >= 0) ? end : size;

  for (unsigned i=begin; i < real_end; i++) {
    std::cerr << "\t\t["<<i<<"]"
               << " concrete? " << isByteConcrete(i)
               << " known-sym? " << isByteKnownSymbolic(i)
               << " flushed? " << isByteFlushed(i) << " = ";
    ref<Expr> e = read8(i);
    std::cerr << e << "\n";
  }

  std::cerr << "\tUpdates:\n";
  for (const UpdateNode *un=updates.head; un; un=un->next) {
    std::cerr << "\t\t[" << un->index << "] = " << un->value << "\n";
  }
}

unsigned int ObjectState::getNumConcrete(void) const
{
	unsigned int ret = 0;
	for (unsigned int i = 0; i < size; i++) {
		if (isByteConcrete(i)) ret++;
	}
	return ret;
}