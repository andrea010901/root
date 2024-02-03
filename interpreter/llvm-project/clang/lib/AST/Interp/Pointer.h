//===--- Pointer.h - Types for the constexpr VM -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines the classes responsible for pointer tracking.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_INTERP_POINTER_H
#define LLVM_CLANG_AST_INTERP_POINTER_H

#include "Descriptor.h"
#include "InterpBlock.h"
#include "clang/AST/ComparisonCategories.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
namespace interp {
class Block;
class DeadBlock;
class Pointer;
class Context;
enum PrimType : unsigned;

class Pointer;
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Pointer &P);

/// A pointer to a memory block, live or dead.
///
/// This object can be allocated into interpreter stack frames. If pointing to
/// a live block, it is a link in the chain of pointers pointing to the block.
///
/// In the simplest form, a Pointer has a Block* (the pointee) and both Base
/// and Offset are 0, which means it will point to raw data.
///
/// The Base field is used to access metadata about the data. For primitive
/// arrays, the Base is followed by an InitMap. In a variety of cases, the
/// Base is preceded by an InlineDescriptor, which is used to track the
/// initialization state, among other things.
///
/// The Offset field is used to access the actual data. In other words, the
/// data the pointer decribes can be found at
/// Pointee->rawData() + Pointer.Offset.
///
///
/// Pointee                      Offset
/// │                              │
/// │                              │
/// ▼                              ▼
/// ┌───────┬────────────┬─────────┬────────────────────────────┐
/// │ Block │ InlineDesc │ InitMap │ Actual Data                │
/// └───────┴────────────┴─────────┴────────────────────────────┘
///                      ▲
///                      │
///                      │
///                     Base
class Pointer {
private:
  static constexpr unsigned PastEndMark = ~0u;
  static constexpr unsigned RootPtrMark = ~0u;

public:
  Pointer() {}
  Pointer(Block *B);
  Pointer(Block *B, unsigned BaseAndOffset);
  Pointer(const Pointer &P);
  Pointer(Pointer &&P);
  ~Pointer();

  void operator=(const Pointer &P);
  void operator=(Pointer &&P);

  /// Equality operators are just for tests.
  bool operator==(const Pointer &P) const {
    return Pointee == P.Pointee && Base == P.Base && Offset == P.Offset;
  }

  bool operator!=(const Pointer &P) const {
    return Pointee != P.Pointee || Base != P.Base || Offset != P.Offset;
  }

  /// Converts the pointer to an APValue.
  APValue toAPValue() const;

  /// Converts the pointer to a string usable in diagnostics.
  std::string toDiagnosticString(const ASTContext &Ctx) const;

  unsigned getIntegerRepresentation() const {
    return reinterpret_cast<uintptr_t>(Pointee) + Offset;
  }

  /// Converts the pointer to an APValue that is an rvalue.
  std::optional<APValue> toRValue(const Context &Ctx) const;

  /// Offsets a pointer inside an array.
  [[nodiscard]] Pointer atIndex(unsigned Idx) const {
    if (Base == RootPtrMark)
      return Pointer(Pointee, RootPtrMark, getDeclDesc()->getSize());
    unsigned Off = Idx * elemSize();
    if (getFieldDesc()->ElemDesc)
      Off += sizeof(InlineDescriptor);
    else
      Off += sizeof(InitMapPtr);
    return Pointer(Pointee, Base, Base + Off);
  }

  /// Creates a pointer to a field.
  [[nodiscard]] Pointer atField(unsigned Off) const {
    unsigned Field = Offset + Off;
    return Pointer(Pointee, Field, Field);
  }

  /// Subtract the given offset from the current Base and Offset
  /// of the pointer.
  [[nodiscard]]  Pointer atFieldSub(unsigned Off) const {
    assert(Offset >= Off);
    unsigned O = Offset - Off;
    return Pointer(Pointee, O, O);
  }

