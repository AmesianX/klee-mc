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

#include <llvm/Support/CommandLine.h>
#include "UnboxingObjectState.h"

using namespace klee;
using namespace llvm;

namespace {
  cl::opt<bool> UseZeroPage("opt-zero-page", cl::init(true));
}

unsigned ObjectState::numObjStates = 0;
std::unique_ptr<ObjectState> ObjectState::zero_page;
ObjectStateAlloc* ObjectState::os_alloc = NULL;


#ifdef KEEP_OBJLIST
ObjectState::objlist_ty	ObjectState::objs;
#define ADD_TO_LIST		\
	do {	numObjStates++;	\
		objs.push_front(this); objs_it = objs.begin(); } while (0)

#define RMV_FROM_LIST	do {numObjStates--; objs.erase(objs_it); } while (0)
#else
#define ADD_TO_LIST	numObjStates++
#define RMV_FROM_LIST	numObjStates--
#endif

ObjectState::ObjectState(unsigned _size)
: src_array(0)
, copyOnWriteOwner(0)
, refCount(0)
, copyDepth(0)
, concreteStore(std::make_unique<uint8_t[]>(_size))
, updates(0, 0)
, readOnly(false)
, size(_size)
{
	assert (size > 0);
	memset(concreteStore.get(), 0, size);
	ADD_TO_LIST;
}

ObjectState::ObjectState(unsigned _size, const ref<Array>& array)
: src_array(array)
, copyOnWriteOwner(0)
, refCount(0)
, copyDepth(0)
, concreteStore(std::make_unique<uint8_t[]>(_size))
, updates(array, 0)
, readOnly(false)
, size(_size)
{
	assert (size > 0);
	memset(concreteStore.get(), 0, size);
	makeSymbolic();
	ADD_TO_LIST;
}

ObjectState::ObjectState(const ObjectState &os)
: src_array(os.src_array)
, copyOnWriteOwner(0)
, refCount(0)
, copyDepth(os.copyDepth+1)
, concreteStore(std::make_unique<uint8_t[]>(os.size))
, concreteMask(os.concreteMask
	? std::make_unique<BitArray>(*os.concreteMask, os.size)
	: nullptr)
, flushMask(os.flushMask
	? std::make_unique<BitArray>(*os.flushMask, os.size)
	: nullptr)
, updates(os.updates)
, readOnly(false)
, size(os.size)
{
	assert(!os.readOnly && "no need to copy read only object?");
	assert (size > 0);

	/* XXX: it would be wise to do this for the parent os,
	 * but it can't be done here. */
	revertToConcrete();

	if (concreteMask != NULL && os.knownSymbolics) {
		knownSymbolics = std::make_unique<ref<Expr>[]>(size);
		for (unsigned i=0; i<size; i++)
			knownSymbolics[i] = os.knownSymbolics[i];
	}

	memcpy(	concreteStore.get(),
		os.concreteStore.get(),
		size * sizeof(concreteStore[0]));
	ADD_TO_LIST;
}

ObjectState::~ObjectState()
{
	assert (!isZeroPage() || (size && this == zero_page.get()));
	RMV_FROM_LIST;
}

const UpdateList &ObjectState::getUpdates() const
{
	// Constant arrays are created lazily.
	if (updates.getRoot().isNull()) buildUpdates();
	return updates;
}

// Collect the list of writes, with the oldest writes first.
//
// FIXME: We should be able to do this more efficiently,
// we just need to be careful to get the interaction with
// the cache right. In particular we should avoid creating
// UpdateNode instances we never use.
void ObjectState::buildUpdates(void) const
{
	unsigned NumWrites = updates.head ? updates.head->getSize() : 0;
	std::vector<std::pair<ref<Expr>, ref<Expr>>> Writes(NumWrites);
	const UpdateNode		*un = updates.head;
	std::vector<ref<ConstantExpr>>	Contents(size, MK_CONST(0, 8));
	std::map<unsigned, ref<Expr>>	concWrites;

	// Collect writes, [0] being first, [n] being last
	for (unsigned i = NumWrites; i != 0; un = un->next) {
		--i;
		Writes[i] = std::make_pair(un->index, un->value);
	}

	// Pull off as many concrete writes as we can.
	unsigned Begin = 0, End = Writes.size();
	for (; Begin != End; ++Begin) {
		auto idx = dyn_cast<ConstantExpr>(Writes[Begin].first);
		if (!idx) break;
		concWrites[idx->getZExtValue()] = Writes[Begin].second;
	}

	// Push concrete writes into the constant array.
	for (const auto &p : concWrites) {
		auto ce = dyn_cast<ConstantExpr>(p.second);
		if (!ce) continue;
		Contents[p.first] = ce;
	}

	// There is no good reason to create multiple ones;
	// start a new update list.
	static unsigned id = 0;
	ref<Array>	array;

	array = Array::create(
		"const_osa" + llvm::utostr(++id),
		MallocKey(size),
		&Contents[0],
		&Contents[0] + Contents.size());
	array = Array::uniqueArray(array);
	updates = UpdateList(array, 0);

	// Apply the remaining (non-constant) writes.
	for (const auto &p : concWrites) {
		if (dyn_cast<ConstantExpr>(p.second))
			continue;
		updates.extend(MK_CONST(p.first, 32), p.second);
	}

	for (; Begin != End; ++Begin) {
		updates.extend(Writes[Begin].first, Writes[Begin].second);
	}
}

