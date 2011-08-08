/* stupid kludges to prevent future errors */
#ifndef KLEE_EXPR_H
#error Only include in expr.h
#endif

#ifdef KLEE_ARRAY_H
#error Only include once
#endif

#define KLEE_ARRAY_H

class Array {
private:
  unsigned int chk_val;
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
  void decRefIfCared() const { if (refCount != refCountDontCare) --refCount; if (refCount == 0) delete this; }

public:
  /// Array - Construct a new array object.
  ///
  /// \param _name - The name for this array. Names should generally be unique
  /// across an application, but this is not necessary for correctness, except
  /// when printing expressions. When expressions are printed the output will
  /// not parse correctly since two arrays with the same name cannot be
  /// distinguished once printed.
  Array(const std::string &_name,
        MallocKey _mallocKey,
        const ref<ConstantExpr> *constantValuesBegin = 0,
        const ref<ConstantExpr> *constantValuesEnd = 0);
  ~Array();

  bool isSymbolicArray() const {
  	assert (chk_val == 0x12345678);
	return (constant_count == 0);
  }
  bool isConstantArray() const { return !isSymbolicArray(); }

  const ref<ConstantExpr> getValue(unsigned int k) const;
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

  struct Compare
  {
    // returns true if a < b
    bool operator()(const Array* a, const Array* b) const
    {
      return (*a < *b);
    }
  };

private:
  /// constantValues - The constant initial values for this array, or empty for
  /// a symbolic array. If non-empty, this size of this array is equivalent to
  /// the array size.
  std::vector< ref<ConstantExpr> >	constantValues_expr;
  uint8_t*				constantValues_u8;
  unsigned int				constant_count;
};
