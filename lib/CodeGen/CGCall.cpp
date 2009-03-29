//===----- CGCall.h - Encapsulate calling convention details ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These classes wrap the information about a call or function
// definition used to handle ABI compliancy.
//
//===----------------------------------------------------------------------===//

#include "CGCall.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Attributes.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetData.h"

#include "ABIInfo.h"

using namespace clang;
using namespace CodeGen;

/***/

// FIXME: Use iterator and sidestep silly type array creation.

const 
CGFunctionInfo &CodeGenTypes::getFunctionInfo(const FunctionNoProtoType *FTNP) {
  return getFunctionInfo(FTNP->getResultType(), 
                         llvm::SmallVector<QualType, 16>());
}

const 
CGFunctionInfo &CodeGenTypes::getFunctionInfo(const FunctionProtoType *FTP) {
  llvm::SmallVector<QualType, 16> ArgTys;
  // FIXME: Kill copy.
  for (unsigned i = 0, e = FTP->getNumArgs(); i != e; ++i)
    ArgTys.push_back(FTP->getArgType(i));
  return getFunctionInfo(FTP->getResultType(), ArgTys);
}

const CGFunctionInfo &CodeGenTypes::getFunctionInfo(const FunctionDecl *FD) {
  const FunctionType *FTy = FD->getType()->getAsFunctionType();
  if (const FunctionProtoType *FTP = dyn_cast<FunctionProtoType>(FTy))
    return getFunctionInfo(FTP);
  return getFunctionInfo(cast<FunctionNoProtoType>(FTy));
}

const CGFunctionInfo &CodeGenTypes::getFunctionInfo(const ObjCMethodDecl *MD) {
  llvm::SmallVector<QualType, 16> ArgTys;
  ArgTys.push_back(MD->getSelfDecl()->getType());
  ArgTys.push_back(Context.getObjCSelType());
  // FIXME: Kill copy?
  for (ObjCMethodDecl::param_iterator i = MD->param_begin(),
         e = MD->param_end(); i != e; ++i)
    ArgTys.push_back((*i)->getType());
  return getFunctionInfo(MD->getResultType(), ArgTys);
}

const CGFunctionInfo &CodeGenTypes::getFunctionInfo(QualType ResTy, 
                                                    const CallArgList &Args) {
  // FIXME: Kill copy.
  llvm::SmallVector<QualType, 16> ArgTys;
  for (CallArgList::const_iterator i = Args.begin(), e = Args.end(); 
       i != e; ++i)
    ArgTys.push_back(i->second);
  return getFunctionInfo(ResTy, ArgTys);
}

const CGFunctionInfo &CodeGenTypes::getFunctionInfo(QualType ResTy, 
                                                  const FunctionArgList &Args) {
  // FIXME: Kill copy.
  llvm::SmallVector<QualType, 16> ArgTys;
  for (FunctionArgList::const_iterator i = Args.begin(), e = Args.end(); 
       i != e; ++i)
    ArgTys.push_back(i->second);
  return getFunctionInfo(ResTy, ArgTys);
}

const CGFunctionInfo &CodeGenTypes::getFunctionInfo(QualType ResTy,
                               const llvm::SmallVector<QualType, 16> &ArgTys) {
  // Lookup or create unique function info.
  llvm::FoldingSetNodeID ID;
  CGFunctionInfo::Profile(ID, ResTy, ArgTys.begin(), ArgTys.end());

  void *InsertPos = 0;
  CGFunctionInfo *FI = FunctionInfos.FindNodeOrInsertPos(ID, InsertPos);
  if (FI)
    return *FI;

  // Construct the function info.
  FI = new CGFunctionInfo(ResTy, ArgTys);
  FunctionInfos.InsertNode(FI, InsertPos);

  // Compute ABI information.
  getABIInfo().computeInfo(*FI, getContext());

  return *FI;
}

/***/

ABIInfo::~ABIInfo() {}

void ABIArgInfo::dump() const {
  fprintf(stderr, "(ABIArgInfo Kind=");
  switch (TheKind) {
  case Direct: 
    fprintf(stderr, "Direct");
    break;
  case Ignore: 
    fprintf(stderr, "Ignore");
    break;
  case Coerce: 
    fprintf(stderr, "Coerce Type=");
    getCoerceToType()->print(llvm::errs());
    break;
  case Indirect: 
    fprintf(stderr, "Indirect Align=%d", getIndirectAlign());
    break;
  case Expand: 
    fprintf(stderr, "Expand");
    break;
  }
  fprintf(stderr, ")\n");
}

/***/

/// isEmptyStruct - Return true iff a structure has no non-empty
/// members. Note that a structure with a flexible array member is not
/// considered empty.
static bool isEmptyStruct(QualType T) {
  const RecordType *RT = T->getAsStructureType();
  if (!RT)
    return 0;
  const RecordDecl *RD = RT->getDecl();
  if (RD->hasFlexibleArrayMember())
    return false;
  for (RecordDecl::field_iterator i = RD->field_begin(), 
         e = RD->field_end(); i != e; ++i) {
    const FieldDecl *FD = *i;
    if (!isEmptyStruct(FD->getType()))
      return false;
  }
  return true;
}

/// isSingleElementStruct - Determine if a structure is a "single
/// element struct", i.e. it has exactly one non-empty field or
/// exactly one field which is itself a single element
/// struct. Structures with flexible array members are never
/// considered single element structs.
///
/// \return The field declaration for the single non-empty field, if
/// it exists.
static const FieldDecl *isSingleElementStruct(QualType T) {
  const RecordType *RT = T->getAsStructureType();
  if (!RT)
    return 0;

  const RecordDecl *RD = RT->getDecl();
  if (RD->hasFlexibleArrayMember())
    return 0;

  const FieldDecl *Found = 0;
  for (RecordDecl::field_iterator i = RD->field_begin(), 
         e = RD->field_end(); i != e; ++i) {
    const FieldDecl *FD = *i;
    QualType FT = FD->getType();

    if (isEmptyStruct(FT)) {
      // Ignore
    } else if (Found) {
      return 0;
    } else if (!CodeGenFunction::hasAggregateLLVMType(FT)) {
      Found = FD;
    } else {
      Found = isSingleElementStruct(FT);
      if (!Found)
        return 0;
    }
  }

  return Found;
}

static bool is32Or64BitBasicType(QualType Ty, ASTContext &Context) {
  if (!Ty->getAsBuiltinType() && !Ty->isPointerType())
    return false;

  uint64_t Size = Context.getTypeSize(Ty);
  return Size == 32 || Size == 64;
}

static bool areAllFields32Or64BitBasicType(const RecordDecl *RD,
                                           ASTContext &Context) {
  for (RecordDecl::field_iterator i = RD->field_begin(), 
         e = RD->field_end(); i != e; ++i) {
    const FieldDecl *FD = *i;

    if (!is32Or64BitBasicType(FD->getType(), Context))
      return false;
    
    // FIXME: Reject bitfields wholesale; there are two problems, we
    // don't know how to expand them yet, and the predicate for
    // telling if a bitfield still counts as "basic" is more
    // complicated than what we were doing previously.
    if (FD->isBitField())
      return false;
  }

  return true;
}

namespace {
/// DefaultABIInfo - The default implementation for ABI specific
/// details. This implementation provides information which results in
/// self-consistent and sensible LLVM IR generation, but does not
/// conform to any particular ABI.
class DefaultABIInfo : public ABIInfo {
  ABIArgInfo classifyReturnType(QualType RetTy, 
                                ASTContext &Context) const;
  
  ABIArgInfo classifyArgumentType(QualType RetTy,
                                  ASTContext &Context) const;

  virtual void computeInfo(CGFunctionInfo &FI, ASTContext &Context) const {
    FI.getReturnInfo() = classifyReturnType(FI.getReturnType(), Context);
    for (CGFunctionInfo::arg_iterator it = FI.arg_begin(), ie = FI.arg_end();
         it != ie; ++it)
      it->info = classifyArgumentType(it->type, Context);
  }

  virtual llvm::Value *EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                 CodeGenFunction &CGF) const;
};

/// X86_32ABIInfo - The X86-32 ABI information.
class X86_32ABIInfo : public ABIInfo {
  bool IsDarwin;

public:
  ABIArgInfo classifyReturnType(QualType RetTy, 
                                ASTContext &Context) const;

  ABIArgInfo classifyArgumentType(QualType RetTy,
                                  ASTContext &Context) const;

  virtual void computeInfo(CGFunctionInfo &FI, ASTContext &Context) const {
    FI.getReturnInfo() = classifyReturnType(FI.getReturnType(), Context);
    for (CGFunctionInfo::arg_iterator it = FI.arg_begin(), ie = FI.arg_end();
         it != ie; ++it)
      it->info = classifyArgumentType(it->type, Context);
  }

  virtual llvm::Value *EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                 CodeGenFunction &CGF) const;

  X86_32ABIInfo(bool d) : ABIInfo(), IsDarwin(d) {}
};
}