void ObjectState::makeConcrete()
{
	concreteMask = nullptr;
	flushMask = nullptr;
	knownSymbolics = nullptr;
}

void ObjectState::markRangeSymbolic(unsigned offset, unsigned len)
{
	assert (!isZeroPage());

	assert (len+offset < size && "Bad range");
	for (unsigned i=0; i<len; i++) {
		markByteSymbolic(i+offset);
		setKnownSymbolic(i+offset, 0);
		markByteFlushed(i+offset);
	}
}

void ObjectState::makeSymbolic(void)
{
	assert (!isZeroPage());
	assert(!updates.head &&
         	"XXX makeSymbolic of objs with symbolic vals is unsupported");

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
	memset(concreteStore.get(), 0, size);
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

void ObjectState::fastRangeCheckOffset(
	ref<Expr> offset, unsigned *base_r, unsigned *size_r) const
{
	*base_r = 0;
	*size_r = size;
}

void ObjectState::flushRangeForRead(
	unsigned rangeBase, unsigned rangeSize) const
{
	if (!flushMask) flushMask = std::make_unique<BitArray>(size, true);

	for (unsigned offset=rangeBase; offset<rangeBase+rangeSize; offset++) {
		if (isByteFlushed(offset)) continue;

		if (isByteConcrete(offset)) {
			updates.extend(
				MK_CONST(offset, Expr::Int32),
				MK_CONST(concreteStore[offset], Expr::Int8));
		} else {
			assert(	isByteKnownSymbolic(offset) &&
				"invalid bit set in flushMask");
			updates.extend(
				MK_CONST(offset, Expr::Int32),
				knownSymbolics[offset]);
		}
		flushMask->unset(offset);
	}
}

void ObjectState::flushWriteByte(unsigned offset)
{
	assert (isByteFlushed(offset) == false);

	if (isByteConcrete(offset)) {
		updates.extend(
			MK_CONST(offset, Expr::Int32),
			MK_CONST(concreteStore[offset], Expr::Int8));
		markByteSymbolic(offset);
	} else {
		assert(	isByteKnownSymbolic(offset) &&
			"invalid bit set in flushMask");
		updates.extend(
			MK_CONST(offset, Expr::Int32), knownSymbolics[offset]);
		setKnownSymbolic(offset, 0);
	}

	flushMask->unset(offset);
}

void ObjectState::flushRangeForWrite(
	unsigned rangeBase, unsigned rangeSize)
{
	if (!flushMask)
		flushMask = std::make_unique<BitArray>(size, true);

	for (unsigned offset=rangeBase; offset<rangeBase+rangeSize; offset++) {
		if (!isByteFlushed(offset)) {
			flushWriteByte(offset);
			continue;
		}
		// flushed bytes that are written over still need
		// to be marked out
		if (isByteConcrete(offset)) {
			markByteSymbolic(offset);
		} else if (isByteKnownSymbolic(offset)) {
			setKnownSymbolic(offset, 0);
		}
	}
}

bool ObjectState::isByteConcrete(unsigned offset) const
{
	assert (offset < size);
	return (concreteMask == NULL || concreteMask->get(offset));
}

bool ObjectState::isByteFlushed(unsigned offset) const
{ return flushMask && !flushMask->get(offset); }

bool ObjectState::isByteKnownSymbolic(unsigned offset) const
{  return knownSymbolics && knownSymbolics[offset].get(); }

void ObjectState::markByteConcrete(unsigned offset)
{ if (concreteMask) concreteMask->set(offset); }

void ObjectState::markByteSymbolic(unsigned offset)
{
	if (concreteMask == NULL)
		concreteMask = std::make_unique<BitArray>(size, true);
	concreteMask->unset(offset);
}

void ObjectState::markByteUnflushed(unsigned offset)
{
	if (flushMask != NULL)
		flushMask->set(offset);
}

void ObjectState::markByteFlushed(unsigned offset)
{
	if (flushMask == NULL) {
		flushMask = std::make_unique<BitArray>(size, false);
	} else {
		flushMask->unset(offset);
	}
}

void ObjectState::setKnownSymbolic(
	unsigned offset,
	Expr *value /* can be null */)
{
	if (knownSymbolics != NULL) {
		knownSymbolics[offset] = value;
		return;
	}

	if (value == NULL)
		return;

	knownSymbolics = std::make_unique<ref<Expr>[]>(size);
	knownSymbolics[offset] = value;
}

uint8_t ObjectState::read8c(unsigned offset) const
{
	assert (isByteConcrete(offset));
	return concreteStore[offset];
}

ref<Expr> ObjectState::read8(unsigned offset) const
{
	if (isByteConcrete(offset))
		return MK_CONST(concreteStore[offset], Expr::Int8);

	if (isByteKnownSymbolic(offset))
		return knownSymbolics[offset];


	assert(isByteFlushed(offset) && "unflushed byte without cache value");
	return MK_READ(getUpdates(), MK_CONST(offset, Expr::Int32));
}

ref<Expr> ObjectState::read8(ref<Expr> offset) const
{
	unsigned base, size;

	assert(	!isa<ConstantExpr>(offset) &&
		"constant offset passed to symbolic read8");

	fastRangeCheckOffset(offset, &base, &size);
	flushRangeForRead(base, size);

	return MK_READ(getUpdates(), MK_ZEXT(offset, Expr::Int32));
}

void ObjectState::write8(unsigned offset, uint8_t value)
{
	assert(!readOnly  && "writing to read-only object!");
	assert(!isZeroPage());

	concreteStore[offset] = value;
	setKnownSymbolic(offset, 0);

	markByteConcrete(offset);
	markByteUnflushed(offset);
}

void ObjectState::write8(unsigned offset, ref<Expr>& value)
{
	assert (!isZeroPage());

	setKnownSymbolic(offset, value.get());

	markByteSymbolic(offset);
	markByteUnflushed(offset);
}

void ObjectState::write8(ref<Expr> offset, ref<Expr>& value)
{
	unsigned	base, size;

	assert (!isZeroPage());
	assert (!readOnly);
	assert(!isa<ConstantExpr>(offset) &&
		"constant offset passed to symbolic write8");

	fastRangeCheckOffset(offset, &base, &size);
	flushRangeForWrite(base, size);

	updates.extend(MK_ZEXT(offset, Expr::Int32), value);
}

ref<Expr> ObjectState::read(ref<Expr> offset, Expr::Width width) const
{
	// Truncate offset to 32-bits.
	offset = ZExtExpr::create(offset, Expr::Int32);

	// Check for reads at constant offsets.
	if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset))
		return read(CE->getZExtValue(32), width);

	// Treat bool specially,
	// it is the only non-byte sized write we allow.
	if (width == Expr::Bool)
		return MK_EXTRACT(read8(offset), 0, Expr::Bool);

	// Otherwise, follow the slow general case.
	return readSlow(offset, width);
}