  /// Restricts the scope of an array element pointer.
  [[nodiscard]] Pointer narrow() const {
    // Null pointers cannot be narrowed.
    if (isZero() || isUnknownSizeArray())
      return *this;

    // Pointer to an array of base types - enter block.
    if (Base == RootPtrMark)
      return Pointer(Pointee, 0, Offset == 0 ? Offset : PastEndMark);

    // Pointer is one past end - magic offset marks that.
    if (isOnePastEnd())
      return Pointer(Pointee, Base, PastEndMark);

    // Primitive arrays are a bit special since they do not have inline
    // descriptors. If Offset != Base, then the pointer already points to
    // an element and there is nothing to do. Otherwise, the pointer is
    // adjusted to the first element of the array.
    if (inPrimitiveArray()) {
      if (Offset != Base)
        return *this;
      return Pointer(Pointee, Base, Offset + sizeof(InitMapPtr));
    }

    // Pointer is to a field or array element - enter it.
    if (Offset != Base)
      return Pointer(Pointee, Offset, Offset);

    // Enter the first element of an array.
    if (!getFieldDesc()->isArray())
      return *this;

    const unsigned NewBase = Base + sizeof(InlineDescriptor);
    return Pointer(Pointee, NewBase, NewBase);
  }

  /// Expands a pointer to the containing array, undoing narrowing.
  [[nodiscard]] Pointer expand() const {
    if (isElementPastEnd()) {
      // Revert to an outer one-past-end pointer.
      unsigned Adjust;
      if (inPrimitiveArray())
        Adjust = sizeof(InitMapPtr);
      else
        Adjust = sizeof(InlineDescriptor);
      return Pointer(Pointee, Base, Base + getSize() + Adjust);
    }

    // Do not step out of array elements.
    if (Base != Offset)
      return *this;

    // If at base, point to an array of base types.
    if (Base == 0)
      return Pointer(Pointee, RootPtrMark, 0);

    // Step into the containing array, if inside one.
    unsigned Next = Base - getInlineDesc()->Offset;
    const Descriptor *Desc =
        Next == 0 ? getDeclDesc() : getDescriptor(Next)->Desc;
    if (!Desc->IsArray)
      return *this;
    return Pointer(Pointee, Next, Offset);
  }

  /// Checks if the pointer is null.
  bool isZero() const { return Pointee == nullptr; }
  /// Checks if the pointer is live.
  bool isLive() const { return Pointee && !Pointee->IsDead; }
  /// Checks if the item is a field in an object.
  bool isField() const { return Base != 0 && Base != RootPtrMark; }

  /// Accessor for information about the declaration site.
  const Descriptor *getDeclDesc() const {
    assert(Pointee);
    return Pointee->Desc;
  }
  SourceLocation getDeclLoc() const { return getDeclDesc()->getLocation(); }

  /// Returns a pointer to the object of which this pointer is a field.
  [[nodiscard]] Pointer getBase() const {
    if (Base == RootPtrMark) {
      assert(Offset == PastEndMark && "cannot get base of a block");
      return Pointer(Pointee, Base, 0);
    }
    assert(Offset == Base && "not an inner field");
    unsigned NewBase = Base - getInlineDesc()->Offset;
    return Pointer(Pointee, NewBase, NewBase);
  }
  /// Returns the parent array.
  [[nodiscard]] Pointer getArray() const {
    if (Base == RootPtrMark) {
      assert(Offset != 0 && Offset != PastEndMark && "not an array element");
      return Pointer(Pointee, Base, 0);
    }
    assert(Offset != Base && "not an array element");
    return Pointer(Pointee, Base, Base);
  }

  /// Accessors for information about the innermost field.
  const Descriptor *getFieldDesc() const {
    if (Base == 0 || Base == RootPtrMark)
      return getDeclDesc();
    return getInlineDesc()->Desc;
  }

  /// Returns the type of the innermost field.
  QualType getType() const {
    if (inPrimitiveArray() && Offset != Base)
      return getFieldDesc()->getType()->getAsArrayTypeUnsafe()->getElementType();
    return getFieldDesc()->getType();
  }