ABIArgInfo X86_32ABIInfo::classifyReturnType(QualType RetTy,
                                            ASTContext &Context) const {
  if (RetTy->isVoidType()) {
    return ABIArgInfo::getIgnore();
  } else if (CodeGenFunction::hasAggregateLLVMType(RetTy)) {
    // Outside of Darwin, structs and unions are always indirect.
    if (!IsDarwin && !RetTy->isAnyComplexType())
      return ABIArgInfo::getIndirect(0);
    // Classify "single element" structs as their element type.
    const FieldDecl *SeltFD = isSingleElementStruct(RetTy);
    if (SeltFD) {
      QualType SeltTy = SeltFD->getType()->getDesugaredType();
      if (const BuiltinType *BT = SeltTy->getAsBuiltinType()) {
        // FIXME: This is gross, it would be nice if we could just
        // pass back SeltTy and have clients deal with it. Is it worth
        // supporting coerce to both LLVM and clang Types?
        if (BT->isIntegerType()) {
          uint64_t Size = Context.getTypeSize(SeltTy);
          return ABIArgInfo::getCoerce(llvm::IntegerType::get((unsigned) Size));
        } else if (BT->getKind() == BuiltinType::Float) {
          return ABIArgInfo::getCoerce(llvm::Type::FloatTy);
        } else if (BT->getKind() == BuiltinType::Double) {
          return ABIArgInfo::getCoerce(llvm::Type::DoubleTy);
        }
      } else if (SeltTy->isPointerType()) {
        // FIXME: It would be really nice if this could come out as
        // the proper pointer type.
        llvm::Type *PtrTy = 
          llvm::PointerType::getUnqual(llvm::Type::Int8Ty);
        return ABIArgInfo::getCoerce(PtrTy);
      }
    }

    uint64_t Size = Context.getTypeSize(RetTy);
    if (Size == 8) {
      return ABIArgInfo::getCoerce(llvm::Type::Int8Ty);
    } else if (Size == 16) {
      return ABIArgInfo::getCoerce(llvm::Type::Int16Ty);
    } else if (Size == 32) {
      return ABIArgInfo::getCoerce(llvm::Type::Int32Ty);
    } else if (Size == 64) {
      return ABIArgInfo::getCoerce(llvm::Type::Int64Ty);
    } else {
      return ABIArgInfo::getIndirect(0);
    }
  } else {
    return ABIArgInfo::getDirect();
  }
}

ABIArgInfo X86_32ABIInfo::classifyArgumentType(QualType Ty,
                                               ASTContext &Context) const {
  // FIXME: Set alignment on indirect arguments.
  if (CodeGenFunction::hasAggregateLLVMType(Ty)) {
    // Structures with flexible arrays are always indirect.
    if (const RecordType *RT = Ty->getAsStructureType())
      if (RT->getDecl()->hasFlexibleArrayMember())
        return ABIArgInfo::getIndirect(0);

    // Ignore empty structs.
    uint64_t Size = Context.getTypeSize(Ty);
    if (Ty->isStructureType() && Size == 0)
      return ABIArgInfo::getIgnore();

    // Expand structs with size <= 128-bits which consist only of
    // basic types (int, long long, float, double, xxx*). This is
    // non-recursive and does not ignore empty fields.
    if (const RecordType *RT = Ty->getAsStructureType()) {
      if (Context.getTypeSize(Ty) <= 4*32 &&
          areAllFields32Or64BitBasicType(RT->getDecl(), Context))
        return ABIArgInfo::getExpand();
    }

    return ABIArgInfo::getIndirect(0);
  } else {
    return ABIArgInfo::getDirect();
  }
}

llvm::Value *X86_32ABIInfo::EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                      CodeGenFunction &CGF) const {
  const llvm::Type *BP = llvm::PointerType::getUnqual(llvm::Type::Int8Ty);
  const llvm::Type *BPP = llvm::PointerType::getUnqual(BP);

  CGBuilderTy &Builder = CGF.Builder;
  llvm::Value *VAListAddrAsBPP = Builder.CreateBitCast(VAListAddr, BPP, 
                                                       "ap");
  llvm::Value *Addr = Builder.CreateLoad(VAListAddrAsBPP, "ap.cur");
  llvm::Type *PTy = 
    llvm::PointerType::getUnqual(CGF.ConvertType(Ty));
  llvm::Value *AddrTyped = Builder.CreateBitCast(Addr, PTy);
  
  uint64_t Offset = 
    llvm::RoundUpToAlignment(CGF.getContext().getTypeSize(Ty) / 8, 4);
  llvm::Value *NextAddr = 
    Builder.CreateGEP(Addr, 
                      llvm::ConstantInt::get(llvm::Type::Int32Ty, Offset),
                      "ap.next");
  Builder.CreateStore(NextAddr, VAListAddrAsBPP);

  return AddrTyped;
}

namespace {
/// X86_64ABIInfo - The X86_64 ABI information.
class X86_64ABIInfo : public ABIInfo {
  enum Class {
    Integer = 0,
    SSE,
    SSEUp,
    X87,
    X87Up,
    ComplexX87,
    NoClass,
    Memory
  };

  /// merge - Implement the X86_64 ABI merging algorithm.
  ///
  /// Merge an accumulating classification \arg Accum with a field
  /// classification \arg Field.
  ///
  /// \param Accum - The accumulating classification. This should
  /// always be either NoClass or the result of a previous merge
  /// call. In addition, this should never be Memory (the caller
  /// should just return Memory for the aggregate).
  Class merge(Class Accum, Class Field) const;

  /// classify - Determine the x86_64 register classes in which the
  /// given type T should be passed.
  ///
  /// \param Lo - The classification for the parts of the type
  /// residing in the low word of the containing object.
  ///
  /// \param Hi - The classification for the parts of the type
  /// residing in the high word of the containing object.
  ///
  /// \param OffsetBase - The bit offset of this type in the
  /// containing object.  Some parameters are classified different
  /// depending on whether they straddle an eightbyte boundary.
  ///
  /// If a word is unused its result will be NoClass; if a type should
  /// be passed in Memory then at least the classification of \arg Lo
  /// will be Memory.
  ///
  /// The \arg Lo class will be NoClass iff the argument is ignored.
  ///
  /// If the \arg Lo class is ComplexX87, then the \arg Hi class will
  /// also be ComplexX87.
  void classify(QualType T, ASTContext &Context, uint64_t OffsetBase,
                Class &Lo, Class &Hi) const;
  
  /// getCoerceResult - Given a source type \arg Ty and an LLVM type
  /// to coerce to, chose the best way to pass Ty in the same place
  /// that \arg CoerceTo would be passed, but while keeping the
  /// emitted code as simple as possible.
  ///
  /// FIXME: Note, this should be cleaned up to just take an
  /// enumeration of all the ways we might want to pass things,
  /// instead of constructing an LLVM type. This makes this code more
  /// explicit, and it makes it clearer that we are also doing this
  /// for correctness in the case of passing scalar types.
  ABIArgInfo getCoerceResult(QualType Ty,
                             const llvm::Type *CoerceTo,
                             ASTContext &Context) const;

  ABIArgInfo classifyReturnType(QualType RetTy, 
                                ASTContext &Context) const;  

  ABIArgInfo classifyArgumentType(QualType Ty,
                                  ASTContext &Context,
                                  unsigned &neededInt,
                                  unsigned &neededSSE) const;

public:
  virtual void computeInfo(CGFunctionInfo &FI, ASTContext &Context) const;

  virtual llvm::Value *EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                 CodeGenFunction &CGF) const;
};
}

X86_64ABIInfo::Class X86_64ABIInfo::merge(Class Accum, 
                                          Class Field) const {
  // AMD64-ABI 3.2.3p2: Rule 4. Each field of an object is
  // classified recursively so that always two fields are
  // considered. The resulting class is calculated according to
  // the classes of the fields in the eightbyte:
  //
  // (a) If both classes are equal, this is the resulting class.
  //
  // (b) If one of the classes is NO_CLASS, the resulting class is
  // the other class.
  //
  // (c) If one of the classes is MEMORY, the result is the MEMORY
  // class.
  //
  // (d) If one of the classes is INTEGER, the result is the
  // INTEGER.
  //
  // (e) If one of the classes is X87, X87UP, COMPLEX_X87 class,
  // MEMORY is used as class.
  //
  // (f) Otherwise class SSE is used.

  // Accum should never be memory (we should have returned) or
  // ComplexX87 (because this cannot be passed in a structure).
  assert((Accum != Memory && Accum != ComplexX87) &&
         "Invalid accumulated classification during merge.");
  if (Accum == Field || Field == NoClass)
    return Accum;
  else if (Field == Memory)
    return Memory;
  else if (Accum == NoClass)
    return Field;
  else if (Accum == Integer || Field == Integer) 
    return Integer;
  else if (Field == X87 || Field == X87Up || Field == ComplexX87)
    return Memory;
  else
    return SSE;
}