ref<Expr> ObjectState::readSlow(ref<Expr>& offset, Expr::Width width) const
{
	ref<Expr>	Res(0);
	unsigned	NumBytes = width / 8;

	assert(width == NumBytes * 8 && "Invalid write size!");
	for (unsigned i = 0; i != NumBytes; ++i) {
		ref<Expr>	byte(0), cur_off;
		unsigned	idx;

		idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
		cur_off = MK_ADD(offset, MK_CONST(idx, Expr::Int32));
		if (cur_off->getKind() == Expr::Constant) {
			byte = read8(
				cast<ConstantExpr>(cur_off)->getZExtValue(32));
		} else
			byte = read8(cur_off);
		Res = i ? ConcatExpr::create(byte, Res) : byte;
	}

	return Res;
}

ref<Expr> ObjectState::readConstantBytes(
	unsigned offset, unsigned NumBytes) const
{
	uint64_t	ret = 0;

	assert (offset + NumBytes <= size);

	/* confirm all bytes in read are concrete */
	if (!isConcrete()) {
		/* page isn't concrete; scan read bytes. */
		for (unsigned i = 0; i < NumBytes; i++) {
			if (!isByteConcrete(offset + i)) {
				return NULL;
			}
		}
	}

	assert (concreteStore);
	for (unsigned i = 0; i < NumBytes; i++) {
		ret <<= 8;
		ret |= read8c(offset + ((NumBytes - 1) - i));
	}

	return MK_CONST(ret, NumBytes*8);
}