  [[nodiscard]] Pointer getDeclPtr() const { return Pointer(Pointee); }

  /// Returns the element size of the innermost field.
  size_t elemSize() const {
    if (Base == RootPtrMark)
      return getDeclDesc()->getSize();
    return getFieldDesc()->getElemSize();
  }
  /// Returns the total size of the innermost field.
  size_t getSize() const { return getFieldDesc()->getSize(); }

  /// Returns the offset into an array.
  unsigned getOffset() const {
    assert(Offset != PastEndMark && "invalid offset");
    if (Base == RootPtrMark)
      return Offset;

    unsigned Adjust = 0;
    if (Offset != Base) {
      if (getFieldDesc()->ElemDesc)
        Adjust = sizeof(InlineDescriptor);
      else
        Adjust = sizeof(InitMapPtr);
    }
    return Offset - Base - Adjust;
  }

  /// Whether this array refers to an array, but not
  /// to the first element.
  bool isArrayRoot() const { return inArray() && Offset == Base; }

  /// Checks if the innermost field is an array.
  bool inArray() const { return getFieldDesc()->IsArray; }
  /// Checks if the structure is a primitive array.
  bool inPrimitiveArray() const { return getFieldDesc()->isPrimitiveArray(); }
  /// Checks if the structure is an array of unknown size.
  bool isUnknownSizeArray() const {
    return getFieldDesc()->isUnknownSizeArray();
  }
  /// Checks if the pointer points to an array.
  bool isArrayElement() const { return inArray() && Base != Offset; }
  /// Pointer points directly to a block.
  bool isRoot() const {
    return (Base == 0 || Base == RootPtrMark) && Offset == 0;
  }

  /// Returns the record descriptor of a class.
  const Record *getRecord() const { return getFieldDesc()->ElemRecord; }
  /// Returns the element record type, if this is a non-primive array.
  const Record *getElemRecord() const {
    const Descriptor *ElemDesc = getFieldDesc()->ElemDesc;
    return ElemDesc ? ElemDesc->ElemRecord : nullptr;
  }
  /// Returns the field information.
  const FieldDecl *getField() const { return getFieldDesc()->asFieldDecl(); }

  /// Checks if the object is a union.
  bool isUnion() const;

  /// Checks if the storage is extern.
  bool isExtern() const { return Pointee && Pointee->isExtern(); }
  /// Checks if the storage is static.
  bool isStatic() const {
    assert(Pointee);
    return Pointee->isStatic();
  }
  /// Checks if the storage is temporary.
  bool isTemporary() const {
    assert(Pointee);
    return Pointee->isTemporary();
  }
  /// Checks if the storage is a static temporary.
  bool isStaticTemporary() const { return isStatic() && isTemporary(); }

  /// Checks if the field is mutable.
  bool isMutable() const {
    return Base != 0 && getInlineDesc()->IsFieldMutable;
  }
  /// Checks if an object was initialized.
  bool isInitialized() const;
  /// Checks if the object is active.
  bool isActive() const { return Base == 0 || getInlineDesc()->IsActive; }
  /// Checks if a structure is a base class.
  bool isBaseClass() const { return isField() && getInlineDesc()->IsBase; }
  /// Checks if the pointer pointers to a dummy value.
  bool isDummy() const { return getDeclDesc()->isDummy(); }

  /// Checks if an object or a subfield is mutable.
  bool isConst() const {
    return Base == 0 ? getDeclDesc()->IsConst : getInlineDesc()->IsConst;
  }

  /// Returns the declaration ID.
  std::optional<unsigned> getDeclID() const {
    assert(Pointee);
    return Pointee->getDeclID();
  }

  /// Returns the byte offset from the start.
  unsigned getByteOffset() const {
    return Offset;
  }

  /// Returns the number of elements.
  unsigned getNumElems() const { return getSize() / elemSize(); }

  const Block *block() const { return Pointee; }