void X86_64ABIInfo::classify(QualType Ty,
                             ASTContext &Context,
                             uint64_t OffsetBase,
                             Class &Lo, Class &Hi) const {
  // FIXME: This code can be simplified by introducing a simple value
  // class for Class pairs with appropriate constructor methods for
  // the various situations.

  // FIXME: Some of the split computations are wrong; unaligned
  // vectors shouldn't be passed in registers for example, so there is
  // no chance they can straddle an eightbyte. Verify & simplify.

  Lo = Hi = NoClass;

  Class &Current = OffsetBase < 64 ? Lo : Hi;
  Current = Memory;

  if (const BuiltinType *BT = Ty->getAsBuiltinType()) {
    BuiltinType::Kind k = BT->getKind();

    if (k == BuiltinType::Void) {
      Current = NoClass; 
    } else if (k >= BuiltinType::Bool && k <= BuiltinType::LongLong) {
      Current = Integer;
    } else if (k == BuiltinType::Float || k == BuiltinType::Double) {
      Current = SSE;
    } else if (k == BuiltinType::LongDouble) {
      Lo = X87;
      Hi = X87Up;
    }
    // FIXME: _Decimal32 and _Decimal64 are SSE.
    // FIXME: _float128 and _Decimal128 are (SSE, SSEUp).
    // FIXME: __int128 is (Integer, Integer).
  } else if (const EnumType *ET = Ty->getAsEnumType()) {
    // Classify the underlying integer type.
    classify(ET->getDecl()->getIntegerType(), Context, OffsetBase, Lo, Hi);
  } else if (Ty->hasPointerRepresentation()) {
    Current = Integer;
  } else if (const VectorType *VT = Ty->getAsVectorType()) {
    uint64_t Size = Context.getTypeSize(VT);
    if (Size == 32) {
      // gcc passes all <4 x char>, <2 x short>, <1 x int>, <1 x
      // float> as integer.      
      Current = Integer;

      // If this type crosses an eightbyte boundary, it should be
      // split.
      uint64_t EB_Real = (OffsetBase) / 64;
      uint64_t EB_Imag = (OffsetBase + Size - 1) / 64;
      if (EB_Real != EB_Imag)
        Hi = Lo;      
    } else if (Size == 64) {
      // gcc passes <1 x double> in memory. :(
      if (VT->getElementType()->isSpecificBuiltinType(BuiltinType::Double))
        return;
      
      // gcc passes <1 x long long> as INTEGER.
      if (VT->getElementType()->isSpecificBuiltinType(BuiltinType::LongLong))
        Current = Integer;
      else
        Current = SSE;

      // If this type crosses an eightbyte boundary, it should be
      // split.
      if (OffsetBase && OffsetBase != 64)
        Hi = Lo;
    } else if (Size == 128) {
      Lo = SSE;
      Hi = SSEUp;
    }
  } else if (const ComplexType *CT = Ty->getAsComplexType()) {
    QualType ET = Context.getCanonicalType(CT->getElementType());
    
    uint64_t Size = Context.getTypeSize(Ty);
    if (ET->isIntegralType()) {
      if (Size <= 64)
        Current = Integer;
      else if (Size <= 128)
        Lo = Hi = Integer;
    } else if (ET == Context.FloatTy) 
      Current = SSE;
    else if (ET == Context.DoubleTy)
      Lo = Hi = SSE;
    else if (ET == Context.LongDoubleTy)
      Current = ComplexX87;

    // If this complex type crosses an eightbyte boundary then it
    // should be split.
    uint64_t EB_Real = (OffsetBase) / 64;
    uint64_t EB_Imag = (OffsetBase + Context.getTypeSize(ET)) / 64;
    if (Hi == NoClass && EB_Real != EB_Imag)
      Hi = Lo;
  } else if (const ConstantArrayType *AT = Context.getAsConstantArrayType(Ty)) {
    // Arrays are treated like structures.

    uint64_t Size = Context.getTypeSize(Ty);
    
    // AMD64-ABI 3.2.3p2: Rule 1. If the size of an object is larger
    // than two eightbytes, ..., it has class MEMORY.
    if (Size > 128)
      return;
    
    // AMD64-ABI 3.2.3p2: Rule 1. If ..., or it contains unaligned
    // fields, it has class MEMORY.
    //
    // Only need to check alignment of array base.
    if (OffsetBase % Context.getTypeAlign(AT->getElementType()))
      return;

    // Otherwise implement simplified merge. We could be smarter about
    // this, but it isn't worth it and would be harder to verify.
    Current = NoClass;
    uint64_t EltSize = Context.getTypeSize(AT->getElementType());
    uint64_t ArraySize = AT->getSize().getZExtValue();
    for (uint64_t i=0, Offset=OffsetBase; i<ArraySize; ++i, Offset += EltSize) {
      Class FieldLo, FieldHi;
      classify(AT->getElementType(), Context, Offset, FieldLo, FieldHi);
      Lo = merge(Lo, FieldLo);
      Hi = merge(Hi, FieldHi);
      if (Lo == Memory || Hi == Memory)
        break;
    }
    
    // Do post merger cleanup (see below). Only case we worry about is Memory.
    if (Hi == Memory)
      Lo = Memory;
    assert((Hi != SSEUp || Lo == SSE) && "Invalid SSEUp array classification.");
  } else if (const RecordType *RT = Ty->getAsRecordType()) {
    uint64_t Size = Context.getTypeSize(Ty);
    
    // AMD64-ABI 3.2.3p2: Rule 1. If the size of an object is larger
    // than two eightbytes, ..., it has class MEMORY.
    if (Size > 128)
      return;

    const RecordDecl *RD = RT->getDecl();

    // Assume variable sized types are passed in memory.
    if (RD->hasFlexibleArrayMember())
      return;

    const ASTRecordLayout &Layout = Context.getASTRecordLayout(RD);
    
    // Reset Lo class, this will be recomputed.
    Current = NoClass;
    unsigned idx = 0;
    for (RecordDecl::field_iterator i = RD->field_begin(), 
           e = RD->field_end(); i != e; ++i, ++idx) {
      uint64_t Offset = OffsetBase + Layout.getFieldOffset(idx);
      bool BitField = i->isBitField();

      // AMD64-ABI 3.2.3p2: Rule 1. If ..., or it contains unaligned
      // fields, it has class MEMORY.
      //
      // Note, skip this test for bitfields, see below.
      if (!BitField && Offset % Context.getTypeAlign(i->getType())) {
        Lo = Memory;
        return;
      }

      // Classify this field.
      //
      // AMD64-ABI 3.2.3p2: Rule 3. If the size of the aggregate
      // exceeds a single eightbyte, each is classified
      // separately. Each eightbyte gets initialized to class
      // NO_CLASS.
      Class FieldLo, FieldHi;
      
      // Bitfields require special handling, they do not force the
      // structure to be passed in memory even if unaligned, and
      // therefore they can straddle an eightbyte.
      if (BitField) {
        uint64_t Offset = OffsetBase + Layout.getFieldOffset(idx);
        uint64_t Size = 
          i->getBitWidth()->getIntegerConstantExprValue(Context).getZExtValue();

        uint64_t EB_Lo = Offset / 64;
        uint64_t EB_Hi = (Offset + Size - 1) / 64;
        FieldLo = FieldHi = NoClass;
        if (EB_Lo) {
          assert(EB_Hi == EB_Lo && "Invalid classification, type > 16 bytes.");
          FieldLo = NoClass;
          FieldHi = Integer;
        } else { 
          FieldLo = Integer;
          FieldHi = EB_Hi ? Integer : NoClass;
        }
      } else
        classify(i->getType(), Context, Offset, FieldLo, FieldHi);
      Lo = merge(Lo, FieldLo);
      Hi = merge(Hi, FieldHi);
      if (Lo == Memory || Hi == Memory)
        break;
    }

    // AMD64-ABI 3.2.3p2: Rule 5. Then a post merger cleanup is done:
    //
    // (a) If one of the classes is MEMORY, the whole argument is
    // passed in memory.
    //
    // (b) If SSEUP is not preceeded by SSE, it is converted to SSE.

    // The first of these conditions is guaranteed by how we implement
    // the merge (just bail). 
    //
    // The second condition occurs in the case of unions; for example
    // union { _Complex double; unsigned; }.
    if (Hi == Memory)
      Lo = Memory;
    if (Hi == SSEUp && Lo != SSE)
      Hi = SSE;
  }
}

ABIArgInfo X86_64ABIInfo::getCoerceResult(QualType Ty,
                                          const llvm::Type *CoerceTo,
                                          ASTContext &Context) const {
  if (CoerceTo == llvm::Type::Int64Ty) {
    // Integer and pointer types will end up in a general purpose
    // register.
    if (Ty->isIntegralType() || Ty->isPointerType())
      return ABIArgInfo::getDirect();

  } else if (CoerceTo == llvm::Type::DoubleTy) {
    // FIXME: It would probably be better to make CGFunctionInfo only
    // map using canonical types than to canonize here.
    QualType CTy = Context.getCanonicalType(Ty);
  
    // Float and double end up in a single SSE reg.
    if (CTy == Context.FloatTy || CTy == Context.DoubleTy)
      return ABIArgInfo::getDirect();

  }

  return ABIArgInfo::getCoerce(CoerceTo);
}

ABIArgInfo X86_64ABIInfo::classifyReturnType(QualType RetTy,
                                            ASTContext &Context) const {
  // AMD64-ABI 3.2.3p4: Rule 1. Classify the return type with the
  // classification algorithm.
  X86_64ABIInfo::Class Lo, Hi;
  classify(RetTy, Context, 0, Lo, Hi);

  // Check some invariants.
  assert((Hi != Memory || Lo == Memory) && "Invalid memory classification.");
  assert((Lo != NoClass || Hi == NoClass) && "Invalid null classification.");
  assert((Hi != SSEUp || Lo == SSE) && "Invalid SSEUp classification.");

  const llvm::Type *ResType = 0;
  switch (Lo) {
  case NoClass:
    return ABIArgInfo::getIgnore();

  case SSEUp:
  case X87Up:
    assert(0 && "Invalid classification for lo word.");

    // AMD64-ABI 3.2.3p4: Rule 2. Types of class memory are returned via
    // hidden argument.
  case Memory:
    return ABIArgInfo::getIndirect(0);

    // AMD64-ABI 3.2.3p4: Rule 3. If the class is INTEGER, the next
    // available register of the sequence %rax, %rdx is used.
  case Integer:
    ResType = llvm::Type::Int64Ty; break;

    // AMD64-ABI 3.2.3p4: Rule 4. If the class is SSE, the next
    // available SSE register of the sequence %xmm0, %xmm1 is used.
  case SSE:
    ResType = llvm::Type::DoubleTy; break;

    // AMD64-ABI 3.2.3p4: Rule 6. If the class is X87, the value is
    // returned on the X87 stack in %st0 as 80-bit x87 number.
  case X87:
    ResType = llvm::Type::X86_FP80Ty; break;

    // AMD64-ABI 3.2.3p4: Rule 8. If the class is COMPLEX_X87, the real
    // part of the value is returned in %st0 and the imaginary part in
    // %st1.
  case ComplexX87:
    assert(Hi == ComplexX87 && "Unexpected ComplexX87 classification.");
    ResType = llvm::StructType::get(llvm::Type::X86_FP80Ty,
                                    llvm::Type::X86_FP80Ty,
                                    NULL);
    break;    
  }

  switch (Hi) {
    // Memory was handled previously and X87 should
    // never occur as a hi class.
  case Memory:
  case X87:
    assert(0 && "Invalid classification for hi word.");

  case ComplexX87: // Previously handled.
  case NoClass: break;

  case Integer:
    ResType = llvm::StructType::get(ResType, llvm::Type::Int64Ty, NULL);
    break;
  case SSE:    
    ResType = llvm::StructType::get(ResType, llvm::Type::DoubleTy, NULL);
    break;

    // AMD64-ABI 3.2.3p4: Rule 5. If the class is SSEUP, the eightbyte
    // is passed in the upper half of the last used SSE register.
    //
    // SSEUP should always be preceeded by SSE, just widen.
  case SSEUp:
    assert(Lo == SSE && "Unexpected SSEUp classification.");
    ResType = llvm::VectorType::get(llvm::Type::DoubleTy, 2);
    break;

    // AMD64-ABI 3.2.3p4: Rule 7. If the class is X87UP, the value is
    // returned together with the previous X87 value in %st0.
  case X87Up:
    // If X87Up is preceeded by X87, we don't need to do
    // anything. However, in some cases with unions it may not be
    // preceeded by X87. In such situations we follow gcc and pass the
    // extra bits in an SSE reg.
    if (Lo != X87) 
      ResType = llvm::StructType::get(ResType, llvm::Type::DoubleTy, NULL);
    break;
  }

  return getCoerceResult(RetTy, ResType, Context);
}

