/* stupid kludges to prevent future errors */
#ifndef KLEE_EXPR_H
#error Only include in expr.h
#endif

#ifdef KLEE_ARRAY_H
#error Only include once
#endif

#define KLEE_ARRAY_H

class ConstantExpr;
static inline ref<ConstantExpr> createConstantExpr(uint64_t v, Expr::Width w);


struct ArrayConsLT { bool operator()(const Array *a, const Array *b) const; };

#define ARRAY_CHK_VAL	0x12345678
#define ARR2REF(x)	ref<Array>(const_cast<Array*>(x))

class Array
{
private:
	unsigned int chk_val;
	// hash cons for Array objects
	typedef std::set<Array*, ArrayConsLT> ArrayHashCons;
	static ArrayHashCons arrayHashCons;
	static unsigned count;

public:
	const std::string name;
	MallocKey mallocKey;

	// FIXME: This does not belong here.
	mutable void *stpInitialArray;
	mutable void *btorInitialArray;
	mutable void *z3InitialArray;
	mutable unsigned refCount;  // used only for const_arr's
	static const unsigned refCountDontCare = unsigned(-1);
	void initRef() const { refCount = 0; }
	void incRefIfCared() const { if (refCount != refCountDontCare) ++refCount; }
	void decRefIfCared() const
	{
		if (refCount != refCountDontCare) --refCount;
		if (refCount == 0) delete this;
	}
	unsigned int getSize(void) const { return mallocKey.size; }

	static ref<Array> get(const std::string &_name);
	static ref<Array> create(
		const std::string &_name,
		MallocKey _mallocKey,
		const ref<ConstantExpr> *constantValuesBegin = 0,
		const ref<ConstantExpr> *constantValuesEnd = 0);
	static ref<Array> uniqueArray(Array* arr);
	static unsigned getNumArrays(void) { return count; }
public:
  /// Array - Construct a new array object.
  ///
  /// \param _name - The name for this array. Names should generally be unique
  /// across an application, but this is not necessary for correctness, except
  /// when printing expressions. When expressions are printed the output will
  /// not parse correctly since two arrays with the same name cannot be
  /// distinguished once printed.
  ~Array();

  bool isSymbolicArray() const {
  	assert (chk_val == ARRAY_CHK_VAL);
	return (constant_count == 0);
  }
  bool isConstantArray() const { return !isSymbolicArray(); }

  /* this was eating about 5% of exec before */
  const ref<ConstantExpr> getValue(unsigned int k) const
  {
	if (constantValues_u8)
		return createConstantExpr(constantValues_u8[k], 8);
	return constantValues_expr[k];
  }

  void getConstantValues(std::vector< ref<ConstantExpr> >& v) const;

  Expr::Width getDomain() const { return Expr::Int32; }
  Expr::Width getRange() const { return Expr::Int8; }

  const char* getSTPArrayName() const {
    if (!stpInitialArray)
      return NULL;
    const char* stpName = exprName(stpInitialArray);
    assert(stpName);
    return stpName;
  }

  // returns true if a < b
  bool operator< (const Array &b) const;
  bool lt_cons (const Array &b) const;

  bool operator== (const Array &b) const
  {
  	if (&b == this) return true;
  	return !(*this < b || b < *this);
  }

  bool isSingleValue(void) const { return !singleValue.isNull(); }

  struct Compare
  {
    // returns true if a < b
    bool operator()(const Array* a, const Array* b) const
    { return (*a < *b); }
  };

  void print(std::ostream& os) const;

private:
	Array(	const std::string &_name,
		MallocKey _mallocKey,
		const ref<ConstantExpr> *constantValuesBegin = 0,
		const ref<ConstantExpr> *constantValuesEnd = 0);

	// constantValues - The constant initial values for this array,
	// or empty for a symbolic array.
	// If non-empty, this size of this array is equiv to the array size.
	std::vector< ref<ConstantExpr> >	constantValues_expr;
	uint8_t*	constantValues_u8;
	unsigned int	constant_count;
	ref<Expr>	singleValue;
	typedef std::map<const std::string, Array*> name2arr_ty;
	static name2arr_ty name2arr;
};