  /// Returns the index into an array.
  int64_t getIndex() const {
    if (isElementPastEnd())
      return 1;

    // narrow()ed element in a composite array.
    if (Base > 0 && Base == Offset)
      return 0;

    if (auto ElemSize = elemSize())
      return getOffset() / ElemSize;
    return 0;
  }

  /// Checks if the index is one past end.
  bool isOnePastEnd() const {
    if (!Pointee)
      return false;
    return isElementPastEnd() || getSize() == getOffset();
  }

  /// Checks if the pointer is an out-of-bounds element pointer.
  bool isElementPastEnd() const { return Offset == PastEndMark; }

  /// Dereferences the pointer, if it's live.
  template <typename T> T &deref() const {
    assert(isLive() && "Invalid pointer");
    assert(Pointee);
    if (isArrayRoot())
      return *reinterpret_cast<T *>(Pointee->rawData() + Base +
                                    sizeof(InitMapPtr));

    assert(Offset + sizeof(T) <= Pointee->getDescriptor()->getAllocSize());
    return *reinterpret_cast<T *>(Pointee->rawData() + Offset);
  }

  /// Dereferences a primitive element.
  template <typename T> T &elem(unsigned I) const {
    assert(I < getNumElems());
    assert(Pointee);
    return reinterpret_cast<T *>(Pointee->data() + sizeof(InitMapPtr))[I];
  }

  /// Initializes a field.
  void initialize() const;
  /// Activats a field.
  void activate() const;
  /// Deactivates an entire strurcutre.
  void deactivate() const;

  /// Compare two pointers.
  ComparisonCategoryResult compare(const Pointer &Other) const {
    if (!hasSameBase(*this, Other))
      return ComparisonCategoryResult::Unordered;

    if (Offset < Other.Offset)
      return ComparisonCategoryResult::Less;
    else if (Offset > Other.Offset)
      return ComparisonCategoryResult::Greater;

    return ComparisonCategoryResult::Equal;
  }

  /// Checks if two pointers are comparable.
  static bool hasSameBase(const Pointer &A, const Pointer &B);
  /// Checks if two pointers can be subtracted.
  static bool hasSameArray(const Pointer &A, const Pointer &B);

  /// Prints the pointer.
  void print(llvm::raw_ostream &OS) const {
    OS << Pointee << " {";
    if (Base == RootPtrMark)
      OS << "rootptr, ";
    else
      OS << Base << ", ";

    if (Offset == PastEndMark)
      OS << "pastend, ";
    else
      OS << Offset << ", ";

    if (Pointee)
      OS << Pointee->getSize();
    else
      OS << "nullptr";
    OS << "}";
  }

private:
  friend class Block;
  friend class DeadBlock;
  friend struct InitMap;

  Pointer(Block *Pointee, unsigned Base, unsigned Offset);

  /// Returns the embedded descriptor preceding a field.
  InlineDescriptor *getInlineDesc() const { return getDescriptor(Base); }

  /// Returns a descriptor at a given offset.
  InlineDescriptor *getDescriptor(unsigned Offset) const {
    assert(Offset != 0 && "Not a nested pointer");
    assert(Pointee);
    return reinterpret_cast<InlineDescriptor *>(Pointee->rawData() + Offset) -
           1;
  }

  /// Returns a reference to the InitMapPtr which stores the initialization map.
  InitMapPtr &getInitMap() const {
    assert(Pointee);
    return *reinterpret_cast<InitMapPtr *>(Pointee->rawData() + Base);
  }

  /// The block the pointer is pointing to.
  Block *Pointee = nullptr;
  /// Start of the current subfield.
  unsigned Base = 0;
  /// Offset into the block.
  unsigned Offset = 0;

  /// Previous link in the pointer chain.
  Pointer *Prev = nullptr;
  /// Next link in the pointer chain.
  Pointer *Next = nullptr;
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Pointer &P) {
  P.print(OS);
  return OS;
}

} // namespace interp
} // namespace clang

#endif