ABIArgInfo X86_64ABIInfo::classifyArgumentType(QualType Ty, ASTContext &Context,
                                               unsigned &neededInt,
                                               unsigned &neededSSE) const {
  X86_64ABIInfo::Class Lo, Hi;
  classify(Ty, Context, 0, Lo, Hi);
  
  // Check some invariants.
  // FIXME: Enforce these by construction.
  assert((Hi != Memory || Lo == Memory) && "Invalid memory classification.");
  assert((Lo != NoClass || Hi == NoClass) && "Invalid null classification.");
  assert((Hi != SSEUp || Lo == SSE) && "Invalid SSEUp classification.");

  neededInt = 0;
  neededSSE = 0;
  const llvm::Type *ResType = 0;
  switch (Lo) {
  case NoClass:
    return ABIArgInfo::getIgnore();

    // AMD64-ABI 3.2.3p3: Rule 1. If the class is MEMORY, pass the argument
    // on the stack.
  case Memory:
    
    // AMD64-ABI 3.2.3p3: Rule 5. If the class is X87, X87UP or
    // COMPLEX_X87, it is passed in memory.
  case X87:
  case ComplexX87:
    return ABIArgInfo::getIndirect(0);

  case SSEUp:
  case X87Up:
    assert(0 && "Invalid classification for lo word.");

    // AMD64-ABI 3.2.3p3: Rule 2. If the class is INTEGER, the next
    // available register of the sequence %rdi, %rsi, %rdx, %rcx, %r8
    // and %r9 is used.
  case Integer:
    ++neededInt; 
    ResType = llvm::Type::Int64Ty;
    break;

    // AMD64-ABI 3.2.3p3: Rule 3. If the class is SSE, the next
    // available SSE register is used, the registers are taken in the
    // order from %xmm0 to %xmm7.
  case SSE:
    ++neededSSE; 
    ResType = llvm::Type::DoubleTy;
    break;
  }

  switch (Hi) {
    // Memory was handled previously, ComplexX87 and X87 should
    // never occur as hi classes, and X87Up must be preceed by X87,
    // which is passed in memory.
  case Memory:
  case X87:
  case ComplexX87:
    assert(0 && "Invalid classification for hi word.");
    break;

  case NoClass: break;
  case Integer:
    ResType = llvm::StructType::get(ResType, llvm::Type::Int64Ty, NULL);
    ++neededInt;
    break;

    // X87Up generally doesn't occur here (long double is passed in
    // memory), except in situations involving unions.
  case X87Up:
  case SSE:
    ResType = llvm::StructType::get(ResType, llvm::Type::DoubleTy, NULL);
    ++neededSSE;
    break;

    // AMD64-ABI 3.2.3p3: Rule 4. If the class is SSEUP, the
    // eightbyte is passed in the upper half of the last used SSE
    // register.
  case SSEUp:
    assert(Lo == SSE && "Unexpected SSEUp classification.");
    ResType = llvm::VectorType::get(llvm::Type::DoubleTy, 2);
    break;
  }

  return getCoerceResult(Ty, ResType, Context);
}

void X86_64ABIInfo::computeInfo(CGFunctionInfo &FI, ASTContext &Context) const {
  FI.getReturnInfo() = classifyReturnType(FI.getReturnType(), Context);

  // Keep track of the number of assigned registers.
  unsigned freeIntRegs = 6, freeSSERegs = 8;

  // AMD64-ABI 3.2.3p3: Once arguments are classified, the registers
  // get assigned (in left-to-right order) for passing as follows...
  for (CGFunctionInfo::arg_iterator it = FI.arg_begin(), ie = FI.arg_end();
       it != ie; ++it) {
    unsigned neededInt, neededSSE;
    it->info = classifyArgumentType(it->type, Context, neededInt, neededSSE);

    // AMD64-ABI 3.2.3p3: If there are no registers available for any
    // eightbyte of an argument, the whole argument is passed on the
    // stack. If registers have already been assigned for some
    // eightbytes of such an argument, the assignments get reverted.
    if (freeIntRegs >= neededInt && freeSSERegs >= neededSSE) {
      freeIntRegs -= neededInt;
      freeSSERegs -= neededSSE;
    } else {
      it->info = ABIArgInfo::getIndirect(0);
    }
  }
}

static llvm::Value *EmitVAArgFromMemory(llvm::Value *VAListAddr, 
                                        QualType Ty,
                                        CodeGenFunction &CGF) {
  llvm::Value *overflow_arg_area_p = 
    CGF.Builder.CreateStructGEP(VAListAddr, 2, "overflow_arg_area_p");
  llvm::Value *overflow_arg_area = 
    CGF.Builder.CreateLoad(overflow_arg_area_p, "overflow_arg_area");

  // AMD64-ABI 3.5.7p5: Step 7. Align l->overflow_arg_area upwards to a 16
  // byte boundary if alignment needed by type exceeds 8 byte boundary.
  uint64_t Align = CGF.getContext().getTypeAlign(Ty) / 8;
  if (Align > 8) {
    // Note that we follow the ABI & gcc here, even though the type
    // could in theory have an alignment greater than 16. This case
    // shouldn't ever matter in practice.

    // overflow_arg_area = (overflow_arg_area + 15) & ~15;
    llvm::Value *Offset = llvm::ConstantInt::get(llvm::Type::Int32Ty, 15);
    overflow_arg_area = CGF.Builder.CreateGEP(overflow_arg_area, Offset);
    llvm::Value *AsInt = CGF.Builder.CreatePtrToInt(overflow_arg_area,
                                                    llvm::Type::Int64Ty);
    llvm::Value *Mask = llvm::ConstantInt::get(llvm::Type::Int64Ty, ~15LL);
    overflow_arg_area = 
      CGF.Builder.CreateIntToPtr(CGF.Builder.CreateAnd(AsInt, Mask),
                                 overflow_arg_area->getType(),
                                 "overflow_arg_area.align");
  }

  // AMD64-ABI 3.5.7p5: Step 8. Fetch type from l->overflow_arg_area.
  const llvm::Type *LTy = CGF.ConvertTypeForMem(Ty);
  llvm::Value *Res = 
    CGF.Builder.CreateBitCast(overflow_arg_area, 
                              llvm::PointerType::getUnqual(LTy));

  // AMD64-ABI 3.5.7p5: Step 9. Set l->overflow_arg_area to:
  // l->overflow_arg_area + sizeof(type).
  // AMD64-ABI 3.5.7p5: Step 10. Align l->overflow_arg_area upwards to
  // an 8 byte boundary.

  uint64_t SizeInBytes = (CGF.getContext().getTypeSize(Ty) + 7) / 8;
  llvm::Value *Offset = llvm::ConstantInt::get(llvm::Type::Int32Ty,
                                               (SizeInBytes + 7)  & ~7);
  overflow_arg_area = CGF.Builder.CreateGEP(overflow_arg_area, Offset,
                                            "overflow_arg_area.next");
  CGF.Builder.CreateStore(overflow_arg_area, overflow_arg_area_p);

  // AMD64-ABI 3.5.7p5: Step 11. Return the fetched type.  
  return Res;
}