ref<Expr> ObjectState::read(unsigned offset, Expr::Width width) const
{
	// Treat bool specially; the only non-byte sized write we allow.
	if (width == Expr::Bool)
		return MK_EXTRACT(read8(offset), 0, Expr::Bool);

	// Otherwise, follow the slow general case.
	unsigned	NumBytes = width / 8;

	assert(width == NumBytes * 8 && "Non-byte aligned write size!");
	if (NumBytes <= 8) {
		ref<Expr>	ret(readConstantBytes(offset, NumBytes));
		if (!ret.isNull())
			return ret;
	}


	ref<Expr> Res(0);
	for (unsigned i = 0; i != NumBytes; ++i) {
		ref<Expr>	Byte(0);
		unsigned	idx;

		idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
		Byte = read8(offset + idx);
		Res = i ? ConcatExpr::create(Byte, Res) : Byte;
	}

	return Res;
}

void ObjectState::write(ref<Expr> offset, const ref<Expr>& value)
{
	assert (!readOnly);
	assert (!isZeroPage());

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
		ref<Expr>	v(ZExtExpr::create(value, Expr::Int8));
		write8(offset, v);
		return;
	}

	// Otherwise, follow the slow general case.
	unsigned NumBytes = w / 8;
	assert(w == NumBytes * 8 && "Invalid write size!");
	for (unsigned i = 0; i != NumBytes; ++i) {
		ref<Expr>	v(0), off(0);
		unsigned	idx;

		idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
		v = ExtractExpr::create(value, 8 * i, Expr::Int8);
		off = MK_ADD(offset, MK_CONST(idx, Expr::Int32));
		write8(off, v);
	}
}


void ObjectState::write(unsigned offset, const ref<Expr>& value)
{
	assert (!isZeroPage());

	// Treat bool specially, it is the only non-byte sized write we allow.
	Expr::Width w = value->getWidth();
	if (w == Expr::Bool) {
		ref<Expr>	v(ZExtExpr::create(value, Expr::Int8));
		write8(offset, v);
		return;
	}

	// Otherwise, follow the slow general case.
	unsigned NumBytes = w / 8;
	assert(w == NumBytes * 8 && "Invalid write size!");
	for (unsigned i = 0; i != NumBytes; ++i) {
		unsigned	idx;
		ref<Expr>	v(0);

		idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
		v = ExtractExpr::create(value, 8 * i, Expr::Int8);
		write8(offset + idx, v);
	}
}

bool ObjectState::writeIVC(unsigned offset, const ref<ConstantExpr>& ce)
{
	unsigned	w = ce->getWidth();
	unsigned	NumBytes = w/8;
	uint64_t	v;
	bool		updated;

	assert (!isZeroPage());
	assert ((w % 8) == 0 && "Expected byte write");

	v = ce->getZExtValue();
	updated = false;
	for (unsigned i = 0; i != NumBytes; ++i) {
		unsigned	idx, cur_off;
		uint8_t		v8;

		idx = Context::get().isLittleEndian() ? i : (NumBytes - i - 1);
		v8 = v >> (8 * i);
		cur_off = offset+idx;

		if (cur_off >= size)
			break;

		if (isByteConcrete(cur_off))
			continue;

		if (isByteKnownSymbolic(cur_off))
			continue;

		concreteStore[cur_off] = v8;
		markByteConcrete(cur_off);
		updated = true;
	}

	return updated;
}

