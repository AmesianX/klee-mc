#ifndef OBJSTATE_H
#define OBJSTATE_H

#ifndef KLEE_MEMORY_H
#error Never include objstate.h; use memory.h
#endif

class ObjectState
{
	friend class AddressSpace;
	friend class ExecutionState;
private:
	const Array* src_array;
	unsigned copyOnWriteOwner; // exclusively for AddressSpace

	friend class ObjectHolder;
	unsigned refCount;

	const MemoryObject *object;

	uint8_t *concreteStore;
	// XXX cleanup name of flushMask (its backwards or something)
	BitArray *concreteMask;

	// mutable because may need flushed during read of const
	mutable BitArray *flushMask;

	ref<Expr> *knownSymbolics;

public:
	// mutable because we may need flush during read of const
	mutable UpdateList updates; /* XXX DEBUG, move to private */

	unsigned size;

	bool readOnly;

	unsigned wrseqno;
	bool wasSymOffObjectWrite;

public:
	/// Create a new object state for the given memory object with concrete
	/// contents. The initial contents are undefined, it is the callers
	/// responsibility to initialize the object contents appropriately.
	ObjectState(const MemoryObject *mo);

	/// Create a new object state for the given memory object with symbolic
	/// contents.
	ObjectState(const MemoryObject *mo, const Array *array);

	ObjectState(const ObjectState &os);
	~ObjectState();

	bool equals(const ObjectState* o1) const;
	const MemoryObject *getObject() const { return object; }

	void setReadOnly(bool ro) { readOnly = ro; }

	// make contents all concrete and zero
	void initializeToZero();
	// make contents all concrete and random
	void initializeToRandom();  

	unsigned int getNumConcrete(void) const;
	bool isByteConcrete(unsigned offset) const;
	bool isByteFlushed(unsigned offset) const;
	bool isByteKnownSymbolic(unsigned offset) const;
	void markRangeSymbolic(unsigned offset, unsigned len);

	ref<Expr> read(ref<Expr> offset, Expr::Width width) const;
	ref<Expr> read(unsigned offset, Expr::Width width) const;
	ref<Expr> read8(unsigned offset) const;

	const Array* getArray(void) const { return src_array; }
	void print(unsigned int begin = 0, int end = -1);

private:

	// return bytes written.
	void write(unsigned offset, ref<Expr> value);
	void write(ref<Expr> offset, ref<Expr> value);

	void write8(unsigned offset, uint8_t value);
	void write16(unsigned offset, uint16_t value);
	void write32(unsigned offset, uint32_t value);
	void write64(unsigned offset, uint64_t value);
private:
	const UpdateList &getUpdates() const;

	void makeConcrete();

	void makeSymbolic();

	ref<Expr> read8(ref<Expr> offset) const;
	void write8(unsigned offset, ref<Expr> value);
	void write8(ref<Expr> offset, ref<Expr> value);

	void fastRangeCheckOffset(ref<Expr> offset, unsigned *base_r, 
			    unsigned *size_r) const;
	void flushRangeForRead(unsigned rangeBase, unsigned rangeSize) const;
	void flushRangeForWrite(unsigned rangeBase, unsigned rangeSize);
	void flushForRead(void) const { flushRangeForRead(0, size); }

	void markByteConcrete(unsigned offset);
	void markByteSymbolic(unsigned offset);
	void markByteFlushed(unsigned offset);
	void markByteUnflushed(unsigned offset);
	void setKnownSymbolic(unsigned offset, Expr *value);
};
  
#endif