llvm::Value *X86_64ABIInfo::EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                      CodeGenFunction &CGF) const {
  // Assume that va_list type is correct; should be pointer to LLVM type:
  // struct {
  //   i32 gp_offset;
  //   i32 fp_offset;
  //   i8* overflow_arg_area;
  //   i8* reg_save_area;
  // }; 
  unsigned neededInt, neededSSE;
  ABIArgInfo AI = classifyArgumentType(Ty, CGF.getContext(), 
                                       neededInt, neededSSE);

  // AMD64-ABI 3.5.7p5: Step 1. Determine whether type may be passed
  // in the registers. If not go to step 7.
  if (!neededInt && !neededSSE)
    return EmitVAArgFromMemory(VAListAddr, Ty, CGF);

  // AMD64-ABI 3.5.7p5: Step 2. Compute num_gp to hold the number of
  // general purpose registers needed to pass type and num_fp to hold
  // the number of floating point registers needed.

  // AMD64-ABI 3.5.7p5: Step 3. Verify whether arguments fit into
  // registers. In the case: l->gp_offset > 48 - num_gp * 8 or
  // l->fp_offset > 304 - num_fp * 16 go to step 7.
  // 
  // NOTE: 304 is a typo, there are (6 * 8 + 8 * 16) = 176 bytes of
  // register save space).

  llvm::Value *InRegs = 0;
  llvm::Value *gp_offset_p = 0, *gp_offset = 0;
  llvm::Value *fp_offset_p = 0, *fp_offset = 0;
  if (neededInt) {
    gp_offset_p = CGF.Builder.CreateStructGEP(VAListAddr, 0, "gp_offset_p");
    gp_offset = CGF.Builder.CreateLoad(gp_offset_p, "gp_offset");
    InRegs = 
      CGF.Builder.CreateICmpULE(gp_offset,
                                llvm::ConstantInt::get(llvm::Type::Int32Ty,
                                                       48 - neededInt * 8),
                                "fits_in_gp");
  }

  if (neededSSE) {
    fp_offset_p = CGF.Builder.CreateStructGEP(VAListAddr, 1, "fp_offset_p");
    fp_offset = CGF.Builder.CreateLoad(fp_offset_p, "fp_offset");
    llvm::Value *FitsInFP = 
      CGF.Builder.CreateICmpULE(fp_offset,
                                llvm::ConstantInt::get(llvm::Type::Int32Ty,
                                                       176 - neededSSE * 16),
                                "fits_in_fp");
    InRegs = InRegs ? CGF.Builder.CreateAnd(InRegs, FitsInFP) : FitsInFP;
  }

  llvm::BasicBlock *InRegBlock = CGF.createBasicBlock("vaarg.in_reg");
  llvm::BasicBlock *InMemBlock = CGF.createBasicBlock("vaarg.in_mem");
  llvm::BasicBlock *ContBlock = CGF.createBasicBlock("vaarg.end");
  CGF.Builder.CreateCondBr(InRegs, InRegBlock, InMemBlock);
  
  // Emit code to load the value if it was passed in registers.
  
  CGF.EmitBlock(InRegBlock);

  // AMD64-ABI 3.5.7p5: Step 4. Fetch type from l->reg_save_area with
  // an offset of l->gp_offset and/or l->fp_offset. This may require
  // copying to a temporary location in case the parameter is passed
  // in different register classes or requires an alignment greater
  // than 8 for general purpose registers and 16 for XMM registers.
  //
  // FIXME: This really results in shameful code when we end up
  // needing to collect arguments from different places; often what
  // should result in a simple assembling of a structure from
  // scattered addresses has many more loads than necessary. Can we
  // clean this up?
  const llvm::Type *LTy = CGF.ConvertTypeForMem(Ty);
  llvm::Value *RegAddr = 
    CGF.Builder.CreateLoad(CGF.Builder.CreateStructGEP(VAListAddr, 3), 
                           "reg_save_area");
  if (neededInt && neededSSE) {
    // FIXME: Cleanup.
    assert(AI.isCoerce() && "Unexpected ABI info for mixed regs");
    const llvm::StructType *ST = cast<llvm::StructType>(AI.getCoerceToType());
    llvm::Value *Tmp = CGF.CreateTempAlloca(ST);
    assert(ST->getNumElements() == 2 && "Unexpected ABI info for mixed regs");
    const llvm::Type *TyLo = ST->getElementType(0);
    const llvm::Type *TyHi = ST->getElementType(1);
    assert((TyLo->isFloatingPoint() ^ TyHi->isFloatingPoint()) &&
           "Unexpected ABI info for mixed regs");
    const llvm::Type *PTyLo = llvm::PointerType::getUnqual(TyLo);
    const llvm::Type *PTyHi = llvm::PointerType::getUnqual(TyHi);
    llvm::Value *GPAddr = CGF.Builder.CreateGEP(RegAddr, gp_offset);
    llvm::Value *FPAddr = CGF.Builder.CreateGEP(RegAddr, fp_offset);
    llvm::Value *RegLoAddr = TyLo->isFloatingPoint() ? FPAddr : GPAddr;
    llvm::Value *RegHiAddr = TyLo->isFloatingPoint() ? GPAddr : FPAddr;
    llvm::Value *V = 
      CGF.Builder.CreateLoad(CGF.Builder.CreateBitCast(RegLoAddr, PTyLo));
    CGF.Builder.CreateStore(V, CGF.Builder.CreateStructGEP(Tmp, 0));
    V = CGF.Builder.CreateLoad(CGF.Builder.CreateBitCast(RegHiAddr, PTyHi));
    CGF.Builder.CreateStore(V, CGF.Builder.CreateStructGEP(Tmp, 1));
    
    RegAddr = CGF.Builder.CreateBitCast(Tmp, llvm::PointerType::getUnqual(LTy));
  } else if (neededInt) {
    RegAddr = CGF.Builder.CreateGEP(RegAddr, gp_offset);
    RegAddr = CGF.Builder.CreateBitCast(RegAddr, 
                                        llvm::PointerType::getUnqual(LTy));
  } else {
    if (neededSSE == 1) {
      RegAddr = CGF.Builder.CreateGEP(RegAddr, fp_offset);
      RegAddr = CGF.Builder.CreateBitCast(RegAddr, 
                                          llvm::PointerType::getUnqual(LTy));
    } else {
      assert(neededSSE == 2 && "Invalid number of needed registers!");
      // SSE registers are spaced 16 bytes apart in the register save
      // area, we need to collect the two eightbytes together.
      llvm::Value *RegAddrLo = CGF.Builder.CreateGEP(RegAddr, fp_offset);
      llvm::Value *RegAddrHi = 
        CGF.Builder.CreateGEP(RegAddrLo, 
                              llvm::ConstantInt::get(llvm::Type::Int32Ty, 16));
      const llvm::Type *DblPtrTy = 
        llvm::PointerType::getUnqual(llvm::Type::DoubleTy);
      const llvm::StructType *ST = llvm::StructType::get(llvm::Type::DoubleTy,
                                                         llvm::Type::DoubleTy,
                                                         NULL);
      llvm::Value *V, *Tmp = CGF.CreateTempAlloca(ST);
      V = CGF.Builder.CreateLoad(CGF.Builder.CreateBitCast(RegAddrLo, 
                                                           DblPtrTy));
      CGF.Builder.CreateStore(V, CGF.Builder.CreateStructGEP(Tmp, 0));
      V = CGF.Builder.CreateLoad(CGF.Builder.CreateBitCast(RegAddrHi, 
                                                           DblPtrTy));
      CGF.Builder.CreateStore(V, CGF.Builder.CreateStructGEP(Tmp, 1));
      RegAddr = CGF.Builder.CreateBitCast(Tmp, 
                                          llvm::PointerType::getUnqual(LTy));
    }
  }

  // AMD64-ABI 3.5.7p5: Step 5. Set: 
  // l->gp_offset = l->gp_offset + num_gp * 8 
  // l->fp_offset = l->fp_offset + num_fp * 16.
  if (neededInt) {
    llvm::Value *Offset = llvm::ConstantInt::get(llvm::Type::Int32Ty,
                                                 neededInt * 8);
    CGF.Builder.CreateStore(CGF.Builder.CreateAdd(gp_offset, Offset),
                            gp_offset_p);
  }
  if (neededSSE) {
    llvm::Value *Offset = llvm::ConstantInt::get(llvm::Type::Int32Ty,
                                                 neededSSE * 16);
    CGF.Builder.CreateStore(CGF.Builder.CreateAdd(fp_offset, Offset),
                            fp_offset_p);
  }
  CGF.EmitBranch(ContBlock);

  // Emit code to load the value if it was passed in memory.
  
  CGF.EmitBlock(InMemBlock);
  llvm::Value *MemAddr = EmitVAArgFromMemory(VAListAddr, Ty, CGF);

  // Return the appropriate result.

  CGF.EmitBlock(ContBlock);  
  llvm::PHINode *ResAddr = CGF.Builder.CreatePHI(RegAddr->getType(), 
                                                 "vaarg.addr");
  ResAddr->reserveOperandSpace(2);
  ResAddr->addIncoming(RegAddr, InRegBlock);
  ResAddr->addIncoming(MemAddr, InMemBlock);
  
  return ResAddr;
}

class ARMABIInfo : public ABIInfo {
  ABIArgInfo classifyReturnType(QualType RetTy, 
                                ASTContext &Context) const;
  
  ABIArgInfo classifyArgumentType(QualType RetTy,
                                  ASTContext &Context) const;

  virtual void computeInfo(CGFunctionInfo &FI, ASTContext &Context) const;

  virtual llvm::Value *EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                 CodeGenFunction &CGF) const;
};

void ARMABIInfo::computeInfo(CGFunctionInfo &FI, ASTContext &Context) const {
  FI.getReturnInfo() = classifyReturnType(FI.getReturnType(), Context);
  for (CGFunctionInfo::arg_iterator it = FI.arg_begin(), ie = FI.arg_end();
       it != ie; ++it) {
    it->info = classifyArgumentType(it->type, Context);
  }
}

ABIArgInfo ARMABIInfo::classifyArgumentType(QualType Ty,
                                            ASTContext &Context) const {
  if (!CodeGenFunction::hasAggregateLLVMType(Ty)) {
    return ABIArgInfo::getDirect();
  }
  // FIXME: This is kind of nasty... but there isn't much choice
  // because the ARM backend doesn't support byval.
  // FIXME: This doesn't handle alignment > 64 bits.
  const llvm::Type* ElemTy;
  unsigned SizeRegs;
  if (Context.getTypeAlign(Ty) > 32) {
    ElemTy = llvm::Type::Int64Ty;
    SizeRegs = (Context.getTypeSize(Ty) + 63) / 64;
  } else {
    ElemTy = llvm::Type::Int32Ty;
    SizeRegs = (Context.getTypeSize(Ty) + 31) / 32;
  }
  std::vector<const llvm::Type*> LLVMFields;
  LLVMFields.push_back(llvm::ArrayType::get(ElemTy, SizeRegs));
  const llvm::Type* STy = llvm::StructType::get(LLVMFields, true);
  return ABIArgInfo::getCoerce(STy);
}

ABIArgInfo ARMABIInfo::classifyReturnType(QualType RetTy,
                                          ASTContext &Context) const {
  if (RetTy->isVoidType()) {
    return ABIArgInfo::getIgnore();
  } else if (CodeGenFunction::hasAggregateLLVMType(RetTy)) {
    // Aggregates <= 4 bytes are returned in r0; other aggregates
    // are returned indirectly.
    uint64_t Size = Context.getTypeSize(RetTy);
    if (Size <= 32)
      return ABIArgInfo::getCoerce(llvm::Type::Int32Ty);
    return ABIArgInfo::getIndirect(0);
  } else {
    return ABIArgInfo::getDirect();
  }
}