void ObjectState::print(unsigned int begin, int end) const
{
  unsigned int real_end;
  std::cerr << "-- ObjectState --\n";
  std::cerr << "\tMemoryObject ID: XXX\n";
  std::cerr << "\tRoot Object: " << updates.getRoot().get() << "\n";
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

unsigned ObjectState::hash(void) const
{
	unsigned hash_ret;

	hash_ret = 0;
	if (getNumConcrete() != size) {
		getUpdates();
		flushRangeForRead(0, size);
		hash_ret = getUpdates().hash();
	}

	if (concreteStore) {
		for (unsigned int i = 0; i < size; i++) {
			hash_ret += (i+1)*concreteStore[i];
		}
	}

	if (knownSymbolics) {
		for (unsigned i = 0; i < size; i++) {
			if (knownSymbolics[i].isNull())
				continue;
			hash_ret += (i+1)*knownSymbolics[i]->hash();
		}
	}

	return hash_ret;
}

/* TODO: support more sizes */
void ObjectState::setupZeroObjs(void)
{
	if (zero_page) return;

	os_alloc = new ObjectStateFactory<UnboxingObjectState>();
	zero_page = std::unique_ptr<ObjectState>(new ObjectState(4096));
	zero_page->copyOnWriteOwner = COW_ZERO;
	zero_page->initializeToZero();
	zero_page->refCount = 1;
}

/* XXX: nothing yet-- not enough concrete-only states to justify */
void ObjectState::garbageCollect(void)
{
#if 0
	unsigned	c = 0;
	foreach (it, objs.begin(), objs.end())
		if ((*it)->isConcrete())
			c++;
	std::cerr << "OS-GC: c=" << c << '\n';
#endif
}

void ObjectState::writeConcrete(const uint8_t* addr, unsigned wr_sz)
{ memcpy(concreteStore.get(), addr, wr_sz); }

void ObjectState::readConcrete(uint8_t* addr, unsigned rd_sz, unsigned off) const
{ memcpy(addr, concreteStore.get() + off, rd_sz);  }


int ObjectState::readConcreteSafe(
	uint8_t* buf, unsigned rd_sz, unsigned off) const
{
	int	copy_total, copy_len;

	copy_len = rd_sz;
	if (rd_sz + off >= size) {
		copy_len = size - off;
	}

	copy_total = 0;
	for (int i = 0; i < copy_len; i++) {
		if (!isByteConcrete(off+i))
			break;
		buf[i] = concreteStore[off+i];
		copy_total++;
	}

	return copy_total;
}

ObjectState* ObjectState::createDemandObj(unsigned sz)
{
	if (sz == 4096 && UseZeroPage) {
		assert (zero_page->isZeroPage());
		assert (zero_page->isConcrete());
		zero_page->refCount++;
		return zero_page.get();
	}
	return ObjectState::create(sz);
}

ObjectState* ObjectState::create(unsigned size)
{ return os_alloc->create(size); }

ObjectState* ObjectState::create(unsigned size, const ref<Array>& arr)
{ return os_alloc->create(size, arr); }

ObjectState* ObjectState::create(const ObjectState& os)
{ return os_alloc->create(os); }

bool ObjectState::isRevertible(void) const
{
	if (isConcrete()) return false;
	return concreteMask->isones(size);
}

bool ObjectState::revertToConcrete(void)
{
	if (isConcrete()) return true;

	assert (concreteMask != nullptr);

	if (concreteMask->isones(size) == false)
		return false;

	flushMask = nullptr;
	knownSymbolics = nullptr;
	concreteMask = nullptr;

	return true;
}


int ObjectState::cmpConcrete(
	const uint8_t* addr, unsigned sz, unsigned off) const
{
	if (isConcrete())
		return memcmp(addr, concreteStore.get() + off, sz);

	for (unsigned i = 0; i < sz; i++) {
		int	diff;
		if (isByteConcrete(i+off)) {
			diff = concreteStore[i+off] - addr[i];
			if (diff) return diff;
		}
	}
	return 0;
}

/* lol hack */
bool ObjectState::revertToConcrete(const ObjectState* os_c)
{
	ObjectState*	os(const_cast<ObjectState*>(os_c));
	return os->revertToConcrete();
}

int ObjectState::cmpConcrete(const ObjectState& os) const
{
	if (os.size != size) return 0;

	revertToConcrete(&os);
	revertToConcrete(this);

	if (!isConcrete() || !os.isConcrete()) return 0;
	return memcmp(concreteStore.get(), os.concreteStore.get(), size);
}

void ObjectState::printDiff(const ObjectState& os) const
{
	if (os.size != size) {
		std::cerr << "[ObjDiff] size mismatch: "
			<< size << " vs " << os.size << '\n';
		return;
	}

	for (unsigned i = 0 ; i < size; i++) {
		if (concreteStore[i] != os.concreteStore[i])
			std::cerr << "[ObjDiff] Diff @" << i << ": " <<
				(void*)(long)concreteStore[i] << " vs " <<
				(void*)(long)os.concreteStore[i] << '\n';
	}
}