llvm::Value *ARMABIInfo::EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                      CodeGenFunction &CGF) const {
  // FIXME: Need to handle alignment
  const llvm::Type *BP = llvm::PointerType::getUnqual(llvm::Type::Int8Ty);
  const llvm::Type *BPP = llvm::PointerType::getUnqual(BP);

  CGBuilderTy &Builder = CGF.Builder;
  llvm::Value *VAListAddrAsBPP = Builder.CreateBitCast(VAListAddr, BPP, 
                                                       "ap");
  llvm::Value *Addr = Builder.CreateLoad(VAListAddrAsBPP, "ap.cur");
  llvm::Type *PTy = 
    llvm::PointerType::getUnqual(CGF.ConvertType(Ty));
  llvm::Value *AddrTyped = Builder.CreateBitCast(Addr, PTy);
  
  uint64_t Offset = 
    llvm::RoundUpToAlignment(CGF.getContext().getTypeSize(Ty) / 8, 4);
  llvm::Value *NextAddr = 
    Builder.CreateGEP(Addr, 
                      llvm::ConstantInt::get(llvm::Type::Int32Ty, Offset),
                      "ap.next");
  Builder.CreateStore(NextAddr, VAListAddrAsBPP);

  return AddrTyped;
}

ABIArgInfo DefaultABIInfo::classifyReturnType(QualType RetTy,
                                              ASTContext &Context) const {
  if (RetTy->isVoidType()) {
    return ABIArgInfo::getIgnore();
  } else if (CodeGenFunction::hasAggregateLLVMType(RetTy)) {
    return ABIArgInfo::getIndirect(0);
  } else {
    return ABIArgInfo::getDirect();
  }
}

ABIArgInfo DefaultABIInfo::classifyArgumentType(QualType Ty,
                                                ASTContext &Context) const {
  if (CodeGenFunction::hasAggregateLLVMType(Ty)) {
    return ABIArgInfo::getIndirect(0);
  } else {
    return ABIArgInfo::getDirect();
  }
}

llvm::Value *DefaultABIInfo::EmitVAArg(llvm::Value *VAListAddr, QualType Ty,
                                       CodeGenFunction &CGF) const {
  return 0;
}

const ABIInfo &CodeGenTypes::getABIInfo() const {
  if (TheABIInfo)
    return *TheABIInfo;

  // For now we just cache this in the CodeGenTypes and don't bother
  // to free it.
  const char *TargetPrefix = getContext().Target.getTargetPrefix();
  if (strcmp(TargetPrefix, "x86") == 0) {
    bool IsDarwin = strstr(getContext().Target.getTargetTriple(), "darwin");
    switch (getContext().Target.getPointerWidth(0)) {
    case 32:
      return *(TheABIInfo = new X86_32ABIInfo(IsDarwin));
    case 64:
      return *(TheABIInfo = new X86_64ABIInfo());
    }
  } else if (strcmp(TargetPrefix, "arm") == 0) {
    // FIXME: Support for OABI?
    return *(TheABIInfo = new ARMABIInfo());
  }

  return *(TheABIInfo = new DefaultABIInfo);
}

/***/

CGFunctionInfo::CGFunctionInfo(QualType ResTy, 
                               const llvm::SmallVector<QualType, 16> &ArgTys) {
  NumArgs = ArgTys.size();
  Args = new ArgInfo[1 + NumArgs];
  Args[0].type = ResTy;
  for (unsigned i = 0; i < NumArgs; ++i)
    Args[1 + i].type = ArgTys[i];
}

/***/

void CodeGenTypes::GetExpandedTypes(QualType Ty, 
                                    std::vector<const llvm::Type*> &ArgTys) {
  const RecordType *RT = Ty->getAsStructureType();
  assert(RT && "Can only expand structure types.");
  const RecordDecl *RD = RT->getDecl();
  assert(!RD->hasFlexibleArrayMember() && 
         "Cannot expand structure with flexible array.");
  
  for (RecordDecl::field_iterator i = RD->field_begin(), 
         e = RD->field_end(); i != e; ++i) {
    const FieldDecl *FD = *i;
    assert(!FD->isBitField() && 
           "Cannot expand structure with bit-field members.");
    
    QualType FT = FD->getType();
    if (CodeGenFunction::hasAggregateLLVMType(FT)) {
      GetExpandedTypes(FT, ArgTys);
    } else {
      ArgTys.push_back(ConvertType(FT));
    }
  }
}

llvm::Function::arg_iterator 
CodeGenFunction::ExpandTypeFromArgs(QualType Ty, LValue LV,
                                    llvm::Function::arg_iterator AI) {
  const RecordType *RT = Ty->getAsStructureType();
  assert(RT && "Can only expand structure types.");

  RecordDecl *RD = RT->getDecl();
  assert(LV.isSimple() && 
         "Unexpected non-simple lvalue during struct expansion.");  
  llvm::Value *Addr = LV.getAddress();
  for (RecordDecl::field_iterator i = RD->field_begin(), 
         e = RD->field_end(); i != e; ++i) {
    FieldDecl *FD = *i;    
    QualType FT = FD->getType();

    // FIXME: What are the right qualifiers here?
    LValue LV = EmitLValueForField(Addr, FD, false, 0);
    if (CodeGenFunction::hasAggregateLLVMType(FT)) {
      AI = ExpandTypeFromArgs(FT, LV, AI);
    } else {
      EmitStoreThroughLValue(RValue::get(AI), LV, FT);
      ++AI;
    }
  }

  return AI;
}

void 
CodeGenFunction::ExpandTypeToArgs(QualType Ty, RValue RV, 
                                  llvm::SmallVector<llvm::Value*, 16> &Args) {
  const RecordType *RT = Ty->getAsStructureType();
  assert(RT && "Can only expand structure types.");

  RecordDecl *RD = RT->getDecl();
  assert(RV.isAggregate() && "Unexpected rvalue during struct expansion");
  llvm::Value *Addr = RV.getAggregateAddr();
  for (RecordDecl::field_iterator i = RD->field_begin(), 
         e = RD->field_end(); i != e; ++i) {
    FieldDecl *FD = *i;    
    QualType FT = FD->getType();
    
    // FIXME: What are the right qualifiers here?
    LValue LV = EmitLValueForField(Addr, FD, false, 0);
    if (CodeGenFunction::hasAggregateLLVMType(FT)) {
      ExpandTypeToArgs(FT, RValue::getAggregate(LV.getAddress()), Args);
    } else {
      RValue RV = EmitLoadOfLValue(LV, FT);
      assert(RV.isScalar() && 
             "Unexpected non-scalar rvalue during struct expansion.");
      Args.push_back(RV.getScalarVal());
    }
  }
}

/// CreateCoercedLoad - Create a load from \arg SrcPtr interpreted as
/// a pointer to an object of type \arg Ty.
///
/// This safely handles the case when the src type is smaller than the
/// destination type; in this situation the values of bits which not
/// present in the src are undefined.
static llvm::Value *CreateCoercedLoad(llvm::Value *SrcPtr,
                                      const llvm::Type *Ty,
                                      CodeGenFunction &CGF) {
  const llvm::Type *SrcTy = 
    cast<llvm::PointerType>(SrcPtr->getType())->getElementType();
  uint64_t SrcSize = CGF.CGM.getTargetData().getTypePaddedSize(SrcTy);
  uint64_t DstSize = CGF.CGM.getTargetData().getTypePaddedSize(Ty);

  // If load is legal, just bitcast the src pointer.
  if (SrcSize == DstSize) {
    llvm::Value *Casted =
      CGF.Builder.CreateBitCast(SrcPtr, llvm::PointerType::getUnqual(Ty));
    llvm::LoadInst *Load = CGF.Builder.CreateLoad(Casted);
    // FIXME: Use better alignment / avoid requiring aligned load.
    Load->setAlignment(1);
    return Load;
  } else {
    assert(SrcSize < DstSize && "Coercion is losing source bits!");

    // Otherwise do coercion through memory. This is stupid, but
    // simple.
    llvm::Value *Tmp = CGF.CreateTempAlloca(Ty);
    llvm::Value *Casted = 
      CGF.Builder.CreateBitCast(Tmp, llvm::PointerType::getUnqual(SrcTy));
    llvm::StoreInst *Store = 
      CGF.Builder.CreateStore(CGF.Builder.CreateLoad(SrcPtr), Casted);
    // FIXME: Use better alignment / avoid requiring aligned store.
    Store->setAlignment(1);
    return CGF.Builder.CreateLoad(Tmp);
  }
}

/// CreateCoercedStore - Create a store to \arg DstPtr from \arg Src,
/// where the source and destination may have different types.
///
/// This safely handles the case when the src type is larger than the
/// destination type; the upper bits of the src will be lost.
static void CreateCoercedStore(llvm::Value *Src,
                               llvm::Value *DstPtr,
                               CodeGenFunction &CGF) {
  const llvm::Type *SrcTy = Src->getType();
  const llvm::Type *DstTy = 
    cast<llvm::PointerType>(DstPtr->getType())->getElementType();

  uint64_t SrcSize = CGF.CGM.getTargetData().getTypePaddedSize(SrcTy);
  uint64_t DstSize = CGF.CGM.getTargetData().getTypePaddedSize(DstTy);

  // If store is legal, just bitcast the src pointer.
  if (SrcSize == DstSize) {
    llvm::Value *Casted =
      CGF.Builder.CreateBitCast(DstPtr, llvm::PointerType::getUnqual(SrcTy));
    // FIXME: Use better alignment / avoid requiring aligned store.
    CGF.Builder.CreateStore(Src, Casted)->setAlignment(1);
  } else {
    assert(SrcSize > DstSize && "Coercion is missing bits!");
    
    // Otherwise do coercion through memory. This is stupid, but
    // simple.
    llvm::Value *Tmp = CGF.CreateTempAlloca(SrcTy);
    CGF.Builder.CreateStore(Src, Tmp);
    llvm::Value *Casted = 
      CGF.Builder.CreateBitCast(Tmp, llvm::PointerType::getUnqual(DstTy));
    llvm::LoadInst *Load = CGF.Builder.CreateLoad(Casted);
    // FIXME: Use better alignment / avoid requiring aligned load.
    Load->setAlignment(1);
    CGF.Builder.CreateStore(Load, DstPtr);
  }
}

/***/

bool CodeGenModule::ReturnTypeUsesSret(const CGFunctionInfo &FI) {
  return FI.getReturnInfo().isIndirect();
}

const llvm::FunctionType *
CodeGenTypes::GetFunctionType(const CGFunctionInfo &FI, bool IsVariadic) {
  std::vector<const llvm::Type*> ArgTys;

  const llvm::Type *ResultType = 0;

  QualType RetTy = FI.getReturnType();
  const ABIArgInfo &RetAI = FI.getReturnInfo();
  switch (RetAI.getKind()) {
  case ABIArgInfo::Expand:
    assert(0 && "Invalid ABI kind for return argument");

  case ABIArgInfo::Direct:
    ResultType = ConvertType(RetTy);
    break;

  case ABIArgInfo::Indirect: {
    assert(!RetAI.getIndirectAlign() && "Align unused on indirect return.");
    ResultType = llvm::Type::VoidTy;
    const llvm::Type *STy = ConvertType(RetTy);
    ArgTys.push_back(llvm::PointerType::get(STy, RetTy.getAddressSpace()));
    break;
  }

  case ABIArgInfo::Ignore:
    ResultType = llvm::Type::VoidTy;
    break;

  case ABIArgInfo::Coerce:
    ResultType = RetAI.getCoerceToType();
    break;
  }
  
  for (CGFunctionInfo::const_arg_iterator it = FI.arg_begin(), 
         ie = FI.arg_end(); it != ie; ++it) {
    const ABIArgInfo &AI = it->info;
    
    switch (AI.getKind()) {
    case ABIArgInfo::Ignore:
      break;

    case ABIArgInfo::Coerce:
      ArgTys.push_back(AI.getCoerceToType());
      break;

    case ABIArgInfo::Indirect: {
      // indirect arguments are always on the stack, which is addr space #0.
      const llvm::Type *LTy = ConvertTypeForMem(it->type);
      ArgTys.push_back(llvm::PointerType::getUnqual(LTy));
      break;
    }
      
    case ABIArgInfo::Direct:
      ArgTys.push_back(ConvertType(it->type));
      break;
     
    case ABIArgInfo::Expand:
      GetExpandedTypes(it->type, ArgTys);
      break;
    }
  }

  return llvm::FunctionType::get(ResultType, ArgTys, IsVariadic);
}

void CodeGenModule::ConstructAttributeList(const CGFunctionInfo &FI,
                                           const Decl *TargetDecl,
                                           AttributeListType &PAL) {
  unsigned FuncAttrs = 0;
  unsigned RetAttrs = 0;

  if (TargetDecl) {
    if (TargetDecl->getAttr<NoThrowAttr>())
      FuncAttrs |= llvm::Attribute::NoUnwind;
    if (TargetDecl->getAttr<NoReturnAttr>())
      FuncAttrs |= llvm::Attribute::NoReturn;
    if (TargetDecl->getAttr<PureAttr>())
      FuncAttrs |= llvm::Attribute::ReadOnly;
    if (TargetDecl->getAttr<ConstAttr>())
      FuncAttrs |= llvm::Attribute::ReadNone;
  }

  QualType RetTy = FI.getReturnType();
  unsigned Index = 1;
  const ABIArgInfo &RetAI = FI.getReturnInfo();
  switch (RetAI.getKind()) {
  case ABIArgInfo::Direct:
    if (RetTy->isPromotableIntegerType()) {
      if (RetTy->isSignedIntegerType()) {
        RetAttrs |= llvm::Attribute::SExt;
      } else if (RetTy->isUnsignedIntegerType()) {
        RetAttrs |= llvm::Attribute::ZExt;
      }
    }
    break;

  case ABIArgInfo::Indirect:
    PAL.push_back(llvm::AttributeWithIndex::get(Index, 
                                                llvm::Attribute::StructRet |
                                                llvm::Attribute::NoAlias));
    ++Index;
    // sret disables readnone and readonly
    FuncAttrs &= ~(llvm::Attribute::ReadOnly |
                   llvm::Attribute::ReadNone);
    break;

  case ABIArgInfo::Ignore:
  case ABIArgInfo::Coerce:
    break;

  case ABIArgInfo::Expand:
    assert(0 && "Invalid ABI kind for return argument");    
  }

  if (RetAttrs)
    PAL.push_back(llvm::AttributeWithIndex::get(0, RetAttrs));
  for (CGFunctionInfo::const_arg_iterator it = FI.arg_begin(), 
         ie = FI.arg_end(); it != ie; ++it) {
    QualType ParamType = it->type;
    const ABIArgInfo &AI = it->info;
    unsigned Attributes = 0;
    
    switch (AI.getKind()) {
    case ABIArgInfo::Coerce:
      break;

    case ABIArgInfo::Indirect:
      Attributes |= llvm::Attribute::ByVal;
      Attributes |= 
        llvm::Attribute::constructAlignmentFromInt(AI.getIndirectAlign());
      // byval disables readnone and readonly.
      FuncAttrs &= ~(llvm::Attribute::ReadOnly |
                     llvm::Attribute::ReadNone);
      break;
      
    case ABIArgInfo::Direct:
      if (ParamType->isPromotableIntegerType()) {
        if (ParamType->isSignedIntegerType()) {
          Attributes |= llvm::Attribute::SExt;
        } else if (ParamType->isUnsignedIntegerType()) {
          Attributes |= llvm::Attribute::ZExt;
        }
      }
      break;
     
    case ABIArgInfo::Ignore:
      // Skip increment, no matching LLVM parameter.
      continue; 

    case ABIArgInfo::Expand: {
      std::vector<const llvm::Type*> Tys;  
      // FIXME: This is rather inefficient. Do we ever actually need
      // to do anything here? The result should be just reconstructed
      // on the other side, so extension should be a non-issue.
      getTypes().GetExpandedTypes(ParamType, Tys);
      Index += Tys.size();
      continue;
    }
    }
      
    if (Attributes)
      PAL.push_back(llvm::AttributeWithIndex::get(Index, Attributes));
    ++Index;
  }
  if (FuncAttrs)
    PAL.push_back(llvm::AttributeWithIndex::get(~0, FuncAttrs));
}

void CodeGenFunction::EmitFunctionProlog(const CGFunctionInfo &FI,
                                         llvm::Function *Fn,
                                         const FunctionArgList &Args) {
  // FIXME: We no longer need the types from FunctionArgList; lift up
  // and simplify.

  // Emit allocs for param decls.  Give the LLVM Argument nodes names.
  llvm::Function::arg_iterator AI = Fn->arg_begin();
  
  // Name the struct return argument.
  if (CGM.ReturnTypeUsesSret(FI)) {
    AI->setName("agg.result");
    ++AI;
  }
    
  assert(FI.arg_size() == Args.size() &&
         "Mismatch between function signature & arguments.");
  CGFunctionInfo::const_arg_iterator info_it = FI.arg_begin();
  for (FunctionArgList::const_iterator i = Args.begin(), e = Args.end();
       i != e; ++i, ++info_it) {
    const VarDecl *Arg = i->first;
    QualType Ty = info_it->type;
    const ABIArgInfo &ArgI = info_it->info;

    switch (ArgI.getKind()) {
    case ABIArgInfo::Indirect: {
      llvm::Value* V = AI;
      if (hasAggregateLLVMType(Ty)) {
        // Do nothing, aggregates and complex variables are accessed by
        // reference.
      } else {
        // Load scalar value from indirect argument.
        V = EmitLoadOfScalar(V, false, Ty);
        if (!getContext().typesAreCompatible(Ty, Arg->getType())) {
          // This must be a promotion, for something like
          // "void a(x) short x; {..."
          V = EmitScalarConversion(V, Ty, Arg->getType());
        }
      }
      EmitParmDecl(*Arg, V);      
      break;
    }
      
    case ABIArgInfo::Direct: {
      assert(AI != Fn->arg_end() && "Argument mismatch!");
      llvm::Value* V = AI;
      if (hasAggregateLLVMType(Ty)) {
        // Create a temporary alloca to hold the argument; the rest of
        // codegen expects to access aggregates & complex values by
        // reference.
        V = CreateTempAlloca(ConvertTypeForMem(Ty));
        Builder.CreateStore(AI, V);
      } else {
        if (!getContext().typesAreCompatible(Ty, Arg->getType())) {
          // This must be a promotion, for something like
          // "void a(x) short x; {..."
          V = EmitScalarConversion(V, Ty, Arg->getType());
        }
      }
      EmitParmDecl(*Arg, V);
      break;
    }
      
    case ABIArgInfo::Expand: {
      // If this structure was expanded into multiple arguments then
      // we need to create a temporary and reconstruct it from the
      // arguments.
      std::string Name = Arg->getNameAsString();
      llvm::Value *Temp = CreateTempAlloca(ConvertTypeForMem(Ty), 
                                           (Name + ".addr").c_str());
      // FIXME: What are the right qualifiers here?
      llvm::Function::arg_iterator End = 
        ExpandTypeFromArgs(Ty, LValue::MakeAddr(Temp,0), AI);      
      EmitParmDecl(*Arg, Temp);

      // Name the arguments used in expansion and increment AI.
      unsigned Index = 0;
      for (; AI != End; ++AI, ++Index)
        AI->setName(Name + "." + llvm::utostr(Index));
      continue;
    }

    case ABIArgInfo::Ignore:
      // Initialize the local variable appropriately.
      if (hasAggregateLLVMType(Ty)) { 
        EmitParmDecl(*Arg, CreateTempAlloca(ConvertTypeForMem(Ty)));
      } else {
        EmitParmDecl(*Arg, llvm::UndefValue::get(ConvertType(Arg->getType())));
      }
      
      // Skip increment, no matching LLVM parameter.
      continue; 

    case ABIArgInfo::Coerce: {
      assert(AI != Fn->arg_end() && "Argument mismatch!");
      // FIXME: This is very wasteful; EmitParmDecl is just going to
      // drop the result in a new alloca anyway, so we could just
      // store into that directly if we broke the abstraction down
      // more.
      llvm::Value *V = CreateTempAlloca(ConvertTypeForMem(Ty), "coerce");
      CreateCoercedStore(AI, V, *this);
      // Match to what EmitParmDecl is expecting for this type.
      if (!CodeGenFunction::hasAggregateLLVMType(Ty)) {
        V = EmitLoadOfScalar(V, false, Ty);
        if (!getContext().typesAreCompatible(Ty, Arg->getType())) {
          // This must be a promotion, for something like
          // "void a(x) short x; {..."
          V = EmitScalarConversion(V, Ty, Arg->getType());
        }
      }
      EmitParmDecl(*Arg, V);
      break;
    }
    }

    ++AI;
  }
  assert(AI == Fn->arg_end() && "Argument mismatch!");
}

void CodeGenFunction::EmitFunctionEpilog(const CGFunctionInfo &FI,
                                         llvm::Value *ReturnValue) {
  llvm::Value *RV = 0;

  // Functions with no result always return void.
  if (ReturnValue) { 
    QualType RetTy = FI.getReturnType();
    const ABIArgInfo &RetAI = FI.getReturnInfo();
    
    switch (RetAI.getKind()) {
    case ABIArgInfo::Indirect:
      if (RetTy->isAnyComplexType()) {
        ComplexPairTy RT = LoadComplexFromAddr(ReturnValue, false);
        StoreComplexToAddr(RT, CurFn->arg_begin(), false);
      } else if (CodeGenFunction::hasAggregateLLVMType(RetTy)) {
        EmitAggregateCopy(CurFn->arg_begin(), ReturnValue, RetTy);
      } else {
        EmitStoreOfScalar(Builder.CreateLoad(ReturnValue), CurFn->arg_begin(), 
                          false);
      }
      break;

    case ABIArgInfo::Direct:
      // The internal return value temp always will have
      // pointer-to-return-type type.
      RV = Builder.CreateLoad(ReturnValue);
      break;

    case ABIArgInfo::Ignore:
      break;
      
    case ABIArgInfo::Coerce:
      RV = CreateCoercedLoad(ReturnValue, RetAI.getCoerceToType(), *this);
      break;

    case ABIArgInfo::Expand:
      assert(0 && "Invalid ABI kind for return argument");    
    }
  }
  
  if (RV) {
    Builder.CreateRet(RV);
  } else {
    Builder.CreateRetVoid();
  }
}

RValue CodeGenFunction::EmitCall(const CGFunctionInfo &CallInfo,
                                 llvm::Value *Callee, 
                                 const CallArgList &CallArgs,
                                 const Decl *TargetDecl) {
  // FIXME: We no longer need the types from CallArgs; lift up and
  // simplify.
  llvm::SmallVector<llvm::Value*, 16> Args;

  // Handle struct-return functions by passing a pointer to the
  // location that we would like to return into.
  QualType RetTy = CallInfo.getReturnType();
  const ABIArgInfo &RetAI = CallInfo.getReturnInfo();
  if (CGM.ReturnTypeUsesSret(CallInfo)) {
    // Create a temporary alloca to hold the result of the call. :(
    Args.push_back(CreateTempAlloca(ConvertTypeForMem(RetTy)));
  }
  
  assert(CallInfo.arg_size() == CallArgs.size() &&
         "Mismatch between function signature & arguments.");
  CGFunctionInfo::const_arg_iterator info_it = CallInfo.arg_begin();
  for (CallArgList::const_iterator I = CallArgs.begin(), E = CallArgs.end(); 
       I != E; ++I, ++info_it) {
    const ABIArgInfo &ArgInfo = info_it->info;
    RValue RV = I->first;

    switch (ArgInfo.getKind()) {
    case ABIArgInfo::Indirect:
      if (RV.isScalar() || RV.isComplex()) {
        // Make a temporary alloca to pass the argument.
        Args.push_back(CreateTempAlloca(ConvertTypeForMem(I->second)));
        if (RV.isScalar())
          EmitStoreOfScalar(RV.getScalarVal(), Args.back(), false);
        else
          StoreComplexToAddr(RV.getComplexVal(), Args.back(), false); 
      } else {
        Args.push_back(RV.getAggregateAddr());
      }
      break;

    case ABIArgInfo::Direct:
      if (RV.isScalar()) {
        Args.push_back(RV.getScalarVal());
      } else if (RV.isComplex()) {
        llvm::Value *Tmp = llvm::UndefValue::get(ConvertType(I->second));
        Tmp = Builder.CreateInsertValue(Tmp, RV.getComplexVal().first, 0);
        Tmp = Builder.CreateInsertValue(Tmp, RV.getComplexVal().second, 1);
        Args.push_back(Tmp);
      } else {
        Args.push_back(Builder.CreateLoad(RV.getAggregateAddr()));
      }
      break;
     
    case ABIArgInfo::Ignore:
      break;

    case ABIArgInfo::Coerce: {
      // FIXME: Avoid the conversion through memory if possible.
      llvm::Value *SrcPtr;
      if (RV.isScalar()) {
        SrcPtr = CreateTempAlloca(ConvertTypeForMem(I->second), "coerce");
        EmitStoreOfScalar(RV.getScalarVal(), SrcPtr, false);
      } else if (RV.isComplex()) {
        SrcPtr = CreateTempAlloca(ConvertTypeForMem(I->second), "coerce");
        StoreComplexToAddr(RV.getComplexVal(), SrcPtr, false);
      } else 
        SrcPtr = RV.getAggregateAddr();
      Args.push_back(CreateCoercedLoad(SrcPtr, ArgInfo.getCoerceToType(), 
                                       *this));
      break;
    }

    case ABIArgInfo::Expand:
      ExpandTypeToArgs(I->second, RV, Args);
      break;
    }
  }

  llvm::BasicBlock *InvokeDest = getInvokeDest();
  CodeGen::AttributeListType AttributeList;
  CGM.ConstructAttributeList(CallInfo, TargetDecl, AttributeList);
  llvm::AttrListPtr Attrs = llvm::AttrListPtr::get(AttributeList.begin(),
                                                   AttributeList.end());
  
  llvm::CallSite CS;
  if (!InvokeDest || (Attrs.getFnAttributes() & llvm::Attribute::NoUnwind)) {
    CS = Builder.CreateCall(Callee, &Args[0], &Args[0]+Args.size());
  } else {
    llvm::BasicBlock *Cont = createBasicBlock("invoke.cont");
    CS = Builder.CreateInvoke(Callee, Cont, InvokeDest, 
                              &Args[0], &Args[0]+Args.size());
    EmitBlock(Cont);
  }

  CS.setAttributes(Attrs);
  if (const llvm::Function *F = dyn_cast<llvm::Function>(Callee))
    CS.setCallingConv(F->getCallingConv());

  // If the call doesn't return, finish the basic block and clear the
  // insertion point; this allows the rest of IRgen to discard
  // unreachable code.
  if (CS.doesNotReturn()) {
    Builder.CreateUnreachable();
    Builder.ClearInsertionPoint();
    
    // FIXME: For now, emit a dummy basic block because expr
    // emitters in generally are not ready to handle emitting
    // expressions at unreachable points.
    EnsureInsertPoint();
    
    // Return a reasonable RValue.
    return GetUndefRValue(RetTy);
  }    

  llvm::Instruction *CI = CS.getInstruction();
  if (Builder.isNamePreserving() && CI->getType() != llvm::Type::VoidTy)
    CI->setName("call");

  switch (RetAI.getKind()) {
  case ABIArgInfo::Indirect:
    if (RetTy->isAnyComplexType())
      return RValue::getComplex(LoadComplexFromAddr(Args[0], false));
    if (CodeGenFunction::hasAggregateLLVMType(RetTy))
      return RValue::getAggregate(Args[0]);
    return RValue::get(EmitLoadOfScalar(Args[0], false, RetTy));

  case ABIArgInfo::Direct:
    if (RetTy->isAnyComplexType()) {
      llvm::Value *Real = Builder.CreateExtractValue(CI, 0);
      llvm::Value *Imag = Builder.CreateExtractValue(CI, 1);
      return RValue::getComplex(std::make_pair(Real, Imag));
    }
    if (CodeGenFunction::hasAggregateLLVMType(RetTy)) {
      llvm::Value *V = CreateTempAlloca(ConvertTypeForMem(RetTy), "agg.tmp");
      Builder.CreateStore(CI, V);
      return RValue::getAggregate(V);
    }
    return RValue::get(CI);

  case ABIArgInfo::Ignore:
    // If we are ignoring an argument that had a result, make sure to
    // construct the appropriate return value for our caller.
    return GetUndefRValue(RetTy);

  case ABIArgInfo::Coerce: {
    // FIXME: Avoid the conversion through memory if possible.
    llvm::Value *V = CreateTempAlloca(ConvertTypeForMem(RetTy), "coerce");
    CreateCoercedStore(CI, V, *this);
    if (RetTy->isAnyComplexType())
      return RValue::getComplex(LoadComplexFromAddr(V, false));
    if (CodeGenFunction::hasAggregateLLVMType(RetTy))
      return RValue::getAggregate(V);
    return RValue::get(EmitLoadOfScalar(V, false, RetTy));
  }

  case ABIArgInfo::Expand:
    assert(0 && "Invalid ABI kind for return argument");    
  }

  assert(0 && "Unhandled ABIArgInfo::Kind");
  return RValue::get(0);
}

/* VarArg handling */

llvm::Value *CodeGenFunction::EmitVAArg(llvm::Value *VAListAddr, QualType Ty) {
  return CGM.getTypes().getABIInfo().EmitVAArg(VAListAddr, Ty, *this);
}
