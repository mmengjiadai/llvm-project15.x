//===- VectorOps.cpp - MLIR Vector Dialect Operations ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements convenience types for working with super-vectorization
// operations, in particular super-vector loads and stores.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Vector/IR/VectorOps.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Utils/IndexingUtils.h"
#include "mlir/Dialect/Utils/StructuredOpsUtils.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/bit.h"

#include <cassert>
#include <cstdint>
#include <numeric>

#include "mlir/Dialect/Vector/IR/VectorOpsDialect.cpp.inc"
// Pull in all enum type and utility function definitions.
#include "mlir/Dialect/Vector/IR/VectorOpsEnums.cpp.inc"

using namespace mlir;
using namespace mlir::vector;

/// Helper enum to classify mask value.
enum class MaskFormat {
  AllTrue = 0,
  AllFalse = 1,
  Unknown = 2,
};

/// Helper method to classify a mask value. Currently, the method
/// looks "under the hood" of a constant value with dense attributes
/// and a constant mask operation (since the client may be called at
/// various stages during progressive lowering).
static MaskFormat getMaskFormat(Value mask) {
  if (auto c = mask.getDefiningOp<arith::ConstantOp>()) {
    // Inspect constant dense values. We count up for bits that
    // are set, count down for bits that are cleared, and bail
    // when a mix is detected.
    if (auto denseElts = llvm::dyn_cast<DenseIntElementsAttr>(c.getValue())) {
      int64_t val = 0;
      for (bool b : denseElts.getValues<bool>())
        if (b && val >= 0)
          val++;
        else if (!b && val <= 0)
          val--;
        else
          return MaskFormat::Unknown;
      if (val > 0)
        return MaskFormat::AllTrue;
      if (val < 0)
        return MaskFormat::AllFalse;
    }
  } else if (auto m = mask.getDefiningOp<ConstantMaskOp>()) {
    // Inspect constant mask index. If the index exceeds the
    // dimension size, all bits are set. If the index is zero
    // or less, no bits are set.
    ArrayAttr masks = m.getMaskDimSizes();
    auto shape = m.getType().getShape();
    bool allTrue = true;
    bool allFalse = true;
    for (auto [maskIdx, dimSize] : llvm::zip_equal(masks, shape)) {
      int64_t i = llvm::cast<IntegerAttr>(maskIdx).getInt();
      if (i < dimSize)
        allTrue = false;
      if (i > 0)
        allFalse = false;
    }
    if (allTrue)
      return MaskFormat::AllTrue;
    if (allFalse)
      return MaskFormat::AllFalse;
  }
  return MaskFormat::Unknown;
}

/// Default callback to build a region with a 'vector.yield' terminator with no
/// arguments.
void mlir::vector::buildTerminatedBody(OpBuilder &builder, Location loc) {
  builder.create<vector::YieldOp>(loc);
}

// Helper for verifying combining kinds in contractions and reductions.
static bool isSupportedCombiningKind(CombiningKind combiningKind,
                                     Type elementType) {
  switch (combiningKind) {
  case CombiningKind::ADD:
  case CombiningKind::MUL:
    return elementType.isIntOrIndexOrFloat();
  case CombiningKind::MINUI:
  case CombiningKind::MINSI:
  case CombiningKind::MAXUI:
  case CombiningKind::MAXSI:
  case CombiningKind::AND:
  case CombiningKind::OR:
  case CombiningKind::XOR:
    return elementType.isIntOrIndex();
  case CombiningKind::MINF:
  case CombiningKind::MAXF:
    return llvm::isa<FloatType>(elementType);
  }
  return false;
}

AffineMap mlir::vector::getTransferMinorIdentityMap(ShapedType shapedType,
                                                    VectorType vectorType) {
  int64_t elementVectorRank = 0;
  VectorType elementVectorType =
      llvm::dyn_cast<VectorType>(shapedType.getElementType());
  if (elementVectorType)
    elementVectorRank += elementVectorType.getRank();
  // 0-d transfers are to/from tensor<t>/memref<t> and vector<1xt>.
  // TODO: replace once we have 0-d vectors.
  if (shapedType.getRank() == 0 &&
      vectorType.getShape() == ArrayRef<int64_t>{1})
    return AffineMap::get(
        /*numDims=*/0, /*numSymbols=*/0,
        getAffineConstantExpr(0, shapedType.getContext()));
  return AffineMap::getMinorIdentityMap(
      shapedType.getRank(), vectorType.getRank() - elementVectorRank,
      shapedType.getContext());
}

bool mlir::vector::checkSameValueRAW(vector::TransferWriteOp defWrite,
                                     vector::TransferReadOp read) {
  return !defWrite.hasOutOfBoundsDim() && !defWrite.getMask() &&
         !read.getMask() && defWrite.getIndices() == read.getIndices() &&
         defWrite.getVectorType() == read.getVectorType() &&
         defWrite.getPermutationMap() == read.getPermutationMap();
}

bool mlir::vector::checkSameValueWAW(vector::TransferWriteOp write,
                                     vector::TransferWriteOp priorWrite) {
  return priorWrite.getIndices() == write.getIndices() &&
         priorWrite.getMask() == write.getMask() &&
         priorWrite.getVectorType() == write.getVectorType() &&
         priorWrite.getPermutationMap() == write.getPermutationMap();
}

bool mlir::vector::isDisjointTransferIndices(
    VectorTransferOpInterface transferA, VectorTransferOpInterface transferB) {
  // For simplicity only look at transfer of same type.
  if (transferA.getVectorType() != transferB.getVectorType())
    return false;
  unsigned rankOffset = transferA.getLeadingShapedRank();
  for (unsigned i = 0, e = transferA.indices().size(); i < e; i++) {
    auto indexA = transferA.indices()[i].getDefiningOp<arith::ConstantOp>();
    auto indexB = transferB.indices()[i].getDefiningOp<arith::ConstantOp>();
    // If any of the indices are dynamic we cannot prove anything.
    if (!indexA || !indexB)
      continue;

    if (i < rankOffset) {
      // For leading dimensions, if we can prove that index are different we
      // know we are accessing disjoint slices.
      if (llvm::cast<IntegerAttr>(indexA.getValue()).getInt() !=
          llvm::cast<IntegerAttr>(indexB.getValue()).getInt())
        return true;
    } else {
      // For this dimension, we slice a part of the memref we need to make sure
      // the intervals accessed don't overlap.
      int64_t distance =
          std::abs(llvm::cast<IntegerAttr>(indexA.getValue()).getInt() -
                   llvm::cast<IntegerAttr>(indexB.getValue()).getInt());
      if (distance >= transferA.getVectorType().getDimSize(i - rankOffset))
        return true;
    }
  }
  return false;
}

bool mlir::vector::isDisjointTransferSet(VectorTransferOpInterface transferA,
                                         VectorTransferOpInterface transferB) {
  if (transferA.source() != transferB.source())
    return false;
  return isDisjointTransferIndices(transferA, transferB);
}

// Helper to iterate over n-D vector slice elements. Calculate the next
// `position` in the n-D vector of size `shape`, applying an offset `offsets`.
// Modifies the `position` in place. Returns a failure when `position` becomes
// the end position.
static LogicalResult incSlicePosition(MutableArrayRef<int64_t> position,
                                      ArrayRef<int64_t> shape,
                                      ArrayRef<int64_t> offsets) {
  for (auto [posInDim, dimSize, offsetInDim] :
       llvm::reverse(llvm::zip_equal(position, shape, offsets))) {
    ++posInDim;
    if (posInDim < dimSize + offsetInDim)
      return success();

    // Carry the overflow to the next loop iteration.
    posInDim = offsetInDim;
  }

  return failure();
}

//===----------------------------------------------------------------------===//
// CombiningKindAttr
//===----------------------------------------------------------------------===//

namespace mlir {
namespace vector {
namespace detail {
struct BitmaskEnumStorage : public AttributeStorage {
  using KeyTy = uint64_t;

  BitmaskEnumStorage(KeyTy val) : value(val) {}

  bool operator==(const KeyTy &key) const { return value == key; }

  static BitmaskEnumStorage *construct(AttributeStorageAllocator &allocator,
                                       const KeyTy &key) {
    return new (allocator.allocate<BitmaskEnumStorage>())
        BitmaskEnumStorage(key);
  }

  KeyTy value = 0;
};
} // namespace detail
} // namespace vector
} // namespace mlir

//===----------------------------------------------------------------------===//
// VectorDialect
//===----------------------------------------------------------------------===//

void VectorDialect::initialize() {
  addAttributes<
#define GET_ATTRDEF_LIST
#include "mlir/Dialect/Vector/IR/VectorOpsAttrDefs.cpp.inc"
      >();

  addOperations<
#define GET_OP_LIST
#include "mlir/Dialect/Vector/IR/VectorOps.cpp.inc"
      >();
}

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *VectorDialect::materializeConstant(OpBuilder &builder,
                                              Attribute value, Type type,
                                              Location loc) {
  return arith::ConstantOp::materialize(builder, value, type, loc);
}

IntegerType vector::getVectorSubscriptType(Builder &builder) {
  return builder.getIntegerType(64);
}

ArrayAttr vector::getVectorSubscriptAttr(Builder &builder,
                                         ArrayRef<int64_t> values) {
  return builder.getI64ArrayAttr(values);
}

//===----------------------------------------------------------------------===//
// MultiDimReductionOp
//===----------------------------------------------------------------------===//

void vector::MultiDimReductionOp::build(OpBuilder &builder,
                                        OperationState &result, Value source,
                                        Value acc, ArrayRef<bool> reductionMask,
                                        CombiningKind kind) {
  SmallVector<int64_t> reductionDims;
  for (const auto &en : llvm::enumerate(reductionMask))
    if (en.value())
      reductionDims.push_back(en.index());
  build(builder, result, kind, source, acc,
        builder.getI64ArrayAttr(reductionDims));
}

OpFoldResult MultiDimReductionOp::fold(FoldAdaptor adaptor) {
  // Single parallel dim, this is a noop.
  if (getSourceVectorType().getRank() == 1 && !isReducedDim(0))
    return getSource();
  return {};
}

std::optional<SmallVector<int64_t, 4>>
MultiDimReductionOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getSourceVectorType().getShape());
}

LogicalResult MultiDimReductionOp::verify() {
  SmallVector<int64_t> targetShape;
  SmallVector<bool> scalableDims;
  Type inferredReturnType;
  auto sourceScalableDims = getSourceVectorType().getScalableDims();
  for (auto it : llvm::enumerate(getSourceVectorType().getShape()))
    if (!llvm::any_of(getReductionDims().getValue(), [&](Attribute attr) {
          return llvm::cast<IntegerAttr>(attr).getValue() == it.index();
        })) {
      targetShape.push_back(it.value());
      scalableDims.push_back(sourceScalableDims[it.index()]);
    }
  // TODO: update to also allow 0-d vectors when available.
  if (targetShape.empty())
    inferredReturnType = getSourceVectorType().getElementType();
  else
    inferredReturnType = VectorType::get(
        targetShape, getSourceVectorType().getElementType(), scalableDims);
  if (getType() != inferredReturnType)
    return emitOpError() << "destination type " << getType()
                         << " is incompatible with source type "
                         << getSourceVectorType();

  return success();
}

/// Returns the mask type expected by this operation.
Type MultiDimReductionOp::getExpectedMaskType() {
  auto vecType = getSourceVectorType();
  return VectorType::get(vecType.getShape(),
                         IntegerType::get(vecType.getContext(), /*width=*/1),
                         vecType.getScalableDims());
}

namespace {
// Only unit dimensions that are being reduced are folded. If the dimension is
// unit, but not reduced, it is not folded, thereby keeping the output type the
// same. If not all dimensions which are reduced are of unit dimension, this
// transformation does nothing. This is just a generalization of
// ElideSingleElementReduction for ReduceOp.
struct ElideUnitDimsInMultiDimReduction
    : public OpRewritePattern<MultiDimReductionOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(MultiDimReductionOp reductionOp,
                                PatternRewriter &rewriter) const override {
    ArrayRef<int64_t> shape = reductionOp.getSourceVectorType().getShape();
    for (const auto &dim : enumerate(shape)) {
      if (reductionOp.isReducedDim(dim.index()) && dim.value() != 1)
        return failure();
    }

    // Vector mask setup.
    OpBuilder::InsertionGuard guard(rewriter);
    Operation *rootOp;
    Value mask;
    if (reductionOp.isMasked()) {
      rewriter.setInsertionPoint(reductionOp.getMaskingOp());
      rootOp = reductionOp.getMaskingOp();
      mask = reductionOp.getMaskingOp().getMask();
    } else {
      rootOp = reductionOp;
    }

    Location loc = reductionOp.getLoc();
    Value acc = reductionOp.getAcc();
    Value cast;
    if (auto dstVecType = dyn_cast<VectorType>(reductionOp.getDestType())) {
      if (mask) {
        VectorType newMaskType =
            VectorType::get(dstVecType.getShape(), rewriter.getI1Type());
        mask = rewriter.create<vector::ShapeCastOp>(loc, newMaskType, mask);
      }
      cast = rewriter.create<vector::ShapeCastOp>(
          loc, reductionOp.getDestType(), reductionOp.getSource());
    } else {
      // This means we are reducing all the dimensions, and all reduction
      // dimensions are of size 1. So a simple extraction would do.
      SmallVector<int64_t> zeroAttr(shape.size(), 0);
      if (mask)
        mask = rewriter.create<vector::ExtractOp>(loc, rewriter.getI1Type(),
                                                  mask, zeroAttr);
      cast = rewriter.create<vector::ExtractOp>(
          loc, reductionOp.getDestType(), reductionOp.getSource(), zeroAttr);
    }

    Value result = vector::makeArithReduction(
        rewriter, loc, reductionOp.getKind(), acc, cast, mask);
    rewriter.replaceOp(rootOp, result);
    return success();
  }
};
} // namespace

void MultiDimReductionOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.add<ElideUnitDimsInMultiDimReduction>(context);
}

//===----------------------------------------------------------------------===//
// ReductionOp
//===----------------------------------------------------------------------===//

void vector::ReductionOp::build(OpBuilder &builder, OperationState &result,
                                CombiningKind kind, Value vector) {
  build(builder, result, kind, vector, /*acc=*/Value());
}

void vector::ReductionOp::build(OpBuilder &builder, OperationState &result,
                                CombiningKind kind, Value vector, Value acc) {
  build(builder, result,
        llvm::cast<VectorType>(vector.getType()).getElementType(), kind, vector,
        acc);
}

LogicalResult ReductionOp::verify() {
  // Verify for 0-D and 1-D vector.
  int64_t rank = getSourceVectorType().getRank();
  if (rank > 1)
    return emitOpError("unsupported reduction rank: ") << rank;

  // Verify supported reduction kind.
  Type eltType = getDest().getType();
  if (!isSupportedCombiningKind(getKind(), eltType))
    return emitOpError("unsupported reduction type '")
           << eltType << "' for kind '" << stringifyCombiningKind(getKind())
           << "'";

  return success();
}

ParseResult ReductionOp::parse(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::UnresolvedOperand, 2> operandsInfo;
  Type redType;
  Type resType;
  CombiningKindAttr kindAttr;
  if (parser.parseCustomAttributeWithFallback(kindAttr, Type{}, "kind",
                                              result.attributes) ||
      parser.parseComma() || parser.parseOperandList(operandsInfo) ||
      parser.parseColonType(redType) ||
      parser.parseKeywordType("into", resType) ||
      (!operandsInfo.empty() &&
       parser.resolveOperand(operandsInfo[0], redType, result.operands)) ||
      (operandsInfo.size() > 1 &&
       parser.resolveOperand(operandsInfo[1], resType, result.operands)) ||
      parser.addTypeToList(resType, result.types))
    return failure();
  if (operandsInfo.empty() || operandsInfo.size() > 2)
    return parser.emitError(parser.getNameLoc(),
                            "unsupported number of operands");
  return success();
}

void ReductionOp::print(OpAsmPrinter &p) {
  p << " ";
  getKindAttr().print(p);
  p << ", " << getVector();
  if (getAcc())
    p << ", " << getAcc();
  p << " : " << getVector().getType() << " into " << getDest().getType();
}

// MaskableOpInterface methods.

/// Returns the mask type expected by this operation.
Type ReductionOp::getExpectedMaskType() {
  auto vecType = getSourceVectorType();
  return VectorType::get(vecType.getShape(),
                         IntegerType::get(vecType.getContext(), /*width=*/1),
                         vecType.getScalableDims());
}

Value mlir::vector::getVectorReductionOp(arith::AtomicRMWKind op,
                                         OpBuilder &builder, Location loc,
                                         Value vector) {
  switch (op) {
  case arith::AtomicRMWKind::addf:
  case arith::AtomicRMWKind::addi:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::ADD, vector);
  case arith::AtomicRMWKind::mulf:
  case arith::AtomicRMWKind::muli:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MUL, vector);
  case arith::AtomicRMWKind::minf:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MINF, vector);
  case arith::AtomicRMWKind::mins:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MINSI, vector);
  case arith::AtomicRMWKind::minu:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MINUI, vector);
  case arith::AtomicRMWKind::maxf:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MAXF, vector);
  case arith::AtomicRMWKind::maxs:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MAXSI, vector);
  case arith::AtomicRMWKind::maxu:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::MAXUI, vector);
  case arith::AtomicRMWKind::andi:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::AND, vector);
  case arith::AtomicRMWKind::ori:
    return builder.create<vector::ReductionOp>(vector.getLoc(),
                                               CombiningKind::OR, vector);
  // TODO: Add remaining reduction operations.
  default:
    (void)emitOptionalError(loc, "Reduction operation type not supported");
    break;
  }
  return nullptr;
}

std::optional<SmallVector<int64_t, 4>> ReductionOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getSourceVectorType().getShape());
}

namespace {
struct ElideSingleElementReduction : public OpRewritePattern<ReductionOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ReductionOp reductionOp,
                                PatternRewriter &rewriter) const override {
    // Vector mask setup.
    OpBuilder::InsertionGuard guard(rewriter);
    auto maskableOp =
        cast<vector::MaskableOpInterface>(reductionOp.getOperation());
    Operation *rootOp;
    Value mask;
    if (maskableOp.isMasked()) {
      rewriter.setInsertionPoint(maskableOp.getMaskingOp());
      rootOp = maskableOp.getMaskingOp();
      mask = maskableOp.getMaskingOp().getMask();
    } else {
      rootOp = reductionOp;
    }

    auto vectorType = reductionOp.getSourceVectorType();
    if (vectorType.getRank() != 0 && vectorType.getDimSize(0) != 1)
      return failure();

    Location loc = reductionOp.getLoc();
    Value result;
    if (vectorType.getRank() == 0) {
      if (mask)
        mask = rewriter.create<ExtractElementOp>(loc, mask);
      result = rewriter.create<ExtractElementOp>(loc, reductionOp.getVector());
    } else {
      if (mask) {
        mask = rewriter.create<ExtractOp>(loc, rewriter.getI1Type(), mask, 0);
      }
      result = rewriter.create<ExtractOp>(loc, reductionOp.getType(),
                                          reductionOp.getVector(), 0);
    }

    if (Value acc = reductionOp.getAcc())
      result = vector::makeArithReduction(rewriter, loc, reductionOp.getKind(),
                                          result, acc, mask);

    rewriter.replaceOp(rootOp, result);
    return success();
  }
};
} // namespace

void ReductionOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                              MLIRContext *context) {
  results.add<ElideSingleElementReduction>(context);
}

//===----------------------------------------------------------------------===//
// ContractionOp
//===----------------------------------------------------------------------===//

void vector::ContractionOp::build(OpBuilder &builder, OperationState &result,
                                  Value lhs, Value rhs, Value acc,
                                  ArrayRef<ArrayRef<AffineExpr>> indexingExprs,
                                  ArrayRef<IteratorType> iteratorTypes) {
  result.addOperands({lhs, rhs, acc});
  result.addTypes(acc.getType());
  result.addAttribute(getIndexingMapsAttrName(result.name),
                      builder.getAffineMapArrayAttr(
                          AffineMap::inferFromExprList(indexingExprs)));
  result.addAttribute(
      getIteratorTypesAttrName(result.name),
      builder.getArrayAttr(llvm::to_vector(llvm::map_range(
          iteratorTypes, [&](IteratorType t) -> mlir::Attribute {
            return IteratorTypeAttr::get(builder.getContext(), t);
          }))));
}

void vector::ContractionOp::build(OpBuilder &builder, OperationState &result,
                                  Value lhs, Value rhs, Value acc,
                                  ArrayAttr indexingMaps,
                                  ArrayAttr iteratorTypes) {
  build(builder, result, lhs, rhs, acc, indexingMaps, iteratorTypes,
        ContractionOp::getDefaultKind());
}

void vector::ContractionOp::build(OpBuilder &builder, OperationState &result,
                                  Value lhs, Value rhs, Value acc,
                                  ArrayAttr indexingMaps,
                                  ArrayAttr iteratorTypes, CombiningKind kind) {
  result.addOperands({lhs, rhs, acc});
  result.addTypes(acc.getType());
  result.addAttribute(getIndexingMapsAttrName(result.name), indexingMaps);
  result.addAttribute(getIteratorTypesAttrName(result.name), iteratorTypes);
  result.addAttribute(getKindAttrName(result.name),
                      CombiningKindAttr::get(builder.getContext(), kind));
}

ParseResult ContractionOp::parse(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::UnresolvedOperand lhsInfo;
  OpAsmParser::UnresolvedOperand rhsInfo;
  OpAsmParser::UnresolvedOperand accInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 2> masksInfo;
  SmallVector<Type, 2> types;
  Type resultType;
  auto loc = parser.getCurrentLocation();
  DictionaryAttr dictAttr;
  // TODO: Unify linalg op attribute parsing.
  if (parser.parseAttribute(dictAttr) || parser.parseOperand(lhsInfo) ||
      parser.parseComma() || parser.parseOperand(rhsInfo) ||
      parser.parseComma() || parser.parseOperand(accInfo) ||
      parser.parseTrailingOperandList(masksInfo) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonTypeList(types) ||
      parser.parseKeywordType("into", resultType) ||
      parser.resolveOperand(lhsInfo, types[0], result.operands) ||
      parser.resolveOperand(rhsInfo, types[1], result.operands) ||
      parser.resolveOperand(accInfo, resultType, result.operands) ||
      parser.addTypeToList(resultType, result.types))
    return failure();
  result.attributes.append(dictAttr.getValue().begin(),
                           dictAttr.getValue().end());

  // Convert array of string into an array of IteratyType enums. This is needed,
  // because tests still use the old format when 'iterator_types' attribute is
  // represented as an array of strings.
  // TODO: Remove this conversion once tests are fixed.
  ArrayAttr iteratorTypes = llvm::cast<ArrayAttr>(
      result.attributes.get(getIteratorTypesAttrName(result.name)));

  SmallVector<Attribute> iteratorTypeAttrs;

  for (StringRef s : iteratorTypes.getAsValueRange<StringAttr>()) {
    auto maybeIteratorType = symbolizeIteratorType(s);
    if (!maybeIteratorType.has_value())
      return parser.emitError(loc) << "unexpected iterator_type (" << s << ")";

    iteratorTypeAttrs.push_back(
        IteratorTypeAttr::get(parser.getContext(), maybeIteratorType.value()));
  }
  result.attributes.set(getIteratorTypesAttrName(result.name),
                        parser.getBuilder().getArrayAttr(iteratorTypeAttrs));

  if (!result.attributes.get(getKindAttrName(result.name))) {
    result.addAttribute(
        getKindAttrName(result.name),
        CombiningKindAttr::get(result.getContext(),
                               ContractionOp::getDefaultKind()));
  }
  if (masksInfo.empty())
    return success();
  if (masksInfo.size() != 2)
    return parser.emitError(parser.getNameLoc(),
                            "expected zero or exactly 2 vector mask operands");
  auto lhsType = llvm::cast<VectorType>(types[0]);
  auto rhsType = llvm::cast<VectorType>(types[1]);
  auto maskElementType = parser.getBuilder().getI1Type();
  std::array<Type, 2> maskTypes = {
      VectorType::Builder(lhsType).setElementType(maskElementType),
      VectorType::Builder(rhsType).setElementType(maskElementType)};
  if (parser.resolveOperands(masksInfo, maskTypes, loc, result.operands))
    return failure();
  return success();
}

void ContractionOp::print(OpAsmPrinter &p) {
  // TODO: Unify printing code with linalg ops.
  auto attrNames = getTraitAttrNames();
  llvm::StringSet<> traitAttrsSet;
  traitAttrsSet.insert(attrNames.begin(), attrNames.end());
  SmallVector<NamedAttribute, 8> attrs;
  for (auto attr : (*this)->getAttrs()) {
    if (attr.getName() == getIteratorTypesAttrName()) {
      auto iteratorTypes =
          llvm::cast<ArrayAttr>(attr.getValue())
              .getAsValueRange<IteratorTypeAttr, IteratorType>();
      // Convert IteratorType enums into the string representation. This is
      // needed, because tests still use the old format when 'iterator_types'
      // attribute is represented as an array of strings.
      // TODO: Remove this conversion once tests are fixed.
      SmallVector<Attribute> iteratorTypeNames = llvm::to_vector(
          llvm::map_range(iteratorTypes, [&](IteratorType t) -> Attribute {
            return StringAttr::get(getContext(), stringifyIteratorType(t));
          }));

      attrs.emplace_back(getIteratorTypesAttrName(),
                         ArrayAttr::get(getContext(), iteratorTypeNames));
    } else if (traitAttrsSet.count(attr.getName().strref()) > 0)
      attrs.push_back(attr);
  }

  auto dictAttr = DictionaryAttr::get(getContext(), attrs);
  p << " " << dictAttr << " " << getLhs() << ", ";
  p << getRhs() << ", " << getAcc();

  p.printOptionalAttrDict((*this)->getAttrs(), attrNames);
  p << " : " << getLhs().getType() << ", " << getRhs().getType() << " into "
    << getResultType();
}

static bool verifyDimMap(VectorType lhsType, VectorType rhsType,
                         const std::vector<std::pair<int64_t, int64_t>> &map) {
  for (auto &dimPair : map) {
    if (dimPair.first < 0 || dimPair.first >= lhsType.getRank() ||
        dimPair.second < 0 || dimPair.second >= rhsType.getRank() ||
        lhsType.getDimSize(dimPair.first) != rhsType.getDimSize(dimPair.second))
      return false;
  }
  return true;
}

static LogicalResult verifyOutputShape(
    ContractionOp op, VectorType lhsType, VectorType rhsType, Type accType,
    Type resType,
    const std::vector<std::pair<int64_t, int64_t>> &contractingDimMap,
    const std::vector<std::pair<int64_t, int64_t>> &batchDimMap) {
  DenseSet<int64_t> lhsContractingDimSet;
  DenseSet<int64_t> rhsContractingDimSet;
  for (auto &dimPair : contractingDimMap) {
    lhsContractingDimSet.insert(dimPair.first);
    rhsContractingDimSet.insert(dimPair.second);
  }
  DenseSet<int64_t> rhsBatchDimSet;
  for (auto &dimPair : batchDimMap)
    rhsBatchDimSet.insert(dimPair.second);

  // Add free and batch dimensions from 'lhsType' to 'expectedResultDims'.
  SmallVector<int64_t, 4> expectedResultDims;
  for (int64_t i = 0, e = lhsType.getRank(); i < e; ++i) {
    if (lhsContractingDimSet.count(i) > 0)
      continue;
    expectedResultDims.push_back(lhsType.getDimSize(i));
  }

  // Add free dimensions from 'rhsType' to 'expectedResultDims'.
  for (int64_t i = 0, e = rhsType.getRank(); i < e; ++i) {
    if (rhsContractingDimSet.count(i) > 0 || rhsBatchDimSet.count(i) > 0)
      continue;
    expectedResultDims.push_back(rhsType.getDimSize(i));
  }

  // Verify 'expectedResultDims'.
  if (expectedResultDims.empty()) {
    // No batch or free dimension implies a scalar result.
    if (llvm::isa<VectorType>(resType) || llvm::isa<VectorType>(accType))
      return op.emitOpError("invalid accumulator/result vector shape");
  } else {
    // At least one batch or free dimension implies a vector result.
    auto resVectorType = llvm::dyn_cast<VectorType>(resType);
    auto accVectorType = llvm::dyn_cast<VectorType>(accType);
    if (!resVectorType || !accVectorType)
      return op.emitOpError("invalid accumulator/result vector shape");

    // Infer expected result vector type. Lhs + rhs map and lhs + rhs vector
    // types fully define the result vector type. This assumes the affine maps
    // are well-formed, which must have been verified already.
    MLIRContext *ctx = op.getContext();
    AffineMap lhsMap = op.getIndexingMapsArray()[0];
    AffineMap rhsMap = op.getIndexingMapsArray()[1];
    if (getUnusedDimsBitVector({lhsMap, rhsMap}).any())
      return op.emitOpError(
          "expected all dimensions to be either a LHS or a RHS dimension");
    SmallVector<AffineExpr, 4> extents(lhsMap.getNumInputs());
    for (auto pair :
         {std::make_pair(lhsType, lhsMap), std::make_pair(rhsType, rhsMap)}) {
      VectorType v = pair.first;
      auto map = pair.second;
      for (unsigned idx = 0, e = v.getRank(); idx < e; ++idx) {
        unsigned pos = map.getDimPosition(idx);
        if (!extents[pos])
          extents[pos] = getAffineConstantExpr(v.getShape()[idx], ctx);
      }
    }
    if (!llvm::all_of(extents, [](AffineExpr e) { return e; }))
      return op.emitOpError("expected all dimensions to get an extent as "
                            "either a LHS or a RHS dimension");

    AffineMap resMap = op.getIndexingMapsArray()[2];
    auto extentsMap = AffineMap::get(/*dimCount=*/extents.size(),
                                     /*symCount=*/0, extents, ctx);
    // Compose the resMap with the extentsMap, which is a constant map.
    AffineMap expectedMap = simplifyAffineMap(resMap.compose(extentsMap));
    assert(llvm::all_of(
               expectedMap.getResults(),
               [](AffineExpr e) { return e.isa<AffineConstantExpr>(); }) &&
           "expected constant extent along all dimensions.");
    // Extract the expected shape and build the type.
    auto expectedShape = llvm::to_vector<4>(
        llvm::map_range(expectedMap.getResults(), [](AffineExpr e) {
          return e.cast<AffineConstantExpr>().getValue();
        }));
    auto expected =
        VectorType::get(expectedShape, resVectorType.getElementType());
    if (resVectorType != expected || accVectorType != expected)
      return op.emitOpError(
                 "invalid accumulator/result vector shape, expected: ")
             << expected;
  }
  return success();
}

LogicalResult ContractionOp::verify() {
  VectorType lhsType = getLhsType();
  VectorType rhsType = getRhsType();
  Type accType = getAccType();
  Type resType = getResultType();

  if (llvm::isa<IntegerType>(lhsType.getElementType())) {
    if (!lhsType.getElementType().isSignlessInteger())
      return emitOpError("only supports signless integer types");
  }

  // Verify that an indexing map was specified for each vector operand.
  if (getIndexingMapsArray().size() != 3)
    return emitOpError("expected an indexing map for each vector operand");

  // Verify that each index map has 'numIterators' inputs, no symbols, and
  // that the number of map outputs equals the rank of its associated
  // vector operand.
  unsigned numIterators = getIteratorTypes().getValue().size();
  for (const auto &it : llvm::enumerate(getIndexingMapsArray())) {
    auto index = it.index();
    auto map = it.value();
    if (map.getNumSymbols() != 0)
      return emitOpError("expected indexing map ")
             << index << " to have no symbols";
    auto vectorType = llvm::dyn_cast<VectorType>(getOperand(index).getType());
    unsigned rank = vectorType ? vectorType.getShape().size() : 0;
    // Verify that the map has the right number of inputs, outputs, and indices.
    // This also correctly accounts for (..) -> () for rank-0 results.
    if (map.getNumDims() != numIterators)
      return emitOpError("expected indexing map ")
             << index << " to have " << numIterators << " number of inputs";
    if (map.getNumResults() != rank)
      return emitOpError("expected indexing map ")
             << index << " to have " << rank << " number of outputs";
    if (!map.isProjectedPermutation())
      return emitOpError("expected indexing map ")
             << index << " to be a projected permutation of its inputs";
  }

  auto contractingDimMap = getContractingDimMap();
  auto batchDimMap = getBatchDimMap();

  // Verify at least one contracting dimension pair was specified.
  if (contractingDimMap.empty())
    return emitOpError("expected at least one contracting dimension pair");

  // Verify contracting dimension map was properly constructed.
  if (!verifyDimMap(lhsType, rhsType, contractingDimMap))
    return emitOpError("invalid contracting dimension map");

  // Verify batch dimension map was properly constructed.
  if (!verifyDimMap(lhsType, rhsType, batchDimMap))
    return emitOpError("invalid batch dimension map");

  // Verify 'accType' and 'resType' shape.
  if (failed(verifyOutputShape(*this, lhsType, rhsType, accType, resType,
                               contractingDimMap, batchDimMap)))
    return failure();

  // Verify supported combining kind.
  auto vectorType = llvm::dyn_cast<VectorType>(resType);
  auto elementType = vectorType ? vectorType.getElementType() : resType;
  if (!isSupportedCombiningKind(getKind(), elementType))
    return emitOpError("unsupported contraction type");

  return success();
}

// MaskableOpInterface methods.

/// Returns the mask type expected by this operation. Mostly used for
/// verification purposes. It requires the operation to be vectorized."
Type ContractionOp::getExpectedMaskType() {
  auto indexingMaps = this->getIndexingMapsArray();
  AffineMap lhsIdxMap = indexingMaps[0];
  AffineMap rhsIdxMap = indexingMaps[1];
  VectorType lhsType = this->getLhsType();
  VectorType rhsType = this->getRhsType();

  unsigned numVecDims = lhsIdxMap.getNumDims();
  SmallVector<int64_t> maskShape(numVecDims, ShapedType::kDynamic);

  // Using the information in the indexing maps, extract the size of each
  // dimension in the vector.contract operation from the two input operands.
  for (auto [dimIdx, dimSize] : llvm::enumerate(lhsType.getShape()))
    maskShape[lhsIdxMap.getDimPosition(dimIdx)] = dimSize;
  for (auto [dimIdx, dimSize] : llvm::enumerate(rhsType.getShape()))
    maskShape[rhsIdxMap.getDimPosition(dimIdx)] = dimSize;

  assert(!ShapedType::isDynamicShape(maskShape) &&
         "Mask shape couldn't be computed");
  // TODO: Extend the scalable vector type representation with a bit map.
  assert(!lhsType.isScalable() && !rhsType.isScalable() &&
         "Scalable vectors are not supported yet");

  return VectorType::get(maskShape,
                         IntegerType::get(lhsType.getContext(), /*width=*/1));
}

SmallVector<StringRef> ContractionOp::getTraitAttrNames() {
  return SmallVector<StringRef>{getIndexingMapsAttrName(),
                                getIteratorTypesAttrName(), getKindAttrName()};
}

static int64_t getResultIndex(AffineMap map, AffineExpr targetExpr) {
  for (int64_t i = 0, e = map.getNumResults(); i < e; ++i)
    if (targetExpr == map.getResult(i))
      return i;
  return -1;
}

static std::vector<std::pair<int64_t, int64_t>>
getDimMap(ArrayRef<AffineMap> indexingMaps, ArrayAttr iteratorTypes,
          IteratorType targetIteratorType, MLIRContext *context) {
  std::vector<std::pair<int64_t, int64_t>> dimMap;
  for (const auto &it : llvm::enumerate(iteratorTypes)) {
    auto iteratorType = llvm::cast<IteratorTypeAttr>(it.value()).getValue();
    if (iteratorType != targetIteratorType)
      continue;
    // Search lhs/rhs map results for 'targetExpr'.
    auto targetExpr = getAffineDimExpr(it.index(), context);
    int64_t lhsDim = getResultIndex(indexingMaps[0], targetExpr);
    int64_t rhsDim = getResultIndex(indexingMaps[1], targetExpr);
    if (lhsDim >= 0 && rhsDim >= 0)
      dimMap.emplace_back(lhsDim, rhsDim);
  }
  return dimMap;
}

void ContractionOp::getIterationBounds(
    SmallVectorImpl<int64_t> &iterationBounds) {
  auto lhsShape = getLhsType().getShape();
  auto resVectorType = llvm::dyn_cast<VectorType>(getResultType());
  SmallVector<AffineMap, 4> indexingMaps(getIndexingMapsArray());
  SmallVector<int64_t, 2> iterationShape;
  for (const auto &it : llvm::enumerate(getIteratorTypes())) {
    // Search lhs/rhs map results for 'targetExpr'.
    auto targetExpr = getAffineDimExpr(it.index(), getContext());
    auto iteratorType = llvm::cast<IteratorTypeAttr>(it.value()).getValue();
    if (iteratorType == IteratorType::reduction) {
      // Get reduction dim size from lhs shape (same size in rhsShape).
      int64_t lhsDimIndex = getResultIndex(indexingMaps[0], targetExpr);
      assert(lhsDimIndex >= 0);
      iterationBounds.push_back(lhsShape[lhsDimIndex]);
      continue;
    }
    // Get parallel dimension size from result shape.
    int64_t resDimIndex = getResultIndex(indexingMaps[2], targetExpr);
    assert(resDimIndex >= 0);
    assert(resVectorType != nullptr);
    iterationBounds.push_back(resVectorType.getShape()[resDimIndex]);
  }
}

void ContractionOp::getIterationIndexMap(
    std::vector<DenseMap<int64_t, int64_t>> &iterationIndexMap) {
  unsigned numMaps = getIndexingMapsArray().size();
  iterationIndexMap.resize(numMaps);
  for (const auto &it : llvm::enumerate(getIndexingMapsArray())) {
    auto index = it.index();
    auto map = it.value();
    for (unsigned i = 0, e = map.getNumResults(); i < e; ++i) {
      auto dim = map.getResult(i).cast<AffineDimExpr>();
      iterationIndexMap[index][dim.getPosition()] = i;
    }
  }
}

std::vector<std::pair<int64_t, int64_t>> ContractionOp::getContractingDimMap() {
  SmallVector<AffineMap, 4> indexingMaps(getIndexingMapsArray());
  return getDimMap(indexingMaps, getIteratorTypes(), IteratorType::reduction,
                   getContext());
}

std::vector<std::pair<int64_t, int64_t>> ContractionOp::getBatchDimMap() {
  SmallVector<AffineMap, 4> indexingMaps(getIndexingMapsArray());
  return getDimMap(indexingMaps, getIteratorTypes(), IteratorType::parallel,
                   getContext());
}

std::optional<SmallVector<int64_t, 4>> ContractionOp::getShapeForUnroll() {
  SmallVector<int64_t, 4> shape;
  getIterationBounds(shape);
  return shape;
}

/// Return a fused vector::ContractionOp which represents a patterns such as:
///
/// ```mlir
///    %c0 = vector.constant 0: ...
///    %c = vector.contract %a, %b, %c0: ...
///    %e = add %c, %d: ...
/// ```
///
/// by:
///
/// ```mlir
///    %e = vector.contract %a, %b, %d: ...
/// ```
///
/// Return null if the canonicalization does not apply.
// TODO: This should be a folding of Add into Contract in core but while they
// live in different dialects, it is not possible without unnatural
// dependencies.
template <typename AddOpType>
struct CanonicalizeContractAdd : public OpRewritePattern<AddOpType> {
  using OpRewritePattern<AddOpType>::OpRewritePattern;

  LogicalResult matchAndRewrite(AddOpType addOp,
                                PatternRewriter &rewriter) const override {
    auto canonicalize = [&](Value maybeContraction,
                            Value otherOperand) -> vector::ContractionOp {
      vector::ContractionOp contractionOp =
          dyn_cast_or_null<vector::ContractionOp>(
              maybeContraction.getDefiningOp());
      if (!contractionOp)
        return vector::ContractionOp();
      if (auto maybeZero = dyn_cast_or_null<arith::ConstantOp>(
              contractionOp.getAcc().getDefiningOp())) {
        if (maybeZero.getValue() ==
            rewriter.getZeroAttr(contractionOp.getAcc().getType())) {
          IRMapping bvm;
          bvm.map(contractionOp.getAcc(), otherOperand);
          auto newContraction =
              cast<vector::ContractionOp>(rewriter.clone(*contractionOp, bvm));
          rewriter.replaceOp(addOp, newContraction.getResult());
          return newContraction;
        }
      }
      return vector::ContractionOp();
    };

    Value a = addOp->getOperand(0), b = addOp->getOperand(1);
    vector::ContractionOp contract = canonicalize(a, b);
    contract = contract ? contract : canonicalize(b, a);
    return contract ? success() : failure();
  }
};

void ContractionOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<CanonicalizeContractAdd<arith::AddIOp>,
              CanonicalizeContractAdd<arith::AddFOp>>(context);
}

//===----------------------------------------------------------------------===//
// ExtractElementOp
//===----------------------------------------------------------------------===//

void vector::ExtractElementOp::build(OpBuilder &builder, OperationState &result,
                                     Value source) {
  result.addOperands({source});
  result.addTypes(llvm::cast<VectorType>(source.getType()).getElementType());
}

LogicalResult vector::ExtractElementOp::verify() {
  VectorType vectorType = getSourceVectorType();
  if (vectorType.getRank() == 0) {
    if (getPosition())
      return emitOpError("expected position to be empty with 0-D vector");
    return success();
  }
  if (vectorType.getRank() != 1)
    return emitOpError("unexpected >1 vector rank");
  if (!getPosition())
    return emitOpError("expected position for 1-D vector");
  return success();
}

OpFoldResult vector::ExtractElementOp::fold(FoldAdaptor adaptor) {
  // Skip the 0-D vector here now.
  if (!adaptor.getPosition())
    return {};

  Attribute src = adaptor.getVector();
  Attribute pos = adaptor.getPosition();

  // Fold extractelement (splat X) -> X.
  if (auto splat = getVector().getDefiningOp<vector::SplatOp>())
    return splat.getInput();

  // Fold extractelement(broadcast(X)) -> X.
  if (auto broadcast = getVector().getDefiningOp<vector::BroadcastOp>())
    if (!llvm::isa<VectorType>(broadcast.getSource().getType()))
      return broadcast.getSource();

  if (!pos || !src)
    return {};

  auto srcElements = llvm::cast<DenseElementsAttr>(src).getValues<Attribute>();

  auto attr = llvm::dyn_cast<IntegerAttr>(pos);
  uint64_t posIdx = attr.getInt();

  return srcElements[posIdx];
}

//===----------------------------------------------------------------------===//
// ExtractOp
//===----------------------------------------------------------------------===//

// Convenience builder which assumes the values are constant indices.
void vector::ExtractOp::build(OpBuilder &builder, OperationState &result,
                              Value source, ValueRange position) {
  SmallVector<int64_t> positionConstants = llvm::to_vector(llvm::map_range(
      position, [](Value pos) { return getConstantIntValue(pos).value(); }));
  build(builder, result, source, positionConstants);
}

LogicalResult
ExtractOp::inferReturnTypes(MLIRContext *, std::optional<Location>,
                            ExtractOp::Adaptor adaptor,
                            SmallVectorImpl<Type> &inferredReturnTypes) {
  auto vectorType = llvm::cast<VectorType>(adaptor.getVector().getType());
  if (static_cast<int64_t>(adaptor.getPosition().size()) ==
      vectorType.getRank()) {
    inferredReturnTypes.push_back(vectorType.getElementType());
  } else {
    auto n =
        std::min<size_t>(adaptor.getPosition().size(), vectorType.getRank());
    inferredReturnTypes.push_back(VectorType::get(
        vectorType.getShape().drop_front(n), vectorType.getElementType()));
  }
  return success();
}

bool ExtractOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  // Allow extracting 1-element vectors instead of scalars.
  auto isCompatible = [](TypeRange l, TypeRange r) {
    auto vectorType = llvm::dyn_cast<VectorType>(l.front());
    return vectorType && vectorType.getShape().equals({1}) &&
           vectorType.getElementType() == r.front();
  };
  if (l.size() == 1 && r.size() == 1 &&
      (isCompatible(l, r) || isCompatible(r, l)))
    return true;
  return l == r;
}

LogicalResult vector::ExtractOp::verify() {
  ArrayRef<int64_t> position = getPosition();
  if (position.size() > static_cast<unsigned>(getSourceVectorType().getRank()))
    return emitOpError(
        "expected position attribute of rank no greater than vector rank");
  for (const auto &en : llvm::enumerate(position)) {
    if (en.value() < 0 ||
        en.value() >= getSourceVectorType().getDimSize(en.index()))
      return emitOpError("expected position attribute #")
             << (en.index() + 1)
             << " to be a non-negative integer smaller than the corresponding "
                "vector dimension";
  }
  return success();
}

template <typename IntType>
static SmallVector<IntType> extractVector(ArrayAttr arrayAttr) {
  return llvm::to_vector<4>(llvm::map_range(
      arrayAttr.getAsRange<IntegerAttr>(),
      [](IntegerAttr attr) { return static_cast<IntType>(attr.getInt()); }));
}

/// Fold the result of chains of ExtractOp in place by simply concatenating the
/// positions.
static LogicalResult foldExtractOpFromExtractChain(ExtractOp extractOp) {
  if (!extractOp.getVector().getDefiningOp<ExtractOp>())
    return failure();

  SmallVector<int64_t, 4> globalPosition;
  ExtractOp currentOp = extractOp;
  ArrayRef<int64_t> extrPos = currentOp.getPosition();
  globalPosition.append(extrPos.rbegin(), extrPos.rend());
  while (ExtractOp nextOp = currentOp.getVector().getDefiningOp<ExtractOp>()) {
    currentOp = nextOp;
    ArrayRef<int64_t> extrPos = currentOp.getPosition();
    globalPosition.append(extrPos.rbegin(), extrPos.rend());
  }
  extractOp.setOperand(currentOp.getVector());
  // OpBuilder is only used as a helper to build an I64ArrayAttr.
  OpBuilder b(extractOp.getContext());
  std::reverse(globalPosition.begin(), globalPosition.end());
  extractOp.setPosition(globalPosition);
  return success();
}

namespace {
/// Fold an ExtractOp that is fed by a chain of InsertOps and TransposeOps.
/// Walk back a chain of InsertOp/TransposeOp until we hit a match.
/// Compose TransposeOp permutations as we walk back.
/// This helper class keeps an updated extraction position `extractPosition`
/// with extra trailing sentinels.
/// The sentinels encode the internal transposition status of the result vector.
/// As we iterate, extractPosition is permuted and updated.
class ExtractFromInsertTransposeChainState {
public:
  ExtractFromInsertTransposeChainState(ExtractOp e);

  /// Iterate over producing insert and transpose ops until we find a fold.
  Value fold();

private:
  /// Return true if the vector at position `a` is contained within the vector
  /// at position `b`. Under insert/extract semantics, this is the same as `a`
  /// is a prefix of `b`.
  template <typename ContainerA, typename ContainerB>
  bool isContainedWithin(const ContainerA &a, const ContainerB &b) {
    return a.size() <= b.size() &&
           std::equal(a.begin(), a.begin() + a.size(), b.begin());
  }

  /// Return true if the vector at position `a` intersects the vector at
  /// position `b`. Under insert/extract semantics, this is the same as equality
  /// of all entries of `a` that are >=0 with the corresponding entries of b.
  /// Comparison is on the common prefix (i.e. zip).
  template <typename ContainerA, typename ContainerB>
  bool intersectsWhereNonNegative(const ContainerA &a, const ContainerB &b) {
    for (auto [elemA, elemB] : llvm::zip(a, b)) {
      if (elemA < 0 || elemB < 0)
        continue;
      if (elemA != elemB)
        return false;
    }
    return true;
  }

  /// Folding is only possible in the absence of an internal permutation in the
  /// result vector.
  bool canFold() {
    return (sentinels == ArrayRef(extractPosition).drop_front(extractedRank));
  }

  // Helper to get the next defining op of interest.
  void updateStateForNextIteration(Value v) {
    nextInsertOp = v.getDefiningOp<vector::InsertOp>();
    nextTransposeOp = v.getDefiningOp<vector::TransposeOp>();
  };

  // Case 1. If we hit a transpose, just compose the map and iterate.
  // Invariant: insert + transpose do not change rank, we can always compose.
  LogicalResult handleTransposeOp();

  // Case 2: the insert position matches extractPosition exactly, early return.
  LogicalResult handleInsertOpWithMatchingPos(Value &res);

  /// Case 3: if the insert position is a prefix of extractPosition, extract a
  /// portion of the source of the insert.
  /// Example:
  /// ```
  /// %ins = vector.insert %source, %vest[1]: vector<3x4> into vector<2x3x4x5>
  /// // extractPosition == [1, 2, 3]
  /// %ext = vector.extract %ins[1, 0]: vector<3x4x5>
  /// // can fold to vector.extract %source[0, 3]
  /// %ext = vector.extract %source[3]: vector<5x6>
  /// ```
  /// To traverse through %source, we need to set the leading dims to 0 and
  /// drop the extra leading dims.
  /// This method updates the internal state.
  LogicalResult handleInsertOpWithPrefixPos(Value &res);

  /// Try to fold in place to extract(source, extractPosition) and return the
  /// folded result. Return null if folding is not possible (e.g. due to an
  /// internal tranposition in the result).
  Value tryToFoldExtractOpInPlace(Value source);

  ExtractOp extractOp;
  int64_t vectorRank;
  int64_t extractedRank;

  InsertOp nextInsertOp;
  TransposeOp nextTransposeOp;

  /// Sentinel values that encode the internal permutation status of the result.
  /// They are set to (-1, ... , -k) at the beginning and appended to
  /// `extractPosition`.
  /// In the end, the tail of `extractPosition` must be exactly `sentinels` to
  /// ensure that there is no internal transposition.
  /// Internal transposition cannot be accounted for with a folding pattern.
  // TODO: We could relax the internal transposition with an extra transposition
  // operation in a future canonicalizer.
  SmallVector<int64_t> sentinels;
  SmallVector<int64_t> extractPosition;
};
} // namespace

ExtractFromInsertTransposeChainState::ExtractFromInsertTransposeChainState(
    ExtractOp e)
    : extractOp(e), vectorRank(extractOp.getSourceVectorType().getRank()),
      extractedRank(extractOp.getPosition().size()) {
  assert(vectorRank >= extractedRank && "extracted pos overflow");
  sentinels.reserve(vectorRank - extractedRank);
  for (int64_t i = 0, e = vectorRank - extractedRank; i < e; ++i)
    sentinels.push_back(-(i + 1));
  extractPosition.assign(extractOp.getPosition().begin(),
                         extractOp.getPosition().end());
  llvm::append_range(extractPosition, sentinels);
}

// Case 1. If we hit a transpose, just compose the map and iterate.
// Invariant: insert + transpose do not change rank, we can always compose.
LogicalResult ExtractFromInsertTransposeChainState::handleTransposeOp() {
  if (!nextTransposeOp)
    return failure();
  auto permutation = extractVector<unsigned>(nextTransposeOp.getTransp());
  AffineMap m = inversePermutation(
      AffineMap::getPermutationMap(permutation, extractOp.getContext()));
  extractPosition = applyPermutationMap(m, ArrayRef(extractPosition));
  return success();
}

// Case 2: the insert position matches extractPosition exactly, early return.
LogicalResult
ExtractFromInsertTransposeChainState::handleInsertOpWithMatchingPos(
    Value &res) {
  ArrayRef<int64_t> insertedPos = nextInsertOp.getPosition();
  if (insertedPos != llvm::ArrayRef(extractPosition).take_front(extractedRank))
    return failure();
  // Case 2.a. early-exit fold.
  res = nextInsertOp.getSource();
  // Case 2.b. if internal transposition is present, canFold will be false.
  return success(canFold());
}

/// Case 3: if inserted position is a prefix of extractPosition,
/// extract a portion of the source of the insertion.
/// This method updates the internal state.
LogicalResult
ExtractFromInsertTransposeChainState::handleInsertOpWithPrefixPos(Value &res) {
  ArrayRef<int64_t> insertedPos = nextInsertOp.getPosition();
  if (!isContainedWithin(insertedPos, extractPosition))
    return failure();
  // Set leading dims to zero.
  std::fill_n(extractPosition.begin(), insertedPos.size(), 0);
  // Drop extra leading dims.
  extractPosition.erase(extractPosition.begin(),
                        extractPosition.begin() + insertedPos.size());
  extractedRank = extractPosition.size() - sentinels.size();
  // Case 3.a. early-exit fold (break and delegate to post-while path).
  res = nextInsertOp.getSource();
  // Case 3.b. if internal transposition is present, canFold will be false.
  return success();
}

/// Try to fold in place to extract(source, extractPosition) and return the
/// folded result. Return null if folding is not possible (e.g. due to an
/// internal tranposition in the result).
Value ExtractFromInsertTransposeChainState::tryToFoldExtractOpInPlace(
    Value source) {
  // If we can't fold (either internal transposition, or nothing to fold), bail.
  bool nothingToFold = (source == extractOp.getVector());
  if (nothingToFold || !canFold())
    return Value();
  // Otherwise, fold by updating the op inplace and return its result.
  OpBuilder b(extractOp.getContext());
  extractOp.setPosition(ArrayRef(extractPosition).take_front(extractedRank));
  extractOp.getVectorMutable().assign(source);
  return extractOp.getResult();
}

/// Iterate over producing insert and transpose ops until we find a fold.
Value ExtractFromInsertTransposeChainState::fold() {
  Value valueToExtractFrom = extractOp.getVector();
  updateStateForNextIteration(valueToExtractFrom);
  while (nextInsertOp || nextTransposeOp) {
    // Case 1. If we hit a transpose, just compose the map and iterate.
    // Invariant: insert + transpose do not change rank, we can always compose.
    if (succeeded(handleTransposeOp())) {
      valueToExtractFrom = nextTransposeOp.getVector();
      updateStateForNextIteration(valueToExtractFrom);
      continue;
    }

    Value result;
    // Case 2: the position match exactly.
    if (succeeded(handleInsertOpWithMatchingPos(result)))
      return result;

    // Case 3: if the inserted position is a prefix of extractPosition, we can
    // just extract a portion of the source of the insert.
    if (succeeded(handleInsertOpWithPrefixPos(result)))
      return tryToFoldExtractOpInPlace(result);

    // Case 4: extractPositionRef intersects insertedPosRef on non-sentinel
    // values. This is a more difficult case and we bail.
    ArrayRef<int64_t> insertedPos = nextInsertOp.getPosition();
    if (isContainedWithin(extractPosition, insertedPos) ||
        intersectsWhereNonNegative(extractPosition, insertedPos))
      return Value();

    // Case 5: No intersection, we forward the extract to insertOp.dest().
    valueToExtractFrom = nextInsertOp.getDest();
    updateStateForNextIteration(valueToExtractFrom);
  }
  // If after all this we can fold, go for it.
  return tryToFoldExtractOpInPlace(valueToExtractFrom);
}

/// Returns true if the operation has a 0-D vector type operand or result.
static bool hasZeroDimVectors(Operation *op) {
  auto hasZeroDimVectorType = [](Type type) -> bool {
    auto vecType = dyn_cast<VectorType>(type);
    return vecType && vecType.getRank() == 0;
  };

  return llvm::any_of(op->getOperandTypes(), hasZeroDimVectorType) ||
         llvm::any_of(op->getResultTypes(), hasZeroDimVectorType);
}

/// Fold extractOp with scalar result coming from BroadcastOp or SplatOp.
static Value foldExtractFromBroadcast(ExtractOp extractOp) {
  Operation *defOp = extractOp.getVector().getDefiningOp();
  if (!defOp || !isa<vector::BroadcastOp, SplatOp>(defOp))
    return Value();

  // 0-D vectors not supported.
  assert(!hasZeroDimVectors(extractOp) && "0-D vectors not supported");
  if (hasZeroDimVectors(defOp))
    return Value();

  Value source = defOp->getOperand(0);
  if (extractOp.getType() == source.getType())
    return source;
  auto getRank = [](Type type) {
    return llvm::isa<VectorType>(type) ? llvm::cast<VectorType>(type).getRank()
                                       : 0;
  };
  // If splat or broadcast from a scalar, just return the source scalar.
  unsigned broadcastSrcRank = getRank(source.getType());
  if (broadcastSrcRank == 0)
    return source;

  unsigned extractResultRank = getRank(extractOp.getType());
  if (extractResultRank >= broadcastSrcRank)
    return Value();
  // Check that the dimension of the result haven't been broadcasted.
  auto extractVecType = llvm::dyn_cast<VectorType>(extractOp.getType());
  auto broadcastVecType = llvm::dyn_cast<VectorType>(source.getType());
  if (extractVecType && broadcastVecType &&
      extractVecType.getShape() !=
          broadcastVecType.getShape().take_back(extractResultRank))
    return Value();

  auto broadcastOp = cast<vector::BroadcastOp>(defOp);
  int64_t broadcastDstRank = broadcastOp.getResultVectorType().getRank();

  // Detect all the positions that come from "dim-1" broadcasting.
  // These dimensions correspond to "dim-1" broadcasted dims; set the mathching
  // extract position to `0` when extracting from the source operand.
  llvm::SetVector<int64_t> broadcastedUnitDims =
      broadcastOp.computeBroadcastedUnitDims();
  SmallVector<int64_t> extractPos(extractOp.getPosition());
  int64_t broadcastRankDiff = broadcastDstRank - broadcastSrcRank;
  for (int64_t i = broadcastRankDiff, e = extractPos.size(); i < e; ++i)
    if (broadcastedUnitDims.contains(i))
      extractPos[i] = 0;
  // `rankDiff` leading dimensions correspond to new broadcasted dims, drop the
  // matching extract position when extracting from the source operand.
  int64_t rankDiff = broadcastSrcRank - extractResultRank;
  extractPos.erase(extractPos.begin(),
                   std::next(extractPos.begin(), extractPos.size() - rankDiff));
  // OpBuilder is only used as a helper to build an I64ArrayAttr.
  OpBuilder b(extractOp.getContext());
  extractOp.setOperand(source);
  extractOp.setPosition(extractPos);
  return extractOp.getResult();
}

// Fold extractOp with source coming from ShapeCast op.
static Value foldExtractFromShapeCast(ExtractOp extractOp) {
  auto shapeCastOp = extractOp.getVector().getDefiningOp<vector::ShapeCastOp>();
  if (!shapeCastOp)
    return Value();

  // 0-D vectors not supported.
  assert(!hasZeroDimVectors(extractOp) && "0-D vectors not supported");
  if (hasZeroDimVectors(shapeCastOp))
    return Value();

  // Get the nth dimension size starting from lowest dimension.
  auto getDimReverse = [](VectorType type, int64_t n) {
    return type.getShape().take_back(n + 1).front();
  };
  int64_t destinationRank =
      llvm::isa<VectorType>(extractOp.getType())
          ? llvm::cast<VectorType>(extractOp.getType()).getRank()
          : 0;
  if (destinationRank > shapeCastOp.getSourceVectorType().getRank())
    return Value();
  if (destinationRank > 0) {
    auto destinationType =
        llvm::cast<VectorType>(extractOp.getResult().getType());
    for (int64_t i = 0; i < destinationRank; i++) {
      // The lowest dimension of of the destination must match the lowest
      // dimension of the shapecast op source.
      // TODO: This case could be support in a canonicalization pattern.
      if (getDimReverse(shapeCastOp.getSourceVectorType(), i) !=
          getDimReverse(destinationType, i))
        return Value();
    }
  }
  // Extract the strides associated with the extract op vector source. Then use
  // this to calculate a linearized position for the extract.
  SmallVector<int64_t> extractedPos(extractOp.getPosition());
  std::reverse(extractedPos.begin(), extractedPos.end());
  SmallVector<int64_t, 4> strides;
  int64_t stride = 1;
  for (int64_t i = 0, e = extractedPos.size(); i < e; i++) {
    strides.push_back(stride);
    stride *=
        getDimReverse(extractOp.getSourceVectorType(), i + destinationRank);
  }

  int64_t position = linearize(extractedPos, strides);
  // Then extract the strides associated to the shapeCast op vector source and
  // delinearize the position using those strides.
  SmallVector<int64_t, 4> newStrides;
  int64_t numDimension =
      shapeCastOp.getSourceVectorType().getRank() - destinationRank;
  stride = 1;
  for (int64_t i = 0; i < numDimension; i++) {
    newStrides.push_back(stride);
    stride *=
        getDimReverse(shapeCastOp.getSourceVectorType(), i + destinationRank);
  }
  std::reverse(newStrides.begin(), newStrides.end());
  SmallVector<int64_t, 4> newPosition = delinearize(position, newStrides);
  // OpBuilder is only used as a helper to build an I64ArrayAttr.
  OpBuilder b(extractOp.getContext());
  extractOp.setPosition(newPosition);
  extractOp.setOperand(shapeCastOp.getSource());
  return extractOp.getResult();
}

/// Fold an ExtractOp from ExtractStridedSliceOp.
static Value foldExtractFromExtractStrided(ExtractOp extractOp) {
  auto extractStridedSliceOp =
      extractOp.getVector().getDefiningOp<vector::ExtractStridedSliceOp>();
  if (!extractStridedSliceOp)
    return Value();

  // 0-D vectors not supported.
  assert(!hasZeroDimVectors(extractOp) && "0-D vectors not supported");
  if (hasZeroDimVectors(extractStridedSliceOp))
    return Value();

  // Return if 'extractStridedSliceOp' has non-unit strides.
  if (extractStridedSliceOp.hasNonUnitStrides())
    return Value();

  // Trim offsets for dimensions fully extracted.
  auto sliceOffsets =
      extractVector<int64_t>(extractStridedSliceOp.getOffsets());
  while (!sliceOffsets.empty()) {
    size_t lastOffset = sliceOffsets.size() - 1;
    if (sliceOffsets.back() != 0 ||
        extractStridedSliceOp.getType().getDimSize(lastOffset) !=
            extractStridedSliceOp.getSourceVectorType().getDimSize(lastOffset))
      break;
    sliceOffsets.pop_back();
  }
  unsigned destinationRank = 0;
  if (auto vecType = llvm::dyn_cast<VectorType>(extractOp.getType()))
    destinationRank = vecType.getRank();
  // The dimensions of the result need to be untouched by the
  // extractStridedSlice op.
  if (destinationRank > extractStridedSliceOp.getSourceVectorType().getRank() -
                            sliceOffsets.size())
    return Value();
  SmallVector<int64_t> extractedPos(extractOp.getPosition());
  assert(extractedPos.size() >= sliceOffsets.size());
  for (size_t i = 0, e = sliceOffsets.size(); i < e; i++)
    extractedPos[i] = extractedPos[i] + sliceOffsets[i];
  extractOp.getVectorMutable().assign(extractStridedSliceOp.getVector());
  // OpBuilder is only used as a helper to build an I64ArrayAttr.
  OpBuilder b(extractOp.getContext());
  extractOp.setPosition(extractedPos);
  return extractOp.getResult();
}

/// Fold extract_op fed from a chain of insertStridedSlice ops.
static Value foldExtractStridedOpFromInsertChain(ExtractOp extractOp) {
  int64_t destinationRank =
      llvm::isa<VectorType>(extractOp.getType())
          ? llvm::cast<VectorType>(extractOp.getType()).getRank()
          : 0;
  auto insertOp = extractOp.getVector().getDefiningOp<InsertStridedSliceOp>();
  if (!insertOp)
    return Value();

  // 0-D vectors not supported.
  assert(!hasZeroDimVectors(extractOp) && "0-D vectors not supported");
  if (hasZeroDimVectors(insertOp))
    return Value();

  while (insertOp) {
    int64_t insertRankDiff = insertOp.getDestVectorType().getRank() -
                             insertOp.getSourceVectorType().getRank();
    if (destinationRank > insertOp.getSourceVectorType().getRank())
      return Value();
    auto insertOffsets = extractVector<int64_t>(insertOp.getOffsets());
    ArrayRef<int64_t> extractOffsets = extractOp.getPosition();

    if (llvm::any_of(insertOp.getStrides(), [](Attribute attr) {
          return llvm::cast<IntegerAttr>(attr).getInt() != 1;
        }))
      return Value();
    bool disjoint = false;
    SmallVector<int64_t, 4> offsetDiffs;
    for (unsigned dim = 0, e = extractOffsets.size(); dim < e; ++dim) {
      int64_t start = insertOffsets[dim];
      int64_t size =
          (dim < insertRankDiff)
              ? 1
              : insertOp.getSourceVectorType().getDimSize(dim - insertRankDiff);
      int64_t end = start + size;
      int64_t offset = extractOffsets[dim];
      // Check if the start of the extract offset is in the interval inserted.
      if (start <= offset && offset < end) {
        if (dim >= insertRankDiff)
          offsetDiffs.push_back(offset - start);
        continue;
      }
      disjoint = true;
      break;
    }
    // The extract element chunk overlap with the vector inserted.
    if (!disjoint) {
      // If any of the inner dimensions are only partially inserted we have a
      // partial overlap.
      int64_t srcRankDiff =
          insertOp.getSourceVectorType().getRank() - destinationRank;
      for (int64_t i = 0; i < destinationRank; i++) {
        if (insertOp.getSourceVectorType().getDimSize(i + srcRankDiff) !=
            insertOp.getDestVectorType().getDimSize(i + srcRankDiff +
                                                    insertRankDiff))
          return Value();
      }
      extractOp.getVectorMutable().assign(insertOp.getSource());
      // OpBuilder is only used as a helper to build an I64ArrayAttr.
      OpBuilder b(extractOp.getContext());
      extractOp.setPosition(offsetDiffs);
      return extractOp.getResult();
    }
    // If the chunk extracted is disjoint from the chunk inserted, keep
    // looking in the insert chain.
    insertOp = insertOp.getDest().getDefiningOp<InsertStridedSliceOp>();
  }
  return Value();
}

OpFoldResult ExtractOp::fold(FoldAdaptor) {
  if (getPosition().empty())
    return getVector();
  if (succeeded(foldExtractOpFromExtractChain(*this)))
    return getResult();
  if (auto res = ExtractFromInsertTransposeChainState(*this).fold())
    return res;
  if (auto res = foldExtractFromBroadcast(*this))
    return res;
  if (auto res = foldExtractFromShapeCast(*this))
    return res;
  if (auto val = foldExtractFromExtractStrided(*this))
    return val;
  if (auto val = foldExtractStridedOpFromInsertChain(*this))
    return val;
  return OpFoldResult();
}

namespace {

// Pattern to rewrite a ExtractOp(Broadcast) -> Broadcast.
class ExtractOpFromBroadcast final : public OpRewritePattern<ExtractOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractOp extractOp,
                                PatternRewriter &rewriter) const override {
    Operation *defOp = extractOp.getVector().getDefiningOp();
    if (!defOp || !isa<vector::BroadcastOp, SplatOp>(defOp))
      return failure();

    Value source = defOp->getOperand(0);
    if (extractOp.getType() == source.getType())
      return failure();
    auto getRank = [](Type type) {
      return llvm::isa<VectorType>(type)
                 ? llvm::cast<VectorType>(type).getRank()
                 : 0;
    };
    unsigned broadcastSrcRank = getRank(source.getType());
    unsigned extractResultRank = getRank(extractOp.getType());
    // We only consider the case where the rank of the source is less than or
    // equal to the rank of the extract dst. The other cases are handled in the
    // folding patterns.
    if (extractResultRank < broadcastSrcRank)
      return failure();

    // Special case if broadcast src is a 0D vector.
    if (extractResultRank == 0) {
      assert(broadcastSrcRank == 0 && llvm::isa<VectorType>(source.getType()));
      rewriter.replaceOpWithNewOp<vector::ExtractElementOp>(extractOp, source);
      return success();
    }
    rewriter.replaceOpWithNewOp<vector::BroadcastOp>(
        extractOp, extractOp.getType(), source);
    return success();
  }
};

// Pattern to rewrite a ExtractOp(splat ConstantOp) -> ConstantOp.
class ExtractOpSplatConstantFolder final : public OpRewritePattern<ExtractOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractOp extractOp,
                                PatternRewriter &rewriter) const override {
    // Return if 'ExtractOp' operand is not defined by a splat vector
    // ConstantOp.
    Value sourceVector = extractOp.getVector();
    Attribute vectorCst;
    if (!matchPattern(sourceVector, m_Constant(&vectorCst)))
      return failure();
    auto splat = llvm::dyn_cast<SplatElementsAttr>(vectorCst);
    if (!splat)
      return failure();
    TypedAttr newAttr = splat.getSplatValue<TypedAttr>();
    if (auto vecDstType = llvm::dyn_cast<VectorType>(extractOp.getType()))
      newAttr = DenseElementsAttr::get(vecDstType, newAttr);
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(extractOp, newAttr);
    return success();
  }
};

// Pattern to rewrite a ExtractOp(non-splat ConstantOp)[...] -> ConstantOp.
class ExtractOpNonSplatConstantFolder final
    : public OpRewritePattern<ExtractOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractOp extractOp,
                                PatternRewriter &rewriter) const override {
    // Return if 'ExtractOp' operand is not defined by a compatible vector
    // ConstantOp.
    Value sourceVector = extractOp.getVector();
    Attribute vectorCst;
    if (!matchPattern(sourceVector, m_Constant(&vectorCst)))
      return failure();

    auto vecTy = llvm::cast<VectorType>(sourceVector.getType());
    if (vecTy.isScalable())
      return failure();

    // The splat case is handled by `ExtractOpSplatConstantFolder`.
    auto dense = llvm::dyn_cast<DenseElementsAttr>(vectorCst);
    if (!dense || dense.isSplat())
      return failure();

    // Calculate the linearized position of the continuous chunk of elements to
    // extract.
    llvm::SmallVector<int64_t> completePositions(vecTy.getRank(), 0);
    copy(extractOp.getPosition(), completePositions.begin());
    int64_t elemBeginPosition =
        linearize(completePositions, computeStrides(vecTy.getShape()));
    auto denseValuesBegin = dense.value_begin<TypedAttr>() + elemBeginPosition;

    TypedAttr newAttr;
    if (auto resVecTy = llvm::dyn_cast<VectorType>(extractOp.getType())) {
      SmallVector<Attribute> elementValues(
          denseValuesBegin, denseValuesBegin + resVecTy.getNumElements());
      newAttr = DenseElementsAttr::get(resVecTy, elementValues);
    } else {
      newAttr = *denseValuesBegin;
    }

    rewriter.replaceOpWithNewOp<arith::ConstantOp>(extractOp, newAttr);
    return success();
  }
};

} // namespace

void ExtractOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<ExtractOpSplatConstantFolder, ExtractOpNonSplatConstantFolder,
              ExtractOpFromBroadcast>(context);
}

static void populateFromInt64AttrArray(ArrayAttr arrayAttr,
                                       SmallVectorImpl<int64_t> &results) {
  for (auto attr : arrayAttr)
    results.push_back(llvm::cast<IntegerAttr>(attr).getInt());
}

//===----------------------------------------------------------------------===//
// FmaOp
//===----------------------------------------------------------------------===//

std::optional<SmallVector<int64_t, 4>> FMAOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getVectorType().getShape());
}

//===----------------------------------------------------------------------===//
// BroadcastOp
//===----------------------------------------------------------------------===//

/// Return the dimensions of the result vector that were formerly ones in the
/// source tensor and thus correspond to "dim-1" broadcasting.
static llvm::SetVector<int64_t>
computeBroadcastedUnitDims(ArrayRef<int64_t> srcShape,
                           ArrayRef<int64_t> dstShape) {
  int64_t rankDiff = dstShape.size() - srcShape.size();
  int64_t dstDim = rankDiff;
  llvm::SetVector<int64_t> res;
  for (auto [s1, s2] :
       llvm::zip_equal(srcShape, dstShape.drop_front(rankDiff))) {
    if (s1 != s2) {
      assert(s1 == 1 && "expected dim-1 broadcasting");
      res.insert(dstDim);
    }
    ++dstDim;
  }
  return res;
}

llvm::SetVector<int64_t> BroadcastOp::computeBroadcastedUnitDims() {
  // Scalar broadcast is without any unit dim broadcast.
  auto srcVectorType = llvm::dyn_cast<VectorType>(getSourceType());
  if (!srcVectorType)
    return {};
  return ::computeBroadcastedUnitDims(srcVectorType.getShape(),
                                      getResultVectorType().getShape());
}

/// Broadcast `value` to a vector of `dstShape`, knowing that exactly the
/// `broadcastedDims` dimensions in the dstShape are broadcasted.
/// This requires (and asserts) that the broadcast is free of dim-1
/// broadcasting.
/// Since vector.broadcast only allows expanding leading dimensions, an extra
/// vector.transpose may be inserted to make the broadcast possible.
/// `value`, `dstShape` and `broadcastedDims` must be properly specified or
/// the helper will assert. This means:
///   1. `dstShape` must not be empty.
///   2. `broadcastedDims` must be confined to [0 .. rank(value.getVectorType)]
///   2. `dstShape` trimmed of the dimensions specified in `broadcastedDims`
//       must match the `value` shape.
Value BroadcastOp::createOrFoldBroadcastOp(
    OpBuilder &b, Value value, ArrayRef<int64_t> dstShape,
    const llvm::SetVector<int64_t> &broadcastedDims) {
  assert(!dstShape.empty() && "unexpected empty dst shape");

  // Well-formedness check.
  SmallVector<int64_t> checkShape;
  for (int i = 0, e = dstShape.size(); i < e; ++i) {
    if (broadcastedDims.contains(i))
      continue;
    checkShape.push_back(dstShape[i]);
  }
  assert(broadcastedDims.size() == dstShape.size() - checkShape.size() &&
         "ill-formed broadcastedDims contains values not confined to "
         "destVectorShape");

  Location loc = value.getLoc();
  Type elementType = getElementTypeOrSelf(value.getType());
  VectorType srcVectorType = llvm::dyn_cast<VectorType>(value.getType());
  VectorType dstVectorType = VectorType::get(dstShape, elementType);

  // Step 2. If scalar -> dstShape broadcast, just do it.
  if (!srcVectorType) {
    assert(checkShape.empty() &&
           "ill-formed createOrFoldBroadcastOp arguments");
    return b.createOrFold<vector::BroadcastOp>(loc, dstVectorType, value);
  }

  assert(srcVectorType.getShape().equals(checkShape) &&
         "ill-formed createOrFoldBroadcastOp arguments");

  // Step 3. Since vector.broadcast only allows creating leading dims,
  //   vector -> dstShape broadcast may require a transpose.
  // Traverse the dims in order and construct:
  //   1. The leading entries of the broadcastShape that is guaranteed to be
  //      achievable by a simple broadcast.
  //   2. The induced permutation for the subsequent vector.transpose that will
  //      bring us from `broadcastShape` back to he desired `dstShape`.
  // If the induced permutation is not the identity, create a vector.transpose.
  SmallVector<int64_t> broadcastShape, permutation(dstShape.size(), -1);
  broadcastShape.reserve(dstShape.size());
  // Consider the example:
  //   srcShape     = 2x4
  //   dstShape     = 1x2x3x4x5
  //   broadcastedDims = [0, 2, 4]
  //
  // We want to build:
  //   broadcastShape  = 1x3x5x2x4
  //   permutation     = [0, 2, 4,                 1, 3]
  //                      ---V---           -----V-----
  //            leading broadcast part      src shape part
  //
  // Note that the trailing dims of broadcastShape are exactly the srcShape
  // by construction.
  // nextSrcShapeDim is used to keep track of where in the permutation the
  // "src shape part" occurs.
  int64_t nextSrcShapeDim = broadcastedDims.size();
  for (int64_t i = 0, e = dstShape.size(); i < e; ++i) {
    if (broadcastedDims.contains(i)) {
      // 3.a. For each dim in the dst shape, if it is a broadcasted dim,
      // bring it to the head of the broadcastShape.
      // It will need to be permuted back from `broadcastShape.size() - 1` into
      // position `i`.
      broadcastShape.push_back(dstShape[i]);
      permutation[i] = broadcastShape.size() - 1;
    } else {
      // 3.b. Otherwise, the dim is not broadcasted, it comes from the src
      // shape and needs to be permuted into position `i`.
      // Don't touch `broadcastShape` here, the whole srcShape will be
      // appended after.
      permutation[i] = nextSrcShapeDim++;
    }
  }
  // 3.c. Append the srcShape.
  llvm::append_range(broadcastShape, srcVectorType.getShape());

  // Ensure there are no dim-1 broadcasts.
  assert(::computeBroadcastedUnitDims(srcVectorType.getShape(), broadcastShape)
             .empty() &&
         "unexpected dim-1 broadcast");

  VectorType broadcastType = VectorType::get(broadcastShape, elementType);
  assert(vector::isBroadcastableTo(value.getType(), broadcastType) ==
             vector::BroadcastableToResult::Success &&
         "must be broadcastable");
  Value res = b.createOrFold<vector::BroadcastOp>(loc, broadcastType, value);
  // Step 4. If we find any dimension that indeed needs to be permuted,
  // immediately return a new vector.transpose.
  for (int64_t i = 0, e = permutation.size(); i < e; ++i)
    if (permutation[i] != i)
      return b.createOrFold<vector::TransposeOp>(loc, res, permutation);
  // Otherwise return res.
  return res;
}

BroadcastableToResult
mlir::vector::isBroadcastableTo(Type srcType, VectorType dstVectorType,
                                std::pair<int, int> *mismatchingDims) {
  // Broadcast scalar to vector of the same element type.
  if (srcType.isIntOrIndexOrFloat() && dstVectorType &&
      getElementTypeOrSelf(srcType) == getElementTypeOrSelf(dstVectorType))
    return BroadcastableToResult::Success;
  // From now on, only vectors broadcast.
  VectorType srcVectorType = llvm::dyn_cast<VectorType>(srcType);
  if (!srcVectorType)
    return BroadcastableToResult::SourceTypeNotAVector;

  int64_t srcRank = srcVectorType.getRank();
  int64_t dstRank = dstVectorType.getRank();
  if (srcRank > dstRank)
    return BroadcastableToResult::SourceRankHigher;
  // Source has an exact match or singleton value for all trailing dimensions
  // (all leading dimensions are simply duplicated).
  int64_t lead = dstRank - srcRank;
  for (int64_t r = 0; r < srcRank; ++r) {
    int64_t srcDim = srcVectorType.getDimSize(r);
    int64_t dstDim = dstVectorType.getDimSize(lead + r);
    if (srcDim != 1 && srcDim != dstDim) {
      if (mismatchingDims) {
        mismatchingDims->first = srcDim;
        mismatchingDims->second = dstDim;
      }
      return BroadcastableToResult::DimensionMismatch;
    }
  }

  return BroadcastableToResult::Success;
}

LogicalResult BroadcastOp::verify() {
  std::pair<int, int> mismatchingDims;
  BroadcastableToResult res = isBroadcastableTo(
      getSourceType(), getResultVectorType(), &mismatchingDims);
  if (res == BroadcastableToResult::Success)
    return success();
  if (res == BroadcastableToResult::SourceRankHigher)
    return emitOpError("source rank higher than destination rank");
  if (res == BroadcastableToResult::DimensionMismatch)
    return emitOpError("dimension mismatch (")
           << mismatchingDims.first << " vs. " << mismatchingDims.second << ")";
  if (res == BroadcastableToResult::SourceTypeNotAVector)
    return emitOpError("source type is not a vector");
  llvm_unreachable("unexpected vector.broadcast op error");
}

OpFoldResult BroadcastOp::fold(FoldAdaptor adaptor) {
  if (getSourceType() == getResultVectorType())
    return getSource();
  if (!adaptor.getSource())
    return {};
  auto vectorType = getResultVectorType();
  if (llvm::isa<IntegerAttr, FloatAttr>(adaptor.getSource()))
    return DenseElementsAttr::get(vectorType, adaptor.getSource());
  if (auto attr = llvm::dyn_cast<SplatElementsAttr>(adaptor.getSource()))
    return DenseElementsAttr::get(vectorType, attr.getSplatValue<Attribute>());
  return {};
}

namespace {

// Fold broadcast1(broadcast2(x)) into broadcast1(x).
struct BroadcastFolder : public OpRewritePattern<BroadcastOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(BroadcastOp broadcastOp,
                                PatternRewriter &rewriter) const override {
    auto srcBroadcast = broadcastOp.getSource().getDefiningOp<BroadcastOp>();
    if (!srcBroadcast)
      return failure();
    rewriter.replaceOpWithNewOp<BroadcastOp>(broadcastOp,
                                             broadcastOp.getResultVectorType(),
                                             srcBroadcast.getSource());
    return success();
  }
};
} // namespace

void BroadcastOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                              MLIRContext *context) {
  // BroadcastToShapeCast is not a default canonicalization, it is opt-in by
  // calling `populateCastAwayVectorLeadingOneDimPatterns`
  results.add<BroadcastFolder>(context);
}

//===----------------------------------------------------------------------===//
// ShuffleOp
//===----------------------------------------------------------------------===//

void ShuffleOp::build(OpBuilder &builder, OperationState &result, Value v1,
                      Value v2, ArrayRef<int64_t> mask) {
  build(builder, result, v1, v2, getVectorSubscriptAttr(builder, mask));
}

LogicalResult ShuffleOp::verify() {
  VectorType resultType = getResultVectorType();
  VectorType v1Type = getV1VectorType();
  VectorType v2Type = getV2VectorType();
  // Verify ranks.
  int64_t resRank = resultType.getRank();
  int64_t v1Rank = v1Type.getRank();
  int64_t v2Rank = v2Type.getRank();
  bool wellFormed0DCase = v1Rank == 0 && v2Rank == 0 && resRank == 1;
  bool wellFormedNDCase = v1Rank == resRank && v2Rank == resRank;
  if (!wellFormed0DCase && !wellFormedNDCase)
    return emitOpError("rank mismatch");

  // Verify all but leading dimension sizes.
  for (int64_t r = 1; r < v1Rank; ++r) {
    int64_t resDim = resultType.getDimSize(r);
    int64_t v1Dim = v1Type.getDimSize(r);
    int64_t v2Dim = v2Type.getDimSize(r);
    if (resDim != v1Dim || v1Dim != v2Dim)
      return emitOpError("dimension mismatch");
  }
  // Verify mask length.
  auto maskAttr = getMask().getValue();
  int64_t maskLength = maskAttr.size();
  if (maskLength <= 0)
    return emitOpError("invalid mask length");
  if (maskLength != resultType.getDimSize(0))
    return emitOpError("mask length mismatch");
  // Verify all indices.
  int64_t indexSize = (v1Type.getRank() == 0 ? 1 : v1Type.getDimSize(0)) +
                      (v2Type.getRank() == 0 ? 1 : v2Type.getDimSize(0));
  for (const auto &en : llvm::enumerate(maskAttr)) {
    auto attr = llvm::dyn_cast<IntegerAttr>(en.value());
    if (!attr || attr.getInt() < 0 || attr.getInt() >= indexSize)
      return emitOpError("mask index #") << (en.index() + 1) << " out of range";
  }
  return success();
}

LogicalResult
ShuffleOp::inferReturnTypes(MLIRContext *, std::optional<Location>,
                            ShuffleOp::Adaptor adaptor,
                            SmallVectorImpl<Type> &inferredReturnTypes) {
  auto v1Type = llvm::cast<VectorType>(adaptor.getV1().getType());
  auto v1Rank = v1Type.getRank();
  // Construct resulting type: leading dimension matches mask
  // length, all trailing dimensions match the operands.
  SmallVector<int64_t, 4> shape;
  shape.reserve(v1Rank);
  shape.push_back(std::max<size_t>(1, adaptor.getMask().size()));
  // In the 0-D case there is no trailing shape to append.
  if (v1Rank > 0)
    llvm::append_range(shape, v1Type.getShape().drop_front());
  inferredReturnTypes.push_back(
      VectorType::get(shape, v1Type.getElementType()));
  return success();
}

static bool isStepIndexArray(ArrayAttr idxArr, uint64_t begin, size_t width) {
  uint64_t expected = begin;
  return idxArr.size() == width &&
         llvm::all_of(idxArr.getAsValueRange<IntegerAttr>(),
                      [&expected](auto attr) {
                        return attr.getZExtValue() == expected++;
                      });
}

OpFoldResult vector::ShuffleOp::fold(FoldAdaptor adaptor) {
  VectorType v1Type = getV1VectorType();
  // For consistency: 0-D shuffle return type is 1-D, this cannot be a folding
  // but must be a canonicalization into a vector.broadcast.
  if (v1Type.getRank() == 0)
    return {};

  // fold shuffle V1, V2, [0, 1, 2, 3] : <4xi32>, <2xi32> -> V1
  if (!v1Type.isScalable() &&
      isStepIndexArray(getMask(), 0, v1Type.getDimSize(0)))
    return getV1();
  // fold shuffle V1, V2, [4, 5] : <4xi32>, <2xi32> -> V2
  if (!getV1VectorType().isScalable() && !getV2VectorType().isScalable() &&
      isStepIndexArray(getMask(), getV1VectorType().getDimSize(0),
                       getV2VectorType().getDimSize(0)))
    return getV2();

  Attribute lhs = adaptor.getV1(), rhs = adaptor.getV2();
  if (!lhs || !rhs)
    return {};

  auto lhsType =
      llvm::cast<VectorType>(llvm::cast<DenseElementsAttr>(lhs).getType());
  // Only support 1-D for now to avoid complicated n-D DenseElementsAttr
  // manipulation.
  if (lhsType.getRank() != 1)
    return {};
  int64_t lhsSize = lhsType.getDimSize(0);

  SmallVector<Attribute> results;
  auto lhsElements = llvm::cast<DenseElementsAttr>(lhs).getValues<Attribute>();
  auto rhsElements = llvm::cast<DenseElementsAttr>(rhs).getValues<Attribute>();
  for (const auto &index : this->getMask().getAsValueRange<IntegerAttr>()) {
    int64_t i = index.getZExtValue();
    if (i >= lhsSize) {
      results.push_back(rhsElements[i - lhsSize]);
    } else {
      results.push_back(lhsElements[i]);
    }
  }

  return DenseElementsAttr::get(getResultVectorType(), results);
}

namespace {

// Pattern to rewrite a 0-D shuffle with [0] or [1] mask returning a 1-D vector
// to a broadcast.
struct Canonicalize0DShuffleOp : public OpRewritePattern<ShuffleOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ShuffleOp shuffleOp,
                                PatternRewriter &rewriter) const override {
    VectorType v1VectorType = shuffleOp.getV1VectorType();
    ArrayAttr mask = shuffleOp.getMask();
    if (v1VectorType.getRank() > 0)
      return failure();
    if (mask.size() != 1)
      return failure();
    Type resType = VectorType::Builder(v1VectorType).setShape({1});
    if (llvm::cast<IntegerAttr>(mask[0]).getInt() == 0)
      rewriter.replaceOpWithNewOp<vector::BroadcastOp>(shuffleOp, resType,
                                                       shuffleOp.getV1());
    else
      rewriter.replaceOpWithNewOp<vector::BroadcastOp>(shuffleOp, resType,
                                                       shuffleOp.getV2());
    return success();
  }
};

/// Pattern to rewrite a ShuffleOp(SplatOp, SplatOp) to SplatOp.
class ShuffleSplat final : public OpRewritePattern<ShuffleOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ShuffleOp op,
                                PatternRewriter &rewriter) const override {
    auto v1Splat = op.getV1().getDefiningOp<SplatOp>();
    auto v2Splat = op.getV2().getDefiningOp<SplatOp>();

    if (!v1Splat || !v2Splat)
      return failure();

    if (v1Splat.getInput() != v2Splat.getInput())
      return failure();

    rewriter.replaceOpWithNewOp<SplatOp>(op, op.getType(), v1Splat.getInput());
    return success();
  }
};

} // namespace

void ShuffleOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<ShuffleSplat, Canonicalize0DShuffleOp>(context);
}

//===----------------------------------------------------------------------===//
// InsertElementOp
//===----------------------------------------------------------------------===//

void InsertElementOp::build(OpBuilder &builder, OperationState &result,
                            Value source, Value dest) {
  build(builder, result, source, dest, {});
}

LogicalResult InsertElementOp::verify() {
  auto dstVectorType = getDestVectorType();
  if (dstVectorType.getRank() == 0) {
    if (getPosition())
      return emitOpError("expected position to be empty with 0-D vector");
    return success();
  }
  if (dstVectorType.getRank() != 1)
    return emitOpError("unexpected >1 vector rank");
  if (!getPosition())
    return emitOpError("expected position for 1-D vector");
  return success();
}

OpFoldResult vector::InsertElementOp::fold(FoldAdaptor adaptor) {
  // Skip the 0-D vector here.
  if (!adaptor.getPosition())
    return {};

  Attribute src = adaptor.getSource();
  Attribute dst = adaptor.getDest();
  Attribute pos = adaptor.getPosition();
  if (!src || !dst || !pos)
    return {};

  auto dstElements = llvm::cast<DenseElementsAttr>(dst).getValues<Attribute>();

  SmallVector<Attribute> results(dstElements);

  auto attr = llvm::dyn_cast<IntegerAttr>(pos);
  uint64_t posIdx = attr.getInt();

  results[posIdx] = src;

  return DenseElementsAttr::get(getDestVectorType(), results);
}

//===----------------------------------------------------------------------===//
// InsertOp
//===----------------------------------------------------------------------===//

// Convenience builder which assumes the values are constant indices.
void InsertOp::build(OpBuilder &builder, OperationState &result, Value source,
                     Value dest, ValueRange position) {
  SmallVector<int64_t, 4> positionConstants =
      llvm::to_vector<4>(llvm::map_range(position, [](Value pos) {
        return getConstantIntValue(pos).value();
      }));
  build(builder, result, source, dest, positionConstants);
}

LogicalResult InsertOp::verify() {
  ArrayRef<int64_t> position = getPosition();
  auto destVectorType = getDestVectorType();
  if (position.size() > static_cast<unsigned>(destVectorType.getRank()))
    return emitOpError(
        "expected position attribute of rank no greater than dest vector rank");
  auto srcVectorType = llvm::dyn_cast<VectorType>(getSourceType());
  if (srcVectorType &&
      (static_cast<unsigned>(srcVectorType.getRank()) + position.size() !=
       static_cast<unsigned>(destVectorType.getRank())))
    return emitOpError("expected position attribute rank + source rank to "
                       "match dest vector rank");
  if (!srcVectorType &&
      (position.size() != static_cast<unsigned>(destVectorType.getRank())))
    return emitOpError(
        "expected position attribute rank to match the dest vector rank");
  for (const auto &en : llvm::enumerate(position)) {
    int64_t attr = en.value();
    if (attr < 0 || attr >= destVectorType.getDimSize(en.index()))
      return emitOpError("expected position attribute #")
             << (en.index() + 1)
             << " to be a non-negative integer smaller than the corresponding "
                "dest vector dimension";
  }
  return success();
}

namespace {

// If insertOp is only inserting unit dimensions it can be transformed to a
// broadcast.
class InsertToBroadcast final : public OpRewritePattern<InsertOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOp insertOp,
                                PatternRewriter &rewriter) const override {
    auto srcVecType = llvm::dyn_cast<VectorType>(insertOp.getSourceType());
    if (!srcVecType || insertOp.getDestVectorType().getNumElements() !=
                           srcVecType.getNumElements())
      return failure();
    rewriter.replaceOpWithNewOp<BroadcastOp>(
        insertOp, insertOp.getDestVectorType(), insertOp.getSource());
    return success();
  }
};

/// Pattern to rewrite a InsertOp(SplatOp, SplatOp) to SplatOp.
class InsertSplatToSplat final : public OpRewritePattern<InsertOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOp op,
                                PatternRewriter &rewriter) const override {
    auto srcSplat = op.getSource().getDefiningOp<SplatOp>();
    auto dstSplat = op.getDest().getDefiningOp<SplatOp>();

    if (!srcSplat || !dstSplat)
      return failure();

    if (srcSplat.getInput() != dstSplat.getInput())
      return failure();

    rewriter.replaceOpWithNewOp<SplatOp>(op, op.getType(), srcSplat.getInput());
    return success();
  }
};

// Pattern to rewrite a InsertOp(ConstantOp into ConstantOp) -> ConstantOp.
class InsertOpConstantFolder final : public OpRewritePattern<InsertOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  // Do not create constants with more than `vectorSizeFoldThreashold` elements,
  // unless the source vector constant has a single use.
  static constexpr int64_t vectorSizeFoldThreshold = 256;

  LogicalResult matchAndRewrite(InsertOp op,
                                PatternRewriter &rewriter) const override {
    // Return if 'InsertOp' operand is not defined by a compatible vector
    // ConstantOp.
    TypedValue<VectorType> destVector = op.getDest();
    Attribute vectorDestCst;
    if (!matchPattern(destVector, m_Constant(&vectorDestCst)))
      return failure();

    VectorType destTy = destVector.getType();
    if (destTy.isScalable())
      return failure();

    // Make sure we do not create too many large constants.
    if (destTy.getNumElements() > vectorSizeFoldThreshold &&
        !destVector.hasOneUse())
      return failure();

    auto denseDest = llvm::cast<DenseElementsAttr>(vectorDestCst);

    Value sourceValue = op.getSource();
    Attribute sourceCst;
    if (!matchPattern(sourceValue, m_Constant(&sourceCst)))
      return failure();

    // Calculate the linearized position of the continuous chunk of elements to
    // insert.
    llvm::SmallVector<int64_t> completePositions(destTy.getRank(), 0);
    copy(op.getPosition(), completePositions.begin());
    int64_t insertBeginPosition =
        linearize(completePositions, computeStrides(destTy.getShape()));

    SmallVector<Attribute> insertedValues;
    if (auto denseSource = llvm::dyn_cast<DenseElementsAttr>(sourceCst))
      llvm::append_range(insertedValues, denseSource.getValues<Attribute>());
    else
      insertedValues.push_back(sourceCst);

    auto allValues = llvm::to_vector(denseDest.getValues<Attribute>());
    copy(insertedValues, allValues.begin() + insertBeginPosition);
    auto newAttr = DenseElementsAttr::get(destTy, allValues);

    rewriter.replaceOpWithNewOp<arith::ConstantOp>(op, newAttr);
    return success();
  }
};

} // namespace

void InsertOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                           MLIRContext *context) {
  results.add<InsertToBroadcast, BroadcastFolder, InsertSplatToSplat,
              InsertOpConstantFolder>(context);
}

// Eliminates insert operations that produce values identical to their source
// value. This happens when the source and destination vectors have identical
// sizes.
OpFoldResult vector::InsertOp::fold(FoldAdaptor adaptor) {
  if (getPosition().empty())
    return getSource();
  return {};
}

//===----------------------------------------------------------------------===//
// InsertStridedSliceOp
//===----------------------------------------------------------------------===//

void InsertStridedSliceOp::build(OpBuilder &builder, OperationState &result,
                                 Value source, Value dest,
                                 ArrayRef<int64_t> offsets,
                                 ArrayRef<int64_t> strides) {
  result.addOperands({source, dest});
  auto offsetsAttr = getVectorSubscriptAttr(builder, offsets);
  auto stridesAttr = getVectorSubscriptAttr(builder, strides);
  result.addTypes(dest.getType());
  result.addAttribute(InsertStridedSliceOp::getOffsetsAttrName(result.name),
                      offsetsAttr);
  result.addAttribute(InsertStridedSliceOp::getStridesAttrName(result.name),
                      stridesAttr);
}

// TODO: Should be moved to Tablegen ConfinedAttr attributes.
template <typename OpType>
static LogicalResult isIntegerArrayAttrSmallerThanShape(OpType op,
                                                        ArrayAttr arrayAttr,
                                                        ArrayRef<int64_t> shape,
                                                        StringRef attrName) {
  if (arrayAttr.size() > shape.size())
    return op.emitOpError("expected ")
           << attrName << " attribute of rank no greater than vector rank";
  return success();
}

// Returns true if all integers in `arrayAttr` are in the half-open [min, max}
// interval. If `halfOpen` is true then the admissible interval is [min, max).
// Otherwise, the admissible interval is [min, max].
template <typename OpType>
static LogicalResult
isIntegerArrayAttrConfinedToRange(OpType op, ArrayAttr arrayAttr, int64_t min,
                                  int64_t max, StringRef attrName,
                                  bool halfOpen = true) {
  for (auto attr : arrayAttr) {
    auto val = llvm::cast<IntegerAttr>(attr).getInt();
    auto upper = max;
    if (!halfOpen)
      upper += 1;
    if (val < min || val >= upper)
      return op.emitOpError("expected ") << attrName << " to be confined to ["
                                         << min << ", " << upper << ")";
  }
  return success();
}

// Returns true if all integers in `arrayAttr` are in the half-open [min, max}
// interval. If `halfOpen` is true then the admissible interval is [min, max).
// Otherwise, the admissible interval is [min, max].
template <typename OpType>
static LogicalResult
isIntegerArrayAttrConfinedToShape(OpType op, ArrayAttr arrayAttr,
                                  ArrayRef<int64_t> shape, StringRef attrName,
                                  bool halfOpen = true, int64_t min = 0) {
  for (auto [index, attrDimPair] :
       llvm::enumerate(llvm::zip_first(arrayAttr, shape))) {
    int64_t val = llvm::cast<IntegerAttr>(std::get<0>(attrDimPair)).getInt();
    int64_t max = std::get<1>(attrDimPair);
    if (!halfOpen)
      max += 1;
    if (val < min || val >= max)
      return op.emitOpError("expected ")
             << attrName << " dimension " << index << " to be confined to ["
             << min << ", " << max << ")";
  }
  return success();
}

// Returns true if all integers in `arrayAttr` are in the interval [min, max}.
// interval. If `halfOpen` is true then the admissible interval is [min, max).
// Otherwise, the admissible interval is [min, max].
template <typename OpType>
static LogicalResult isSumOfIntegerArrayAttrConfinedToShape(
    OpType op, ArrayAttr arrayAttr1, ArrayAttr arrayAttr2,
    ArrayRef<int64_t> shape, StringRef attrName1, StringRef attrName2,
    bool halfOpen = true, int64_t min = 1) {
  assert(arrayAttr1.size() <= shape.size());
  assert(arrayAttr2.size() <= shape.size());
  for (auto [index, it] :
       llvm::enumerate(llvm::zip(arrayAttr1, arrayAttr2, shape))) {
    auto val1 = llvm::cast<IntegerAttr>(std::get<0>(it)).getInt();
    auto val2 = llvm::cast<IntegerAttr>(std::get<1>(it)).getInt();
    int64_t max = std::get<2>(it);
    if (!halfOpen)
      max += 1;
    if (val1 + val2 < 0 || val1 + val2 >= max)
      return op.emitOpError("expected sum(")
             << attrName1 << ", " << attrName2 << ") dimension " << index
             << " to be confined to [" << min << ", " << max << ")";
  }
  return success();
}

static ArrayAttr makeI64ArrayAttr(ArrayRef<int64_t> values,
                                  MLIRContext *context) {
  auto attrs = llvm::map_range(values, [context](int64_t v) -> Attribute {
    return IntegerAttr::get(IntegerType::get(context, 64), APInt(64, v));
  });
  return ArrayAttr::get(context, llvm::to_vector<8>(attrs));
}

LogicalResult InsertStridedSliceOp::verify() {
  auto sourceVectorType = getSourceVectorType();
  auto destVectorType = getDestVectorType();
  auto offsets = getOffsetsAttr();
  auto strides = getStridesAttr();
  if (offsets.size() != static_cast<unsigned>(destVectorType.getRank()))
    return emitOpError(
        "expected offsets of same size as destination vector rank");
  if (strides.size() != static_cast<unsigned>(sourceVectorType.getRank()))
    return emitOpError("expected strides of same size as source vector rank");
  if (sourceVectorType.getRank() > destVectorType.getRank())
    return emitOpError(
        "expected source rank to be no greater than destination rank");

  auto sourceShape = sourceVectorType.getShape();
  auto destShape = destVectorType.getShape();
  SmallVector<int64_t, 4> sourceShapeAsDestShape(
      destShape.size() - sourceShape.size(), 0);
  sourceShapeAsDestShape.append(sourceShape.begin(), sourceShape.end());
  auto offName = InsertStridedSliceOp::getOffsetsAttrName();
  auto stridesName = InsertStridedSliceOp::getStridesAttrName();
  if (failed(isIntegerArrayAttrConfinedToShape(*this, offsets, destShape,
                                               offName)) ||
      failed(isIntegerArrayAttrConfinedToRange(*this, strides, 1, 1,
                                               stridesName,
                                               /*halfOpen=*/false)) ||
      failed(isSumOfIntegerArrayAttrConfinedToShape(
          *this, offsets,
          makeI64ArrayAttr(sourceShapeAsDestShape, getContext()), destShape,
          offName, "source vector shape",
          /*halfOpen=*/false, /*min=*/1)))
    return failure();

  return success();
}

namespace {
/// Pattern to rewrite an InsertStridedSliceOp(SplatOp(X):src_type,
/// SplatOp(X):dst_type) to SplatOp(X):dst_type.
class FoldInsertStridedSliceSplat final
    : public OpRewritePattern<InsertStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertStridedSliceOp insertStridedSliceOp,
                                PatternRewriter &rewriter) const override {
    auto srcSplatOp =
        insertStridedSliceOp.getSource().getDefiningOp<vector::SplatOp>();
    auto destSplatOp =
        insertStridedSliceOp.getDest().getDefiningOp<vector::SplatOp>();

    if (!srcSplatOp || !destSplatOp)
      return failure();

    if (srcSplatOp.getInput() != destSplatOp.getInput())
      return failure();

    rewriter.replaceOp(insertStridedSliceOp, insertStridedSliceOp.getDest());
    return success();
  }
};

/// Pattern to rewrite an InsertStridedSliceOp(ExtractStridedSliceOp(dst), dst)
/// to dst.
class FoldInsertStridedSliceOfExtract final
    : public OpRewritePattern<InsertStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertStridedSliceOp insertStridedSliceOp,
                                PatternRewriter &rewriter) const override {
    auto extractStridedSliceOp =
        insertStridedSliceOp.getSource()
            .getDefiningOp<vector::ExtractStridedSliceOp>();

    if (!extractStridedSliceOp)
      return failure();

    if (extractStridedSliceOp.getOperand() != insertStridedSliceOp.getDest())
      return failure();

    // Check if have the same strides and offsets.
    if (extractStridedSliceOp.getStrides() !=
            insertStridedSliceOp.getStrides() ||
        extractStridedSliceOp.getOffsets() != insertStridedSliceOp.getOffsets())
      return failure();

    rewriter.replaceOp(insertStridedSliceOp, insertStridedSliceOp.getDest());
    return success();
  }
};

// Pattern to rewrite an InsertStridedSliceOp(ConstantOp into ConstantOp) ->
// ConstantOp.
class InsertStridedSliceConstantFolder final
    : public OpRewritePattern<InsertStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  // Do not create constants with more than `vectorSizeFoldThreashold` elements,
  // unless the source vector constant has a single use.
  static constexpr int64_t vectorSizeFoldThreshold = 256;

  LogicalResult matchAndRewrite(InsertStridedSliceOp op,
                                PatternRewriter &rewriter) const override {
    // Return if 'InsertOp' operand is not defined by a compatible vector
    // ConstantOp.
    TypedValue<VectorType> destVector = op.getDest();
    Attribute vectorDestCst;
    if (!matchPattern(destVector, m_Constant(&vectorDestCst)))
      return failure();

    VectorType destTy = destVector.getType();
    if (destTy.isScalable())
      return failure();

    // Make sure we do not create too many large constants.
    if (destTy.getNumElements() > vectorSizeFoldThreshold &&
        !destVector.hasOneUse())
      return failure();

    auto denseDest = llvm::cast<DenseElementsAttr>(vectorDestCst);

    TypedValue<VectorType> sourceValue = op.getSource();
    Attribute sourceCst;
    if (!matchPattern(sourceValue, m_Constant(&sourceCst)))
      return failure();

    // TODO: Handle non-unit strides when they become available.
    if (op.hasNonUnitStrides())
      return failure();

    VectorType sliceVecTy = sourceValue.getType();
    ArrayRef<int64_t> sliceShape = sliceVecTy.getShape();
    int64_t rankDifference = destTy.getRank() - sliceVecTy.getRank();
    SmallVector<int64_t, 4> offsets = getI64SubArray(op.getOffsets());
    SmallVector<int64_t, 4> destStrides = computeStrides(destTy.getShape());

    // Calcualte the destination element indices by enumerating all slice
    // positions within the destination and linearizing them. The enumeration
    // order is lexicographic which yields a sequence of monotonically
    // increasing linearized position indices.
    // Because the destination may have higher dimensionality then the slice,
    // we keep track of two overlapping sets of positions and offsets.
    auto denseSlice = llvm::cast<DenseElementsAttr>(sourceCst);
    auto sliceValuesIt = denseSlice.value_begin<Attribute>();
    auto newValues = llvm::to_vector(denseDest.getValues<Attribute>());
    SmallVector<int64_t> currDestPosition(offsets.begin(), offsets.end());
    MutableArrayRef<int64_t> currSlicePosition(
        currDestPosition.begin() + rankDifference, currDestPosition.end());
    ArrayRef<int64_t> sliceOffsets(offsets.begin() + rankDifference,
                                   offsets.end());
    do {
      int64_t linearizedPosition = linearize(currDestPosition, destStrides);
      assert(linearizedPosition < destTy.getNumElements() && "Invalid index");
      assert(sliceValuesIt != denseSlice.value_end<Attribute>() &&
             "Invalid slice element");
      newValues[linearizedPosition] = *sliceValuesIt;
      ++sliceValuesIt;
    } while (succeeded(
        incSlicePosition(currSlicePosition, sliceShape, sliceOffsets)));

    auto newAttr = DenseElementsAttr::get(destTy, newValues);
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(op, newAttr);
    return success();
  }
};

} // namespace

void vector::InsertStridedSliceOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.add<FoldInsertStridedSliceSplat, FoldInsertStridedSliceOfExtract,
              InsertStridedSliceConstantFolder>(context);
}

OpFoldResult InsertStridedSliceOp::fold(FoldAdaptor adaptor) {
  if (getSourceVectorType() == getDestVectorType())
    return getSource();
  return {};
}

//===----------------------------------------------------------------------===//
// OuterProductOp
//===----------------------------------------------------------------------===//

/// Build an op without mask, use the type of `acc` as the return type.
void OuterProductOp::build(OpBuilder &builder, OperationState &result,
                           Value lhs, Value rhs, Value acc) {
  result.addOperands({lhs, rhs, acc});
  result.addTypes(acc.getType());
}

void OuterProductOp::print(OpAsmPrinter &p) {
  p << " " << getLhs() << ", " << getRhs();
  if (!getAcc().empty()) {
    p << ", " << getAcc();
    p.printOptionalAttrDict((*this)->getAttrs());
  }
  p << " : " << getLhs().getType() << ", " << getRhs().getType();
}

ParseResult OuterProductOp::parse(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::UnresolvedOperand, 3> operandsInfo;
  Type tLHS, tRHS;
  if (parser.parseOperandList(operandsInfo) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(tLHS) || parser.parseComma() ||
      parser.parseType(tRHS))
    return failure();
  if (operandsInfo.size() < 2)
    return parser.emitError(parser.getNameLoc(),
                            "expected at least 2 operands");
  VectorType vLHS = llvm::dyn_cast<VectorType>(tLHS);
  VectorType vRHS = llvm::dyn_cast<VectorType>(tRHS);
  if (!vLHS)
    return parser.emitError(parser.getNameLoc(),
                            "expected vector type for operand #1");

  VectorType resType;
  if (vRHS) {
    SmallVector<bool> scalableDimsRes{vLHS.getScalableDims()[0],
                                      vRHS.getScalableDims()[0]};
    resType = VectorType::get({vLHS.getDimSize(0), vRHS.getDimSize(0)},
                              vLHS.getElementType(), scalableDimsRes);
  } else {
    // Scalar RHS operand
    SmallVector<bool> scalableDimsRes{vLHS.getScalableDims()[0]};
    resType = VectorType::get({vLHS.getDimSize(0)}, vLHS.getElementType(),
                              scalableDimsRes);
  }

  if (!result.attributes.get(OuterProductOp::getKindAttrName(result.name))) {
    result.attributes.append(
        OuterProductOp::getKindAttrName(result.name),
        CombiningKindAttr::get(result.getContext(),
                               OuterProductOp::getDefaultKind()));
  }

  return failure(
      parser.resolveOperand(operandsInfo[0], tLHS, result.operands) ||
      parser.resolveOperand(operandsInfo[1], tRHS, result.operands) ||
      (operandsInfo.size() > 2 &&
       parser.resolveOperand(operandsInfo[2], resType, result.operands)) ||
      parser.addTypeToList(resType, result.types));
}

LogicalResult OuterProductOp::verify() {
  Type tRHS = getOperandTypeRHS();
  VectorType vLHS = getOperandVectorTypeLHS(),
             vRHS = llvm::dyn_cast<VectorType>(tRHS),
             vACC = getOperandVectorTypeACC(), vRES = getResultVectorType();

  if (vLHS.getRank() != 1)
    return emitOpError("expected 1-d vector for operand #1");

  if (vRHS) {
    // Proper OUTER operation.
    if (vRHS.getRank() != 1)
      return emitOpError("expected 1-d vector for operand #2");
    if (vRES.getRank() != 2)
      return emitOpError("expected 2-d vector result");
    if (vLHS.getDimSize(0) != vRES.getDimSize(0))
      return emitOpError("expected #1 operand dim to match result dim #1");
    if (vRHS.getDimSize(0) != vRES.getDimSize(1))
      return emitOpError("expected #2 operand dim to match result dim #2");
    if (vRHS.isScalable() != vLHS.isScalable())
      return emitOpError("expected either all or none of vector operands #1 "
                         "and #2 to be scalable");
  } else {
    // An AXPY operation.
    if (vRES.getRank() != 1)
      return emitOpError("expected 1-d vector result");
    if (vLHS.getDimSize(0) != vRES.getDimSize(0))
      return emitOpError("expected #1 operand dim to match result dim #1");
  }

  if (vACC && vACC != vRES)
    return emitOpError("expected operand #3 of same type as result type");

  // Verify supported combining kind.
  if (!isSupportedCombiningKind(getKind(), vRES.getElementType()))
    return emitOpError("unsupported outerproduct type");

  return success();
}

// MaskableOpInterface methods.

/// Returns the mask type expected by this operation. Mostly used for
/// verification purposes. It requires the operation to be vectorized."
Type OuterProductOp::getExpectedMaskType() {
  auto vecType = this->getResultVectorType();
  return VectorType::get(vecType.getShape(),
                         IntegerType::get(vecType.getContext(), /*width=*/1),
                         vecType.getScalableDims());
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

LogicalResult ReshapeOp::verify() {
  // Verify that rank(numInputs/outputs) + numFixedVec dim matches vec rank.
  auto inputVectorType = getInputVectorType();
  auto outputVectorType = getOutputVectorType();
  int64_t inputShapeRank = getNumInputShapeSizes();
  int64_t outputShapeRank = getNumOutputShapeSizes();
  SmallVector<int64_t, 4> fixedVectorSizes;
  getFixedVectorSizes(fixedVectorSizes);
  int64_t numFixedVectorSizes = fixedVectorSizes.size();

  if (inputVectorType.getRank() != inputShapeRank + numFixedVectorSizes)
    return emitError("invalid input shape for vector type ") << inputVectorType;

  if (outputVectorType.getRank() != outputShapeRank + numFixedVectorSizes)
    return emitError("invalid output shape for vector type ")
           << outputVectorType;

  // Verify that the 'fixedVectorSizes' match an input/output vector shape
  // suffix.
  unsigned inputVectorRank = inputVectorType.getRank();
  for (unsigned i = 0; i < numFixedVectorSizes; ++i) {
    unsigned index = inputVectorRank - numFixedVectorSizes - i;
    if (fixedVectorSizes[i] != inputVectorType.getShape()[index])
      return emitError("fixed vector size must match input vector for dim ")
             << i;
  }

  unsigned outputVectorRank = outputVectorType.getRank();
  for (unsigned i = 0; i < numFixedVectorSizes; ++i) {
    unsigned index = outputVectorRank - numFixedVectorSizes - i;
    if (fixedVectorSizes[i] != outputVectorType.getShape()[index])
      return emitError("fixed vector size must match output vector for dim ")
             << i;
  }

  // If all shape operands are produced by constant ops, verify that product
  // of dimensions for input/output shape match.
  auto isDefByConstant = [](Value operand) {
    return getConstantIntValue(operand).has_value();
  };
  if (llvm::all_of(getInputShape(), isDefByConstant) &&
      llvm::all_of(getOutputShape(), isDefByConstant)) {
    int64_t numInputElements = 1;
    for (auto operand : getInputShape())
      numInputElements *= getConstantIntValue(operand).value();
    int64_t numOutputElements = 1;
    for (auto operand : getOutputShape())
      numOutputElements *= getConstantIntValue(operand).value();
    if (numInputElements != numOutputElements)
      return emitError("product of input and output shape sizes must match");
  }
  return success();
}

void ReshapeOp::getFixedVectorSizes(SmallVectorImpl<int64_t> &results) {
  populateFromInt64AttrArray(getFixedVectorSizes(), results);
}

//===----------------------------------------------------------------------===//
// ExtractStridedSliceOp
//===----------------------------------------------------------------------===//

// Inference works as follows:
//   1. Add 'sizes' from prefix of dims in 'offsets'.
//   2. Add sizes from 'vectorType' for remaining dims.
static Type inferStridedSliceOpResultType(VectorType vectorType,
                                          ArrayAttr offsets, ArrayAttr sizes,
                                          ArrayAttr strides) {
  assert(offsets.size() == sizes.size() && offsets.size() == strides.size());
  SmallVector<int64_t, 4> shape;
  shape.reserve(vectorType.getRank());
  unsigned idx = 0;
  for (unsigned e = offsets.size(); idx < e; ++idx)
    shape.push_back(llvm::cast<IntegerAttr>(sizes[idx]).getInt());
  for (unsigned e = vectorType.getShape().size(); idx < e; ++idx)
    shape.push_back(vectorType.getShape()[idx]);

  return VectorType::get(shape, vectorType.getElementType());
}

void ExtractStridedSliceOp::build(OpBuilder &builder, OperationState &result,
                                  Value source, ArrayRef<int64_t> offsets,
                                  ArrayRef<int64_t> sizes,
                                  ArrayRef<int64_t> strides) {
  result.addOperands(source);
  auto offsetsAttr = getVectorSubscriptAttr(builder, offsets);
  auto sizesAttr = getVectorSubscriptAttr(builder, sizes);
  auto stridesAttr = getVectorSubscriptAttr(builder, strides);
  result.addTypes(
      inferStridedSliceOpResultType(llvm::cast<VectorType>(source.getType()),
                                    offsetsAttr, sizesAttr, stridesAttr));
  result.addAttribute(ExtractStridedSliceOp::getOffsetsAttrName(result.name),
                      offsetsAttr);
  result.addAttribute(ExtractStridedSliceOp::getSizesAttrName(result.name),
                      sizesAttr);
  result.addAttribute(ExtractStridedSliceOp::getStridesAttrName(result.name),
                      stridesAttr);
}

LogicalResult ExtractStridedSliceOp::verify() {
  auto type = getSourceVectorType();
  auto offsets = getOffsetsAttr();
  auto sizes = getSizesAttr();
  auto strides = getStridesAttr();
  if (offsets.size() != sizes.size() || offsets.size() != strides.size())
    return emitOpError(
        "expected offsets, sizes and strides attributes of same size");

  auto shape = type.getShape();
  auto offName = getOffsetsAttrName();
  auto sizesName = getSizesAttrName();
  auto stridesName = getStridesAttrName();
  if (failed(
          isIntegerArrayAttrSmallerThanShape(*this, offsets, shape, offName)) ||
      failed(
          isIntegerArrayAttrSmallerThanShape(*this, sizes, shape, sizesName)) ||
      failed(isIntegerArrayAttrSmallerThanShape(*this, strides, shape,
                                                stridesName)) ||
      failed(
          isIntegerArrayAttrConfinedToShape(*this, offsets, shape, offName)) ||
      failed(isIntegerArrayAttrConfinedToShape(*this, sizes, shape, sizesName,
                                               /*halfOpen=*/false,
                                               /*min=*/1)) ||
      failed(isIntegerArrayAttrConfinedToRange(*this, strides, 1, 1,
                                               stridesName,
                                               /*halfOpen=*/false)) ||
      failed(isSumOfIntegerArrayAttrConfinedToShape(*this, offsets, sizes,
                                                    shape, offName, sizesName,
                                                    /*halfOpen=*/false)))
    return failure();

  auto resultType = inferStridedSliceOpResultType(getSourceVectorType(),
                                                  offsets, sizes, strides);
  if (getResult().getType() != resultType)
    return emitOpError("expected result type to be ") << resultType;

  return success();
}

// When the source of ExtractStrided comes from a chain of InsertStrided ops try
// to use the source of the InsertStrided ops if we can detect that the
// extracted vector is a subset of one of the vector inserted.
static LogicalResult
foldExtractStridedOpFromInsertChain(ExtractStridedSliceOp op) {
  // Helper to extract integer out of ArrayAttr.
  auto getElement = [](ArrayAttr array, int idx) {
    return llvm::cast<IntegerAttr>(array[idx]).getInt();
  };
  ArrayAttr extractOffsets = op.getOffsets();
  ArrayAttr extractStrides = op.getStrides();
  ArrayAttr extractSizes = op.getSizes();
  auto insertOp = op.getVector().getDefiningOp<InsertStridedSliceOp>();
  while (insertOp) {
    if (op.getSourceVectorType().getRank() !=
        insertOp.getSourceVectorType().getRank())
      return failure();
    ArrayAttr insertOffsets = insertOp.getOffsets();
    ArrayAttr insertStrides = insertOp.getStrides();
    // If the rank of extract is greater than the rank of insert, we are likely
    // extracting a partial chunk of the vector inserted.
    if (extractOffsets.size() > insertOffsets.size())
      return failure();
    bool patialoverlap = false;
    bool disjoint = false;
    SmallVector<int64_t, 4> offsetDiffs;
    for (unsigned dim = 0, e = extractOffsets.size(); dim < e; ++dim) {
      if (getElement(extractStrides, dim) != getElement(insertStrides, dim))
        return failure();
      int64_t start = getElement(insertOffsets, dim);
      int64_t end = start + insertOp.getSourceVectorType().getDimSize(dim);
      int64_t offset = getElement(extractOffsets, dim);
      int64_t size = getElement(extractSizes, dim);
      // Check if the start of the extract offset is in the interval inserted.
      if (start <= offset && offset < end) {
        // If the extract interval overlaps but is not fully included we may
        // have a partial overlap that will prevent any folding.
        if (offset + size > end)
          patialoverlap = true;
        offsetDiffs.push_back(offset - start);
        continue;
      }
      disjoint = true;
      break;
    }
    // The extract element chunk is a subset of the insert element.
    if (!disjoint && !patialoverlap) {
      op.setOperand(insertOp.getSource());
      // OpBuilder is only used as a helper to build an I64ArrayAttr.
      OpBuilder b(op.getContext());
      op.setOffsetsAttr(b.getI64ArrayAttr(offsetDiffs));
      return success();
    }
    // If the chunk extracted is disjoint from the chunk inserted, keep looking
    // in the insert chain.
    if (disjoint)
      insertOp = insertOp.getDest().getDefiningOp<InsertStridedSliceOp>();
    else {
      // The extracted vector partially overlap the inserted vector, we cannot
      // fold.
      return failure();
    }
  }
  return failure();
}

OpFoldResult ExtractStridedSliceOp::fold(FoldAdaptor adaptor) {
  if (getSourceVectorType() == getResult().getType())
    return getVector();
  if (succeeded(foldExtractStridedOpFromInsertChain(*this)))
    return getResult();
  return {};
}

void ExtractStridedSliceOp::getOffsets(SmallVectorImpl<int64_t> &results) {
  populateFromInt64AttrArray(getOffsets(), results);
}

namespace {

// Pattern to rewrite an ExtractStridedSliceOp(ConstantMaskOp) to
// ConstantMaskOp.
class StridedSliceConstantMaskFolder final
    : public OpRewritePattern<ExtractStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractStridedSliceOp extractStridedSliceOp,
                                PatternRewriter &rewriter) const override {
    // Return if 'extractStridedSliceOp' operand is not defined by a
    // ConstantMaskOp.
    auto *defOp = extractStridedSliceOp.getVector().getDefiningOp();
    auto constantMaskOp = dyn_cast_or_null<ConstantMaskOp>(defOp);
    if (!constantMaskOp)
      return failure();
    // Return if 'extractStridedSliceOp' has non-unit strides.
    if (extractStridedSliceOp.hasNonUnitStrides())
      return failure();
    // Gather constant mask dimension sizes.
    SmallVector<int64_t, 4> maskDimSizes;
    populateFromInt64AttrArray(constantMaskOp.getMaskDimSizes(), maskDimSizes);
    // Gather strided slice offsets and sizes.
    SmallVector<int64_t, 4> sliceOffsets;
    populateFromInt64AttrArray(extractStridedSliceOp.getOffsets(),
                               sliceOffsets);
    SmallVector<int64_t, 4> sliceSizes;
    populateFromInt64AttrArray(extractStridedSliceOp.getSizes(), sliceSizes);

    // Compute slice of vector mask region.
    SmallVector<int64_t, 4> sliceMaskDimSizes;
    sliceMaskDimSizes.reserve(maskDimSizes.size());
    for (auto [maskDimSize, sliceOffset, sliceSize] :
         llvm::zip(maskDimSizes, sliceOffsets, sliceSizes)) {
      int64_t sliceMaskDimSize = std::max(
          static_cast<int64_t>(0),
          std::min(sliceOffset + sliceSize, maskDimSize) - sliceOffset);
      sliceMaskDimSizes.push_back(sliceMaskDimSize);
    }
    // Add unchanged dimensions.
    if (sliceMaskDimSizes.size() < maskDimSizes.size())
      for (size_t i = sliceMaskDimSizes.size(); i < maskDimSizes.size(); ++i)
        sliceMaskDimSizes.push_back(maskDimSizes[i]);
    // If any of 'sliceMaskDimSizes' are zero, then set all to zero (masked
    // region is a conjunction of mask dim intervals).
    if (llvm::is_contained(sliceMaskDimSizes, 0))
      sliceMaskDimSizes.assign(maskDimSizes.size(), 0);

    // Replace 'extractStridedSliceOp' with ConstantMaskOp with sliced mask
    // region.
    rewriter.replaceOpWithNewOp<ConstantMaskOp>(
        extractStridedSliceOp, extractStridedSliceOp.getResult().getType(),
        vector::getVectorSubscriptAttr(rewriter, sliceMaskDimSizes));
    return success();
  }
};

// Pattern to rewrite a ExtractStridedSliceOp(splat ConstantOp) -> ConstantOp.
class StridedSliceSplatConstantFolder final
    : public OpRewritePattern<ExtractStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractStridedSliceOp extractStridedSliceOp,
                                PatternRewriter &rewriter) const override {
    // Return if 'ExtractStridedSliceOp' operand is not defined by a splat
    // ConstantOp.
    Value sourceVector = extractStridedSliceOp.getVector();
    Attribute vectorCst;
    if (!matchPattern(sourceVector, m_Constant(&vectorCst)))
      return failure();

    auto splat = llvm::dyn_cast<SplatElementsAttr>(vectorCst);
    if (!splat)
      return failure();

    auto newAttr = SplatElementsAttr::get(extractStridedSliceOp.getType(),
                                          splat.getSplatValue<Attribute>());
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(extractStridedSliceOp,
                                                   newAttr);
    return success();
  }
};

// Pattern to rewrite a ExtractStridedSliceOp(non-splat ConstantOp) ->
// ConstantOp.
class StridedSliceNonSplatConstantFolder final
    : public OpRewritePattern<ExtractStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractStridedSliceOp extractStridedSliceOp,
                                PatternRewriter &rewriter) const override {
    // Return if 'ExtractStridedSliceOp' operand is not defined by a non-splat
    // ConstantOp.
    Value sourceVector = extractStridedSliceOp.getVector();
    Attribute vectorCst;
    if (!matchPattern(sourceVector, m_Constant(&vectorCst)))
      return failure();

    // The splat case is handled by `StridedSliceSplatConstantFolder`.
    auto dense = llvm::dyn_cast<DenseElementsAttr>(vectorCst);
    if (!dense || dense.isSplat())
      return failure();

    // TODO: Handle non-unit strides when they become available.
    if (extractStridedSliceOp.hasNonUnitStrides())
      return failure();

    auto sourceVecTy = llvm::cast<VectorType>(sourceVector.getType());
    ArrayRef<int64_t> sourceShape = sourceVecTy.getShape();
    SmallVector<int64_t, 4> sourceStrides = computeStrides(sourceShape);

    VectorType sliceVecTy = extractStridedSliceOp.getType();
    ArrayRef<int64_t> sliceShape = sliceVecTy.getShape();
    int64_t sliceRank = sliceVecTy.getRank();

    // Expand offsets and sizes to match the vector rank.
    SmallVector<int64_t, 4> offsets(sliceRank, 0);
    copy(getI64SubArray(extractStridedSliceOp.getOffsets()), offsets.begin());

    SmallVector<int64_t, 4> sizes(sourceShape.begin(), sourceShape.end());
    copy(getI64SubArray(extractStridedSliceOp.getSizes()), sizes.begin());

    // Calculate the slice elements by enumerating all slice positions and
    // linearizing them. The enumeration order is lexicographic which yields a
    // sequence of monotonically increasing linearized position indices.
    auto denseValuesBegin = dense.value_begin<Attribute>();
    SmallVector<Attribute> sliceValues;
    sliceValues.reserve(sliceVecTy.getNumElements());
    SmallVector<int64_t> currSlicePosition(offsets.begin(), offsets.end());
    do {
      int64_t linearizedPosition = linearize(currSlicePosition, sourceStrides);
      assert(linearizedPosition < sourceVecTy.getNumElements() &&
             "Invalid index");
      sliceValues.push_back(*(denseValuesBegin + linearizedPosition));
    } while (
        succeeded(incSlicePosition(currSlicePosition, sliceShape, offsets)));

    assert(static_cast<int64_t>(sliceValues.size()) ==
               sliceVecTy.getNumElements() &&
           "Invalid number of slice elements");
    auto newAttr = DenseElementsAttr::get(sliceVecTy, sliceValues);
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(extractStridedSliceOp,
                                                   newAttr);
    return success();
  }
};

// Pattern to rewrite an ExtractStridedSliceOp(BroadcastOp) to
// BroadcastOp(ExtractStrideSliceOp).
class StridedSliceBroadcast final
    : public OpRewritePattern<ExtractStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractStridedSliceOp op,
                                PatternRewriter &rewriter) const override {
    auto broadcast = op.getVector().getDefiningOp<BroadcastOp>();
    if (!broadcast)
      return failure();
    auto srcVecType =
        llvm::dyn_cast<VectorType>(broadcast.getSource().getType());
    unsigned srcRank = srcVecType ? srcVecType.getRank() : 0;
    auto dstVecType = llvm::cast<VectorType>(op.getType());
    unsigned dstRank = dstVecType.getRank();
    unsigned rankDiff = dstRank - srcRank;
    // Check if the most inner dimensions of the source of the broadcast are the
    // same as the destination of the extract. If this is the case we can just
    // use a broadcast as the original dimensions are untouched.
    bool lowerDimMatch = true;
    for (unsigned i = 0; i < srcRank; i++) {
      if (srcVecType.getDimSize(i) != dstVecType.getDimSize(i + rankDiff)) {
        lowerDimMatch = false;
        break;
      }
    }
    Value source = broadcast.getSource();
    // If the inner dimensions don't match, it means we need to extract from the
    // source of the orignal broadcast and then broadcast the extracted value.
    // We also need to handle degenerated cases where the source is effectively
    // just a single scalar.
    bool isScalarSrc = (srcRank == 0 || srcVecType.getNumElements() == 1);
    if (!lowerDimMatch && !isScalarSrc) {
      source = rewriter.create<ExtractStridedSliceOp>(
          op->getLoc(), source,
          getI64SubArray(op.getOffsets(), /* dropFront=*/rankDiff),
          getI64SubArray(op.getSizes(), /* dropFront=*/rankDiff),
          getI64SubArray(op.getStrides(), /* dropFront=*/rankDiff));
    }
    rewriter.replaceOpWithNewOp<BroadcastOp>(op, op.getType(), source);
    return success();
  }
};

/// Pattern to rewrite an ExtractStridedSliceOp(SplatOp) to SplatOp.
class StridedSliceSplat final : public OpRewritePattern<ExtractStridedSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractStridedSliceOp op,
                                PatternRewriter &rewriter) const override {
    auto splat = op.getVector().getDefiningOp<SplatOp>();
    if (!splat)
      return failure();
    rewriter.replaceOpWithNewOp<SplatOp>(op, op.getType(), splat.getInput());
    return success();
  }
};

} // namespace

void ExtractStridedSliceOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  // Pattern to rewrite a ExtractStridedSliceOp(ConstantMaskOp) ->
  // ConstantMaskOp and ExtractStridedSliceOp(ConstantOp) -> ConstantOp.
  results.add<StridedSliceConstantMaskFolder, StridedSliceSplatConstantFolder,
              StridedSliceNonSplatConstantFolder, StridedSliceBroadcast,
              StridedSliceSplat>(context);
}

//===----------------------------------------------------------------------===//
// TransferReadOp
//===----------------------------------------------------------------------===//

/// 1. Builder that sets padding to zero and an empty mask (variant with attrs).
void TransferReadOp::build(OpBuilder &builder, OperationState &result,
                           VectorType vectorType, Value source,
                           ValueRange indices, AffineMapAttr permutationMapAttr,
                           /*optional*/ ArrayAttr inBoundsAttr) {
  Type elemType = llvm::cast<ShapedType>(source.getType()).getElementType();
  Value padding = builder.create<arith::ConstantOp>(
      result.location, elemType, builder.getZeroAttr(elemType));
  build(builder, result, vectorType, source, indices, permutationMapAttr,
        padding, /*mask=*/Value(), inBoundsAttr);
}

/// 2. Builder that sets padding to zero an empty mask (variant without attrs).
void TransferReadOp::build(OpBuilder &builder, OperationState &result,
                           VectorType vectorType, Value source,
                           ValueRange indices, AffineMap permutationMap,
                           std::optional<ArrayRef<bool>> inBounds) {
  auto permutationMapAttr = AffineMapAttr::get(permutationMap);
  auto inBoundsAttr = (inBounds && !inBounds.value().empty())
                          ? builder.getBoolArrayAttr(inBounds.value())
                          : ArrayAttr();
  build(builder, result, vectorType, source, indices, permutationMapAttr,
        inBoundsAttr);
}

/// 3. Builder that sets permutation map to 'getMinorIdentityMap'.
void TransferReadOp::build(OpBuilder &builder, OperationState &result,
                           VectorType vectorType, Value source,
                           ValueRange indices, Value padding,
                           std::optional<ArrayRef<bool>> inBounds) {
  AffineMap permutationMap = getTransferMinorIdentityMap(
      llvm::cast<ShapedType>(source.getType()), vectorType);
  auto permutationMapAttr = AffineMapAttr::get(permutationMap);
  auto inBoundsAttr = (inBounds && !inBounds.value().empty())
                          ? builder.getBoolArrayAttr(inBounds.value())
                          : ArrayAttr();
  build(builder, result, vectorType, source, indices, permutationMapAttr,
        padding,
        /*mask=*/Value(), inBoundsAttr);
}

/// 4. Builder that sets padding to zero and permutation map to
/// 'getMinorIdentityMap'.
void TransferReadOp::build(OpBuilder &builder, OperationState &result,
                           VectorType vectorType, Value source,
                           ValueRange indices,
                           std::optional<ArrayRef<bool>> inBounds) {
  Type elemType = llvm::cast<ShapedType>(source.getType()).getElementType();
  Value padding = builder.create<arith::ConstantOp>(
      result.location, elemType, builder.getZeroAttr(elemType));
  build(builder, result, vectorType, source, indices, padding, inBounds);
}

template <typename EmitFun>
static LogicalResult verifyPermutationMap(AffineMap permutationMap,
                                          EmitFun emitOpError) {
  SmallVector<bool, 8> seen(permutationMap.getNumInputs(), false);
  for (auto expr : permutationMap.getResults()) {
    auto dim = expr.dyn_cast<AffineDimExpr>();
    auto zero = expr.dyn_cast<AffineConstantExpr>();
    if (zero) {
      if (zero.getValue() != 0) {
        return emitOpError(
            "requires a projected permutation_map (at most one dim or the zero "
            "constant can appear in each result)");
      }
      continue;
    }
    if (!dim) {
      return emitOpError("requires a projected permutation_map (at most one "
                         "dim or the zero constant can appear in each result)");
    }
    if (seen[dim.getPosition()]) {
      return emitOpError(
          "requires a permutation_map that is a permutation (found one dim "
          "used more than once)");
    }
    seen[dim.getPosition()] = true;
  }
  return success();
}

static LogicalResult
verifyTransferOp(VectorTransferOpInterface op, ShapedType shapedType,
                 VectorType vectorType, VectorType maskType,
                 VectorType inferredMaskType, AffineMap permutationMap,
                 ArrayAttr inBounds) {
  if (op->hasAttr("masked")) {
    return op->emitOpError("masked attribute has been removed. "
                           "Use in_bounds instead.");
  }

  if (!llvm::isa<MemRefType, RankedTensorType>(shapedType))
    return op->emitOpError(
        "requires source to be a memref or ranked tensor type");

  auto elementType = shapedType.getElementType();
  DataLayout dataLayout = DataLayout::closest(op);
  if (auto vectorElementType = llvm::dyn_cast<VectorType>(elementType)) {
    // Memref or tensor has vector element type.
    unsigned sourceVecSize =
        dataLayout.getTypeSizeInBits(vectorElementType.getElementType()) *
        vectorElementType.getShape().back();
    unsigned resultVecSize =
        dataLayout.getTypeSizeInBits(vectorType.getElementType()) *
        vectorType.getShape().back();
    if (resultVecSize % sourceVecSize != 0)
      return op->emitOpError(
          "requires the bitwidth of the minor 1-D vector to be an integral "
          "multiple of the bitwidth of the minor 1-D vector of the source");

    unsigned sourceVecEltRank = vectorElementType.getRank();
    unsigned resultVecRank = vectorType.getRank();
    if (sourceVecEltRank > resultVecRank)
      return op->emitOpError(
          "requires source vector element and vector result ranks to match.");
    unsigned rankOffset = resultVecRank - sourceVecEltRank;
    // Check that permutation map results match 'rankOffset' of vector type.
    if (permutationMap.getNumResults() != rankOffset)
      return op->emitOpError("requires a permutation_map with result dims of "
                             "the same rank as the vector type");

    if (maskType)
      return op->emitOpError("does not support masks with vector element type");
  } else {
    // Memref or tensor has scalar element type.
    unsigned minorSize =
        vectorType.getRank() == 0 ? 1 : vectorType.getShape().back();
    unsigned resultVecSize =
        dataLayout.getTypeSizeInBits(vectorType.getElementType()) * minorSize;
    if (resultVecSize % dataLayout.getTypeSizeInBits(elementType) != 0)
      return op->emitOpError(
          "requires the bitwidth of the minor 1-D vector to be an integral "
          "multiple of the bitwidth of the source element type");

    // Check that permutation map results match rank of vector type.
    if (permutationMap.getNumResults() != vectorType.getRank())
      return op->emitOpError("requires a permutation_map with result dims of "
                             "the same rank as the vector type");
  }

  if (permutationMap.getNumSymbols() != 0)
    return op->emitOpError("requires permutation_map without symbols");

  if (permutationMap.getNumInputs() != shapedType.getRank())
    return op->emitOpError("requires a permutation_map with input dims of the "
                           "same rank as the source type");

  if (maskType && maskType != inferredMaskType)
    return op->emitOpError("inferred mask type (")
           << inferredMaskType << ") and mask operand type (" << maskType
           << ") don't match";

  if (inBounds) {
    if (permutationMap.getNumResults() != static_cast<int64_t>(inBounds.size()))
      return op->emitOpError("expects the optional in_bounds attr of same rank "
                             "as permutation_map results: ")
             << AffineMapAttr::get(permutationMap)
             << " vs inBounds of size: " << inBounds.size();
    for (unsigned int i = 0; i < permutationMap.getNumResults(); ++i)
      if (permutationMap.getResult(i).isa<AffineConstantExpr>() &&
          !llvm::cast<BoolAttr>(inBounds.getValue()[i]).getValue())
        return op->emitOpError("requires broadcast dimensions to be in-bounds");
  }

  return success();
}

static void printTransferAttrs(OpAsmPrinter &p, VectorTransferOpInterface op) {
  SmallVector<StringRef, 3> elidedAttrs;
  elidedAttrs.push_back(TransferReadOp::getOperandSegmentSizeAttr());
  if (op.getPermutationMap().isMinorIdentity())
    elidedAttrs.push_back(op.getPermutationMapAttrStrName());
  // Elide in_bounds attribute if all dims are out-of-bounds.
  if (llvm::none_of(op.getInBoundsValues(), [](bool b) { return b; }))
    elidedAttrs.push_back(op.getInBoundsAttrStrName());
  p.printOptionalAttrDict(op->getAttrs(), elidedAttrs);
}

void TransferReadOp::print(OpAsmPrinter &p) {
  p << " " << getSource() << "[" << getIndices() << "], " << getPadding();
  if (getMask())
    p << ", " << getMask();
  printTransferAttrs(p, *this);
  p << " : " << getShapedType() << ", " << getVectorType();
}

/// Infers the mask type for a transfer op given its vector type and
/// permutation map. The mask in a transfer op operation applies to the
/// tensor/buffer part of it and its type should match the vector shape
/// *before* any permutation or broadcasting.
static VectorType inferTransferOpMaskType(VectorType vecType,
                                          AffineMap permMap) {
  auto i1Type = IntegerType::get(permMap.getContext(), 1);
  AffineMap invPermMap = inversePermutation(compressUnusedDims(permMap));
  assert(invPermMap && "Inversed permutation map couldn't be computed");
  SmallVector<int64_t, 8> maskShape = invPermMap.compose(vecType.getShape());

  SmallVector<bool> scalableDims =
      applyPermutationMap(invPermMap, vecType.getScalableDims());

  return VectorType::get(maskShape, i1Type, scalableDims);
}

ParseResult TransferReadOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  SMLoc typesLoc;
  OpAsmParser::UnresolvedOperand sourceInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 8> indexInfo;
  OpAsmParser::UnresolvedOperand paddingInfo;
  SmallVector<Type, 2> types;
  OpAsmParser::UnresolvedOperand maskInfo;
  // Parsing with support for paddingValue.
  if (parser.parseOperand(sourceInfo) ||
      parser.parseOperandList(indexInfo, OpAsmParser::Delimiter::Square) ||
      parser.parseComma() || parser.parseOperand(paddingInfo))
    return failure();
  ParseResult hasMask = parser.parseOptionalComma();
  if (hasMask.succeeded()) {
    if (parser.parseOperand(maskInfo))
      return failure();
  }
  if (parser.parseOptionalAttrDict(result.attributes) ||
      parser.getCurrentLocation(&typesLoc) || parser.parseColonTypeList(types))
    return failure();
  if (types.size() != 2)
    return parser.emitError(typesLoc, "requires two types");
  auto indexType = builder.getIndexType();
  auto shapedType = llvm::dyn_cast<ShapedType>(types[0]);
  if (!shapedType || !llvm::isa<MemRefType, RankedTensorType>(shapedType))
    return parser.emitError(typesLoc, "requires memref or ranked tensor type");
  VectorType vectorType = llvm::dyn_cast<VectorType>(types[1]);
  if (!vectorType)
    return parser.emitError(typesLoc, "requires vector type");
  auto permMapAttrName = TransferReadOp::getPermutationMapAttrStrName();
  Attribute permMapAttr = result.attributes.get(permMapAttrName);
  AffineMap permMap;
  if (!permMapAttr) {
    permMap = getTransferMinorIdentityMap(shapedType, vectorType);
    result.attributes.set(permMapAttrName, AffineMapAttr::get(permMap));
  } else {
    permMap = llvm::cast<AffineMapAttr>(permMapAttr).getValue();
  }
  if (parser.resolveOperand(sourceInfo, shapedType, result.operands) ||
      parser.resolveOperands(indexInfo, indexType, result.operands) ||
      parser.resolveOperand(paddingInfo, shapedType.getElementType(),
                            result.operands))
    return failure();
  if (hasMask.succeeded()) {
    if (llvm::dyn_cast<VectorType>(shapedType.getElementType()))
      return parser.emitError(
          maskInfo.location, "does not support masks with vector element type");
    // Instead of adding the mask type as an op type, compute it based on the
    // vector type and the permutation map (to keep the type signature small).
    auto maskType = inferTransferOpMaskType(vectorType, permMap);
    if (parser.resolveOperand(maskInfo, maskType, result.operands))
      return failure();
  }
  result.addAttribute(TransferReadOp::getOperandSegmentSizeAttr(),
                      builder.getDenseI32ArrayAttr(
                          {1, static_cast<int32_t>(indexInfo.size()), 1,
                           static_cast<int32_t>(hasMask.succeeded())}));
  return parser.addTypeToList(vectorType, result.types);
}

LogicalResult TransferReadOp::verify() {
  // Consistency of elemental types in source and vector.
  ShapedType shapedType = getShapedType();
  VectorType vectorType = getVectorType();
  VectorType maskType = getMaskType();
  auto paddingType = getPadding().getType();
  auto permutationMap = getPermutationMap();
  VectorType inferredMaskType =
      maskType ? inferTransferOpMaskType(vectorType, permutationMap)
               : VectorType();
  auto sourceElementType = shapedType.getElementType();

  if (static_cast<int64_t>(getIndices().size()) != shapedType.getRank())
    return emitOpError("requires ") << shapedType.getRank() << " indices";

  if (failed(verifyTransferOp(cast<VectorTransferOpInterface>(getOperation()),
                              shapedType, vectorType, maskType,
                              inferredMaskType, permutationMap,
                              getInBounds() ? *getInBounds() : ArrayAttr())))
    return failure();

  if (auto sourceVectorElementType =
          llvm::dyn_cast<VectorType>(sourceElementType)) {
    // Source has vector element type.
    // Check that 'sourceVectorElementType' and 'paddingType' types match.
    if (sourceVectorElementType != paddingType)
      return emitOpError(
          "requires source element type and padding type to match.");

  } else {
    // Check that 'paddingType' is valid to store in a vector type.
    if (!VectorType::isValidElementType(paddingType))
      return emitOpError("requires valid padding vector elemental type");

    // Check that padding type and vector element types match.
    if (paddingType != sourceElementType)
      return emitOpError(
          "requires formal padding and source of the same elemental type");
  }

  return verifyPermutationMap(permutationMap,
                              [&](Twine t) { return emitOpError(t); });
}

// MaskableOpInterface methods.

/// Returns the mask type expected by this operation. Mostly used for
/// verification purposes. It requires the operation to be vectorized."
Type TransferReadOp::getExpectedMaskType() {
  return inferTransferOpMaskType(getVectorType(), getPermutationMap());
}

template <typename TransferOp>
static bool isInBounds(TransferOp op, int64_t resultIdx, int64_t indicesIdx) {
  // TODO: support more aggressive createOrFold on:
  // `op.indices()[indicesIdx] + vectorType < dim(op.source(), indicesIdx)`
  if (op.getShapedType().isDynamicDim(indicesIdx))
    return false;
  Value index = op.getIndices()[indicesIdx];
  std::optional<int64_t> cstOp = getConstantIntValue(index);
  if (!cstOp.has_value())
    return false;

  int64_t sourceSize = op.getShapedType().getDimSize(indicesIdx);
  int64_t vectorSize = op.getVectorType().getDimSize(resultIdx);

  return cstOp.value() + vectorSize <= sourceSize;
}

template <typename TransferOp>
static LogicalResult foldTransferInBoundsAttribute(TransferOp op) {
  // TODO: support 0-d corner case.
  // TODO: Be less conservative.
  if (op.getTransferRank() == 0)
    return failure();
  AffineMap permutationMap = op.getPermutationMap();
  bool changed = false;
  SmallVector<bool, 4> newInBounds;
  newInBounds.reserve(op.getTransferRank());
  for (unsigned i = 0; i < op.getTransferRank(); ++i) {
    // Already marked as in-bounds, nothing to see here.
    if (op.isDimInBounds(i)) {
      newInBounds.push_back(true);
      continue;
    }
    // Currently out-of-bounds, check whether we can statically determine it is
    // inBounds.
    auto dimExpr = permutationMap.getResult(i).dyn_cast<AffineDimExpr>();
    assert(dimExpr && "Broadcast dims must be in-bounds");
    auto inBounds =
        isInBounds(op, /*resultIdx=*/i, /*indicesIdx=*/dimExpr.getPosition());
    newInBounds.push_back(inBounds);
    // We commit the pattern if it is "more inbounds".
    changed |= inBounds;
  }
  if (!changed)
    return failure();
  // OpBuilder is only used as a helper to build an I64ArrayAttr.
  OpBuilder b(op.getContext());
  op->setAttr(TransferOp::getInBoundsAttrStrName(),
              b.getBoolArrayAttr(newInBounds));
  return success();
}

///  ```
///  %w0 = vector.transfer_write %v0, %arg0[%c1, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
///  %0 = vector.transfer_read %w0[%c1, %c0], %cf0 {in_bounds = [true, true]}
///    : tensor<4x4xf32>, vector<1x4xf32>
///  ```
///  -> Folds into
///  ```
///  %v0
///  ```
static Value foldRAW(TransferReadOp readOp) {
  if (!llvm::isa<RankedTensorType>(readOp.getShapedType()))
    return {};
  auto defWrite = readOp.getSource().getDefiningOp<vector::TransferWriteOp>();
  while (defWrite) {
    if (checkSameValueRAW(defWrite, readOp))
      return defWrite.getVector();
    if (!isDisjointTransferIndices(
            cast<VectorTransferOpInterface>(defWrite.getOperation()),
            cast<VectorTransferOpInterface>(readOp.getOperation())))
      break;
    defWrite = defWrite.getSource().getDefiningOp<vector::TransferWriteOp>();
  }
  return {};
}

OpFoldResult TransferReadOp::fold(FoldAdaptor) {
  if (Value vec = foldRAW(*this))
    return vec;
  /// transfer_read(memrefcast) -> transfer_read
  if (succeeded(foldTransferInBoundsAttribute(*this)))
    return getResult();
  if (succeeded(memref::foldMemRefCast(*this)))
    return getResult();
  if (succeeded(tensor::foldTensorCast(*this)))
    return getResult();
  return OpFoldResult();
}

std::optional<SmallVector<int64_t, 4>> TransferReadOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getVectorType().getShape());
}

void TransferReadOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  if (llvm::isa<MemRefType>(getShapedType()))
    effects.emplace_back(MemoryEffects::Read::get(), getSource(),
                         SideEffects::DefaultResource::get());
}

namespace {
/// Store to load forwarding for transfer operations with permuation maps.
/// Even if the permutation maps are different we can still propagate the store
/// into the load if the size of the dimensions read and written match. Then we
/// can replace the transfer_read + transfer_write by vector.broadcast and
/// vector.transpose.
/// Example:
/// ```
/// %w0 = vector.transfer_write %v0, %arg0[%c0, %c0, %c0]
///  {in_bounds = [true, true],
///   permutation_map = affine_map<(d0, d1, d2) -> (d2, d1)>} :
///   vector<4x1xf32>, tensor<4x4x4xf32>
///  %r = vector.transfer_read %w0[%c0, %c0, %c0], %cf0
///   {in_bounds = [true, true, true, true],
///   permutation_map = affine_map<(d0, d1, d2) -> (d1, 0, d2, 0)>} :
///   tensor<4x4x4xf32>, vector<1x100x4x5xf32>
/// ```
/// To:
/// ```
/// %0 = vector.broadcast %arg1 : vector<4x1xf32> to vector<100x5x4x1xf32>
/// %r = vector.transpose %0, [3, 0, 2, 1] :
///   vector<100x5x4x1xf32> to vector<1x100x4x5xf32>
/// ```
struct TransferReadAfterWriteToBroadcast
    : public OpRewritePattern<TransferReadOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(TransferReadOp readOp,
                                PatternRewriter &rewriter) const override {
    if (readOp.hasOutOfBoundsDim() ||
        !llvm::isa<RankedTensorType>(readOp.getShapedType()))
      return failure();
    auto defWrite = readOp.getSource().getDefiningOp<vector::TransferWriteOp>();
    if (!defWrite)
      return failure();

    SmallVector<int64_t> readDims = readOp.getTransferChunkAccessed();
    Value vec;
    if (readOp.getIndices() == defWrite.getIndices() &&
        readOp.getMask() == defWrite.getMask()) {
      SmallVector<int64_t> writeDims = defWrite.getTransferChunkAccessed();
      // TODO: If the writeDim is a superset of the read dims we could do an
      // extract_strided_slice.
      if (writeDims == readDims)
        vec = defWrite.getVector();
    }
    // TODO: loop through the chain of transfer_write if we can prove that they
    // don't overlap with the transfer_read. This requires improving
    // `isDisjointTransferIndices` helper.
    if (!vec)
      return failure();
    SmallVector<unsigned> permutation;
    AffineMap readMap = compressUnusedDims(readOp.getPermutationMap());
    AffineMap writeMap = compressUnusedDims(defWrite.getPermutationMap());
    AffineMap map = readMap.compose(writeMap);
    if (map.getNumResults() == 0)
      return failure();
    // Calculate the permuation to apply to go from the vector stored to the
    // vector read.
    if (!map.isPermutationOfMinorIdentityWithBroadcasting(permutation))
      return failure();

    Location loc = readOp.getLoc();
    // Calculate the broadcast shape by applying the reverse permuation to the
    // final shape we want.
    ArrayRef<int64_t> destShape = readOp.getVectorType().getShape();
    SmallVector<int64_t> broadcastShape(destShape.size());
    for (const auto &pos : llvm::enumerate(permutation))
      broadcastShape[pos.value()] = destShape[pos.index()];
    VectorType broadcastedType = VectorType::get(
        broadcastShape, defWrite.getVectorType().getElementType());
    vec = rewriter.create<vector::BroadcastOp>(loc, broadcastedType, vec);
    SmallVector<int64_t> transposePerm(permutation.begin(), permutation.end());
    rewriter.replaceOpWithNewOp<vector::TransposeOp>(readOp, vec,
                                                     transposePerm);
    return success();
  }
};
} // namespace

void TransferReadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  results.add<TransferReadAfterWriteToBroadcast>(context);
}

//===----------------------------------------------------------------------===//
// TransferWriteOp
//===----------------------------------------------------------------------===//

/// 1. Builder with type inference.
void TransferWriteOp::build(OpBuilder &builder, OperationState &result,
                            Value vector, Value dest, ValueRange indices,
                            AffineMapAttr permutationMapAttr,
                            /*optional*/ Value mask,
                            /*optional*/ ArrayAttr inBoundsAttr) {
  Type resultType = llvm::dyn_cast<RankedTensorType>(dest.getType());
  build(builder, result, resultType, vector, dest, indices, permutationMapAttr,
        mask, inBoundsAttr);
}

/// 2. Builder with type inference that sets an empty mask (variant with attrs).
void TransferWriteOp::build(OpBuilder &builder, OperationState &result,
                            Value vector, Value dest, ValueRange indices,
                            AffineMapAttr permutationMapAttr,
                            /*optional*/ ArrayAttr inBoundsAttr) {
  build(builder, result, vector, dest, indices, permutationMapAttr,
        /*mask=*/Value(), inBoundsAttr);
}

/// 3. Builder with type inference that sets an empty mask (variant without
/// attrs)
void TransferWriteOp::build(OpBuilder &builder, OperationState &result,
                            Value vector, Value dest, ValueRange indices,
                            AffineMap permutationMap,
                            std::optional<ArrayRef<bool>> inBounds) {
  auto permutationMapAttr = AffineMapAttr::get(permutationMap);
  auto inBoundsAttr = (inBounds && !inBounds.value().empty())
                          ? builder.getBoolArrayAttr(inBounds.value())
                          : ArrayAttr();
  build(builder, result, vector, dest, indices, permutationMapAttr,
        /*mask=*/Value(), inBoundsAttr);
}

/// 4. Builder with type inference that sets an empty mask and sets permutation
///    map to 'getMinorIdentityMap'.
void TransferWriteOp::build(OpBuilder &builder, OperationState &result,
                            Value vector, Value dest, ValueRange indices,
                            std::optional<ArrayRef<bool>> inBounds) {
  auto vectorType = llvm::cast<VectorType>(vector.getType());
  AffineMap permutationMap = getTransferMinorIdentityMap(
      llvm::cast<ShapedType>(dest.getType()), vectorType);
  build(builder, result, vector, dest, indices, permutationMap, inBounds);
}

ParseResult TransferWriteOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  auto &builder = parser.getBuilder();
  SMLoc typesLoc;
  OpAsmParser::UnresolvedOperand vectorInfo, sourceInfo;
  SmallVector<OpAsmParser::UnresolvedOperand, 8> indexInfo;
  SmallVector<Type, 2> types;
  OpAsmParser::UnresolvedOperand maskInfo;
  if (parser.parseOperand(vectorInfo) || parser.parseComma() ||
      parser.parseOperand(sourceInfo) ||
      parser.parseOperandList(indexInfo, OpAsmParser::Delimiter::Square))
    return failure();
  ParseResult hasMask = parser.parseOptionalComma();
  if (hasMask.succeeded() && parser.parseOperand(maskInfo))
    return failure();
  if (parser.parseOptionalAttrDict(result.attributes) ||
      parser.getCurrentLocation(&typesLoc) || parser.parseColonTypeList(types))
    return failure();
  if (types.size() != 2)
    return parser.emitError(typesLoc, "requires two types");
  auto indexType = builder.getIndexType();
  VectorType vectorType = llvm::dyn_cast<VectorType>(types[0]);
  if (!vectorType)
    return parser.emitError(typesLoc, "requires vector type");
  ShapedType shapedType = llvm::dyn_cast<ShapedType>(types[1]);
  if (!shapedType || !llvm::isa<MemRefType, RankedTensorType>(shapedType))
    return parser.emitError(typesLoc, "requires memref or ranked tensor type");
  auto permMapAttrName = TransferWriteOp::getPermutationMapAttrStrName();
  auto permMapAttr = result.attributes.get(permMapAttrName);
  AffineMap permMap;
  if (!permMapAttr) {
    permMap = getTransferMinorIdentityMap(shapedType, vectorType);
    result.attributes.set(permMapAttrName, AffineMapAttr::get(permMap));
  } else {
    permMap = llvm::cast<AffineMapAttr>(permMapAttr).getValue();
  }
  if (parser.resolveOperand(vectorInfo, vectorType, result.operands) ||
      parser.resolveOperand(sourceInfo, shapedType, result.operands) ||
      parser.resolveOperands(indexInfo, indexType, result.operands))
    return failure();
  if (hasMask.succeeded()) {
    if (llvm::dyn_cast<VectorType>(shapedType.getElementType()))
      return parser.emitError(
          maskInfo.location, "does not support masks with vector element type");
    auto maskType = inferTransferOpMaskType(vectorType, permMap);
    if (parser.resolveOperand(maskInfo, maskType, result.operands))
      return failure();
  }
  result.addAttribute(TransferWriteOp::getOperandSegmentSizeAttr(),
                      builder.getDenseI32ArrayAttr(
                          {1, 1, static_cast<int32_t>(indexInfo.size()),
                           static_cast<int32_t>(hasMask.succeeded())}));
  return failure(llvm::isa<RankedTensorType>(shapedType) &&
                 parser.addTypeToList(shapedType, result.types));
}

void TransferWriteOp::print(OpAsmPrinter &p) {
  p << " " << getVector() << ", " << getSource() << "[" << getIndices() << "]";
  if (getMask())
    p << ", " << getMask();
  printTransferAttrs(p, *this);
  p << " : " << getVectorType() << ", " << getShapedType();
}

LogicalResult TransferWriteOp::verify() {
  // Consistency of elemental types in shape and vector.
  ShapedType shapedType = getShapedType();
  VectorType vectorType = getVectorType();
  VectorType maskType = getMaskType();
  auto permutationMap = getPermutationMap();
  VectorType inferredMaskType =
      maskType ? inferTransferOpMaskType(vectorType, permutationMap)
               : VectorType();

  if (llvm::size(getIndices()) != shapedType.getRank())
    return emitOpError("requires ") << shapedType.getRank() << " indices";

  // We do not allow broadcast dimensions on TransferWriteOps for the moment,
  // as the semantics is unclear. This can be revisited later if necessary.
  if (hasBroadcastDim())
    return emitOpError("should not have broadcast dimensions");

  if (failed(verifyTransferOp(cast<VectorTransferOpInterface>(getOperation()),
                              shapedType, vectorType, maskType,
                              inferredMaskType, permutationMap,
                              getInBounds() ? *getInBounds() : ArrayAttr())))
    return failure();

  return verifyPermutationMap(permutationMap,
                              [&](Twine t) { return emitOpError(t); });
}

// MaskableOpInterface methods.

/// Returns the mask type expected by this operation. Mostly used for
/// verification purposes.
Type TransferWriteOp::getExpectedMaskType() {
  return inferTransferOpMaskType(getVectorType(), getPermutationMap());
}

/// Fold:
/// ```
///    %t1 = ...
///    %v = vector.transfer_read %t0[%c0...], {in_bounds = [true...]} :
///      tensor<static_sizesxf32>, vector<static_sizesxf32>
///    %t2 = vector.transfer_write %v, %t1[%c0...] {in_bounds = [true...]} :
///      vector<static_sizesxf32>, tensor<static_sizesxf32>
/// ```
///
/// into:
///
/// ```
///    %t0
/// ```
///
/// The producer of t1 may or may not be DCE'd depending on whether it is a
/// block argument or has side effects.
static LogicalResult foldReadInitWrite(TransferWriteOp write,
                                       ArrayRef<Attribute>,
                                       SmallVectorImpl<OpFoldResult> &results) {
  // TODO: support 0-d corner case.
  if (write.getTransferRank() == 0)
    return failure();
  auto rankedTensorType =
      llvm::dyn_cast<RankedTensorType>(write.getSource().getType());
  // If not operating on tensors, bail.
  if (!rankedTensorType)
    return failure();
  // If no read, bail.
  auto read = write.getVector().getDefiningOp<vector::TransferReadOp>();
  if (!read)
    return failure();
  // TODO: support 0-d corner case.
  if (read.getTransferRank() == 0)
    return failure();
  // For now, only accept minor identity. Future: composition is minor identity.
  if (!read.getPermutationMap().isMinorIdentity() ||
      !write.getPermutationMap().isMinorIdentity())
    return failure();
  // Bail on mismatching ranks.
  if (read.getTransferRank() != write.getTransferRank())
    return failure();
  // Bail on potential out-of-bounds accesses.
  if (read.hasOutOfBoundsDim() || write.hasOutOfBoundsDim())
    return failure();
  // Tensor types must be the same.
  if (read.getSource().getType() != rankedTensorType)
    return failure();
  // Vector types must be the same.
  if (read.getVectorType() != write.getVectorType())
    return failure();
  // Vector and Tensor shapes must match.
  if (read.getVectorType().getShape() != rankedTensorType.getShape())
    return failure();
  // If any index is nonzero.
  auto isNotConstantZero = [](Value v) {
    auto cstOp = getConstantIntValue(v);
    return !cstOp.has_value() || cstOp.value() != 0;
  };
  if (llvm::any_of(read.getIndices(), isNotConstantZero) ||
      llvm::any_of(write.getIndices(), isNotConstantZero))
    return failure();
  // Success.
  results.push_back(read.getSource());
  return success();
}

static bool checkSameValueWAR(vector::TransferReadOp read,
                              vector::TransferWriteOp write) {
  return read.getSource() == write.getSource() &&
         read.getIndices() == write.getIndices() &&
         read.getPermutationMap() == write.getPermutationMap() &&
         read.getVectorType() == write.getVectorType() && !read.getMask() &&
         !write.getMask();
}
/// Fold transfer_write write after read:
/// ```
///    %t0 = ...
///    %v = vector.transfer_read %t0[%c0...] :
///      tensor<static_sizesxf32>, vector<static_sizesxf32>
///    %t1 = vector.transfer_write %v, %t0[%c0...] :
///      vector<static_sizesxf32>, tensor<static_sizesxf32>
/// ```
///
/// into:
///
/// ```
///    %t0
/// ```
static LogicalResult foldWAR(TransferWriteOp write,
                             SmallVectorImpl<OpFoldResult> &results) {
  if (!llvm::isa<RankedTensorType>(write.getSource().getType()))
    return failure();
  auto read = write.getVector().getDefiningOp<vector::TransferReadOp>();
  if (!read)
    return failure();

  if (!checkSameValueWAR(read, write))
    return failure();
  results.push_back(read.getSource());
  return success();
}

LogicalResult TransferWriteOp::fold(FoldAdaptor adaptor,
                                    SmallVectorImpl<OpFoldResult> &results) {
  if (succeeded(foldReadInitWrite(*this, adaptor.getOperands(), results)))
    return success();
  if (succeeded(foldWAR(*this, results)))
    return success();
  if (succeeded(foldTransferInBoundsAttribute(*this)))
    return success();
  return memref::foldMemRefCast(*this);
}

std::optional<SmallVector<int64_t, 4>> TransferWriteOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getVectorType().getShape());
}

void TransferWriteOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>
        &effects) {
  if (llvm::isa<MemRefType>(getShapedType()))
    effects.emplace_back(MemoryEffects::Write::get(), getSource(),
                         SideEffects::DefaultResource::get());
}

namespace {
/// Remove dead transfer write from the SSA chain so that it an be eliminated by
/// DCE
/// ```
///  %w0 = vector.transfer_write %v0, %arg0[%c1, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
///  %w1 = vector.transfer_write %v0, %w0[%c2, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
///  %w2 = vector.transfer_write %v1, %w1[%c1, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
/// ```
///
/// into:
///
/// ```
///  %w0 = vector.transfer_write %v0, %arg0[%c1, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
///  %w1 = vector.transfer_write %v0, %arg0[%c2, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
///  %w2 = vector.transfer_write %v1, %w1[%c1, %c0] {in_bounds = [true, true]}
///    : vector<1x4xf32>, tensor<4x4xf32>
/// ```
///
/// `%w0 = vector.transfer_write` op will be removed by DCE if it doesn't have
/// any other uses.
class FoldWaw final : public OpRewritePattern<TransferWriteOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(TransferWriteOp writeOp,
                                PatternRewriter &rewriter) const override {
    if (!llvm::isa<RankedTensorType>(writeOp.getShapedType()))
      return failure();
    vector::TransferWriteOp writeToModify = writeOp;

    auto defWrite =
        writeOp.getSource().getDefiningOp<vector::TransferWriteOp>();
    while (defWrite) {
      if (checkSameValueWAW(writeOp, defWrite)) {
        rewriter.updateRootInPlace(writeToModify, [&]() {
          writeToModify.getSourceMutable().assign(defWrite.getSource());
        });
        return success();
      }
      if (!isDisjointTransferIndices(
              cast<VectorTransferOpInterface>(defWrite.getOperation()),
              cast<VectorTransferOpInterface>(writeOp.getOperation())))
        break;
      // If the previous write op doesn't have any other use we an safely look
      // at the previous store to see if it can be removed.
      if (!defWrite->hasOneUse())
        break;
      writeToModify = defWrite;
      defWrite = defWrite.getSource().getDefiningOp<vector::TransferWriteOp>();
    }
    return failure();
  }
};

/// Rewrite tensor::ExtractSliceOp(vector::TransferWriteOp) to
/// vector::TransferWriteOp(tensor::ExtractSliceOp) if the full slice is
/// overwritten and inserted into another tensor. After this rewrite, the
/// operations bufferize in-place since all of them work on the same slice.
///
/// For example:
/// ```mlir
///   %0 = vector.transfer_write %vec, %init_tensor[%c0, %c0]
///        : vector<8x16xf32>, tensor<8x16xf32>
///   %1 = tensor.extract_slice %0[0, 0] [%sz0, %sz1] [1, 1]
///        : tensor<8x16xf32> to tensor<?x?xf32>
///   %r = tensor.insert_slice %1 into %iter_arg[%iv0, %iv1] [%sz0, %sz1] [1, 1]
///        : tensor<?x?xf32> into tensor<27x37xf32>
/// ```
/// folds to
/// ```mlir
///   %0 = tensor.extract_slice %iter_arg[%iv0, %iv1] [%sz0, %sz1] [1, 1]
///        : tensor<27x37xf32> to tensor<?x?xf32>
///   %1 = vector.transfer_write %vec, %0[%c0, %c0]
///        : vector<8x16xf32>, tensor<?x?xf32>
///   %r = tensor.insert_slice %1 into %iter_arg[%iv0, %iv1] [%sz0, %sz1] [1, 1]
///        : tensor<?x?xf32> into tensor<27x37xf32>
/// ```
struct SwapExtractSliceOfTransferWrite
    : public OpRewritePattern<tensor::InsertSliceOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::InsertSliceOp insertOp,
                                PatternRewriter &rewriter) const override {
    if (!insertOp.hasUnitStride())
      return failure();
    auto extractOp =
        insertOp.getSource().getDefiningOp<tensor::ExtractSliceOp>();
    if (!extractOp || !extractOp.hasUnitStride() || !extractOp->hasOneUse())
      return failure();
    auto transferOp = extractOp.getSource().getDefiningOp<TransferWriteOp>();
    if (!transferOp || !transferOp->hasOneUse())
      return failure();

    // Fail if vector::TransferWriteOp or tensor::ExtractSliceOp is
    // rank-reducing.
    if (insertOp.getSourceType().getRank() != transferOp.getTransferRank()) {
      return rewriter.notifyMatchFailure(insertOp,
                                         "use-def chain is rank-reducing");
    }

    // Fail if tensor::ExtractSliceOp has non-zero offset.
    if (!extractOp.hasZeroOffset()) {
      return rewriter.notifyMatchFailure(insertOp,
                                         "ExtractSliceOp has non-zero offset");
    }

    // Fail if tensor::TransferWriteOp has non-zero offset.
    if (!llvm::all_of(transferOp.getIndices(), [](Value value) {
          return getConstantIntValue(value) == static_cast<int64_t>(0);
        })) {
      return rewriter.notifyMatchFailure(insertOp,
                                         "TranferWriteOp has non-zero offset");
    }

    // Fail if tensor::ExtractSliceOp and tensor::InsertSliceOp sizes differ.
    if (insertOp.getMixedSizes().size() != extractOp.getMixedSizes().size()) {
      return rewriter.notifyMatchFailure(
          insertOp, "InsertSliceOp and ExtractSliceOp ranks differ");
    }

    for (auto [insertSize, extractSize] :
         llvm::zip_equal(insertOp.getMixedSizes(), extractOp.getMixedSizes())) {
      if (!isEqualConstantIntOrValue(insertSize, extractSize)) {
        return rewriter.notifyMatchFailure(
            insertOp, "InsertSliceOp and ExtractSliceOp sizes differ");
      }
    }

    // Fail if the vector::TransferWriteOp may not overwrite the full tensor.
    assert(transferOp.getVectorType().hasStaticShape() &&
           "expected vector to have a static shape");
    ArrayRef<int64_t> vectorShape = transferOp.getVectorType().getShape();
    SmallVector<int64_t> resultShape = applyPermutationMap(
        transferOp.getPermutationMap(), transferOp.getShapedType().getShape());
    if (transferOp.getMask() || !vectorShape.equals(resultShape)) {
      return rewriter.notifyMatchFailure(
          insertOp, "TransferWriteOp may not write the full tensor.");
    }

    // Swap the tensor::ExtractSliceOp in front of the vector::TransferWriteOp.
    // Set all in_bounds to false and let the folder infer them.
    SmallVector<bool> newInBounds(vectorShape.size(), false);
    auto newExtractOp = rewriter.create<tensor::ExtractSliceOp>(
        extractOp.getLoc(), insertOp.getSourceType(), insertOp.getDest(),
        insertOp.getMixedOffsets(), insertOp.getMixedSizes(),
        insertOp.getMixedStrides());
    auto newTransferWriteOp = rewriter.create<TransferWriteOp>(
        transferOp.getLoc(), transferOp.getVector(), newExtractOp.getResult(),
        transferOp.getIndices(), transferOp.getPermutationMapAttr(),
        rewriter.getBoolArrayAttr(newInBounds));
    rewriter.updateRootInPlace(insertOp, [&]() {
      insertOp.getSourceMutable().assign(newTransferWriteOp.getResult());
    });
    return success();
  }
};

} // namespace

void TransferWriteOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.add<FoldWaw, SwapExtractSliceOfTransferWrite>(context);
}

//===----------------------------------------------------------------------===//
// LoadOp
//===----------------------------------------------------------------------===//

static LogicalResult verifyLoadStoreMemRefLayout(Operation *op,
                                                 MemRefType memRefTy) {
  if (!isLastMemrefDimUnitStride(memRefTy))
    return op->emitOpError("most minor memref dim must have unit stride");
  return success();
}

LogicalResult vector::LoadOp::verify() {
  VectorType resVecTy = getVectorType();
  MemRefType memRefTy = getMemRefType();

  if (failed(verifyLoadStoreMemRefLayout(*this, memRefTy)))
    return failure();

  // Checks for vector memrefs.
  Type memElemTy = memRefTy.getElementType();
  if (auto memVecTy = llvm::dyn_cast<VectorType>(memElemTy)) {
    if (memVecTy != resVecTy)
      return emitOpError("base memref and result vector types should match");
    memElemTy = memVecTy.getElementType();
  }

  if (resVecTy.getElementType() != memElemTy)
    return emitOpError("base and result element types should match");
  if (llvm::size(getIndices()) != memRefTy.getRank())
    return emitOpError("requires ") << memRefTy.getRank() << " indices";
  return success();
}

OpFoldResult LoadOp::fold(FoldAdaptor) {
  if (succeeded(memref::foldMemRefCast(*this)))
    return getResult();
  return OpFoldResult();
}

//===----------------------------------------------------------------------===//
// StoreOp
//===----------------------------------------------------------------------===//

LogicalResult vector::StoreOp::verify() {
  VectorType valueVecTy = getVectorType();
  MemRefType memRefTy = getMemRefType();

  if (failed(verifyLoadStoreMemRefLayout(*this, memRefTy)))
    return failure();

  // Checks for vector memrefs.
  Type memElemTy = memRefTy.getElementType();
  if (auto memVecTy = llvm::dyn_cast<VectorType>(memElemTy)) {
    if (memVecTy != valueVecTy)
      return emitOpError(
          "base memref and valueToStore vector types should match");
    memElemTy = memVecTy.getElementType();
  }

  if (valueVecTy.getElementType() != memElemTy)
    return emitOpError("base and valueToStore element type should match");
  if (llvm::size(getIndices()) != memRefTy.getRank())
    return emitOpError("requires ") << memRefTy.getRank() << " indices";
  return success();
}

LogicalResult StoreOp::fold(FoldAdaptor adaptor,
                            SmallVectorImpl<OpFoldResult> &results) {
  return memref::foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// MaskedLoadOp
//===----------------------------------------------------------------------===//

LogicalResult MaskedLoadOp::verify() {
  VectorType maskVType = getMaskVectorType();
  VectorType passVType = getPassThruVectorType();
  VectorType resVType = getVectorType();
  MemRefType memType = getMemRefType();

  if (resVType.getElementType() != memType.getElementType())
    return emitOpError("base and result element type should match");
  if (llvm::size(getIndices()) != memType.getRank())
    return emitOpError("requires ") << memType.getRank() << " indices";
  if (resVType.getDimSize(0) != maskVType.getDimSize(0))
    return emitOpError("expected result dim to match mask dim");
  if (resVType != passVType)
    return emitOpError("expected pass_thru of same type as result type");
  return success();
}

namespace {
class MaskedLoadFolder final : public OpRewritePattern<MaskedLoadOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(MaskedLoadOp load,
                                PatternRewriter &rewriter) const override {
    switch (getMaskFormat(load.getMask())) {
    case MaskFormat::AllTrue:
      rewriter.replaceOpWithNewOp<vector::LoadOp>(
          load, load.getType(), load.getBase(), load.getIndices());
      return success();
    case MaskFormat::AllFalse:
      rewriter.replaceOp(load, load.getPassThru());
      return success();
    case MaskFormat::Unknown:
      return failure();
    }
    llvm_unreachable("Unexpected 1DMaskFormat on MaskedLoad");
  }
};
} // namespace

void MaskedLoadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.add<MaskedLoadFolder>(context);
}

OpFoldResult MaskedLoadOp::fold(FoldAdaptor) {
  if (succeeded(memref::foldMemRefCast(*this)))
    return getResult();
  return OpFoldResult();
}

//===----------------------------------------------------------------------===//
// MaskedStoreOp
//===----------------------------------------------------------------------===//

LogicalResult MaskedStoreOp::verify() {
  VectorType maskVType = getMaskVectorType();
  VectorType valueVType = getVectorType();
  MemRefType memType = getMemRefType();

  if (valueVType.getElementType() != memType.getElementType())
    return emitOpError("base and valueToStore element type should match");
  if (llvm::size(getIndices()) != memType.getRank())
    return emitOpError("requires ") << memType.getRank() << " indices";
  if (valueVType.getDimSize(0) != maskVType.getDimSize(0))
    return emitOpError("expected valueToStore dim to match mask dim");
  return success();
}

namespace {
class MaskedStoreFolder final : public OpRewritePattern<MaskedStoreOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(MaskedStoreOp store,
                                PatternRewriter &rewriter) const override {
    switch (getMaskFormat(store.getMask())) {
    case MaskFormat::AllTrue:
      rewriter.replaceOpWithNewOp<vector::StoreOp>(
          store, store.getValueToStore(), store.getBase(), store.getIndices());
      return success();
    case MaskFormat::AllFalse:
      rewriter.eraseOp(store);
      return success();
    case MaskFormat::Unknown:
      return failure();
    }
    llvm_unreachable("Unexpected 1DMaskFormat on MaskedStore");
  }
};
} // namespace

void MaskedStoreOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<MaskedStoreFolder>(context);
}

LogicalResult MaskedStoreOp::fold(FoldAdaptor adaptor,
                                  SmallVectorImpl<OpFoldResult> &results) {
  return memref::foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// GatherOp
//===----------------------------------------------------------------------===//

LogicalResult GatherOp::verify() {
  VectorType indVType = getIndexVectorType();
  VectorType maskVType = getMaskVectorType();
  VectorType resVType = getVectorType();
  ShapedType baseType = getBaseType();

  if (!llvm::isa<MemRefType, RankedTensorType>(baseType))
    return emitOpError("requires base to be a memref or ranked tensor type");

  if (resVType.getElementType() != baseType.getElementType())
    return emitOpError("base and result element type should match");
  if (llvm::size(getIndices()) != baseType.getRank())
    return emitOpError("requires ") << baseType.getRank() << " indices";
  if (resVType.getShape() != indVType.getShape())
    return emitOpError("expected result dim to match indices dim");
  if (resVType.getShape() != maskVType.getShape())
    return emitOpError("expected result dim to match mask dim");
  if (resVType != getPassThruVectorType())
    return emitOpError("expected pass_thru of same type as result type");
  return success();
}

// MaskableOpInterface methods.

/// Returns the mask type expected by this operation. Mostly used for
/// verification purposes. It requires the operation to be vectorized."
Type GatherOp::getExpectedMaskType() {
  auto vecType = this->getIndexVectorType();
  return VectorType::get(vecType.getShape(),
                         IntegerType::get(vecType.getContext(), /*width=*/1),
                         vecType.getScalableDims());
}

std::optional<SmallVector<int64_t, 4>> GatherOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getVectorType().getShape());
}

namespace {
class GatherFolder final : public OpRewritePattern<GatherOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(GatherOp gather,
                                PatternRewriter &rewriter) const override {
    switch (getMaskFormat(gather.getMask())) {
    case MaskFormat::AllTrue:
      return failure(); // no unmasked equivalent
    case MaskFormat::AllFalse:
      rewriter.replaceOp(gather, gather.getPassThru());
      return success();
    case MaskFormat::Unknown:
      return failure();
    }
    llvm_unreachable("Unexpected 1DMaskFormat on GatherFolder");
  }
};
} // namespace

void GatherOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                           MLIRContext *context) {
  results.add<GatherFolder>(context);
}

//===----------------------------------------------------------------------===//
// ScatterOp
//===----------------------------------------------------------------------===//

LogicalResult ScatterOp::verify() {
  VectorType indVType = getIndexVectorType();
  VectorType maskVType = getMaskVectorType();
  VectorType valueVType = getVectorType();
  MemRefType memType = getMemRefType();

  if (valueVType.getElementType() != memType.getElementType())
    return emitOpError("base and valueToStore element type should match");
  if (llvm::size(getIndices()) != memType.getRank())
    return emitOpError("requires ") << memType.getRank() << " indices";
  if (valueVType.getDimSize(0) != indVType.getDimSize(0))
    return emitOpError("expected valueToStore dim to match indices dim");
  if (valueVType.getDimSize(0) != maskVType.getDimSize(0))
    return emitOpError("expected valueToStore dim to match mask dim");
  return success();
}

namespace {
class ScatterFolder final : public OpRewritePattern<ScatterOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ScatterOp scatter,
                                PatternRewriter &rewriter) const override {
    switch (getMaskFormat(scatter.getMask())) {
    case MaskFormat::AllTrue:
      return failure(); // no unmasked equivalent
    case MaskFormat::AllFalse:
      rewriter.eraseOp(scatter);
      return success();
    case MaskFormat::Unknown:
      return failure();
    }
    llvm_unreachable("Unexpected 1DMaskFormat on ScatterFolder");
  }
};
} // namespace

void ScatterOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<ScatterFolder>(context);
}

//===----------------------------------------------------------------------===//
// ExpandLoadOp
//===----------------------------------------------------------------------===//

LogicalResult ExpandLoadOp::verify() {
  VectorType maskVType = getMaskVectorType();
  VectorType passVType = getPassThruVectorType();
  VectorType resVType = getVectorType();
  MemRefType memType = getMemRefType();

  if (resVType.getElementType() != memType.getElementType())
    return emitOpError("base and result element type should match");
  if (llvm::size(getIndices()) != memType.getRank())
    return emitOpError("requires ") << memType.getRank() << " indices";
  if (resVType.getDimSize(0) != maskVType.getDimSize(0))
    return emitOpError("expected result dim to match mask dim");
  if (resVType != passVType)
    return emitOpError("expected pass_thru of same type as result type");
  return success();
}

namespace {
class ExpandLoadFolder final : public OpRewritePattern<ExpandLoadOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ExpandLoadOp expand,
                                PatternRewriter &rewriter) const override {
    switch (getMaskFormat(expand.getMask())) {
    case MaskFormat::AllTrue:
      rewriter.replaceOpWithNewOp<vector::LoadOp>(
          expand, expand.getType(), expand.getBase(), expand.getIndices());
      return success();
    case MaskFormat::AllFalse:
      rewriter.replaceOp(expand, expand.getPassThru());
      return success();
    case MaskFormat::Unknown:
      return failure();
    }
    llvm_unreachable("Unexpected 1DMaskFormat on ExpandLoadFolder");
  }
};
} // namespace

void ExpandLoadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.add<ExpandLoadFolder>(context);
}

//===----------------------------------------------------------------------===//
// CompressStoreOp
//===----------------------------------------------------------------------===//

LogicalResult CompressStoreOp::verify() {
  VectorType maskVType = getMaskVectorType();
  VectorType valueVType = getVectorType();
  MemRefType memType = getMemRefType();

  if (valueVType.getElementType() != memType.getElementType())
    return emitOpError("base and valueToStore element type should match");
  if (llvm::size(getIndices()) != memType.getRank())
    return emitOpError("requires ") << memType.getRank() << " indices";
  if (valueVType.getDimSize(0) != maskVType.getDimSize(0))
    return emitOpError("expected valueToStore dim to match mask dim");
  return success();
}

namespace {
class CompressStoreFolder final : public OpRewritePattern<CompressStoreOp> {
public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(CompressStoreOp compress,
                                PatternRewriter &rewriter) const override {
    switch (getMaskFormat(compress.getMask())) {
    case MaskFormat::AllTrue:
      rewriter.replaceOpWithNewOp<vector::StoreOp>(
          compress, compress.getValueToStore(), compress.getBase(),
          compress.getIndices());
      return success();
    case MaskFormat::AllFalse:
      rewriter.eraseOp(compress);
      return success();
    case MaskFormat::Unknown:
      return failure();
    }
    llvm_unreachable("Unexpected 1DMaskFormat on CompressStoreFolder");
  }
};
} // namespace

void CompressStoreOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.add<CompressStoreFolder>(context);
}

//===----------------------------------------------------------------------===//
// ShapeCastOp
//===----------------------------------------------------------------------===//

/// Returns true if each element of 'a' is equal to the product of a contiguous
/// sequence of the elements of 'b'. Returns false otherwise.
static bool isValidShapeCast(ArrayRef<int64_t> a, ArrayRef<int64_t> b) {
  unsigned rankA = a.size();
  unsigned rankB = b.size();
  assert(rankA < rankB);

  auto isOne = [](int64_t v) { return v == 1; };

  // Special-case for n-D to 0-d shape cast. 'b' must be all ones to be shape
  // casted to a 0-d vector.
  if (rankA == 0 && llvm::all_of(b, isOne))
    return true;

  unsigned i = 0;
  unsigned j = 0;
  while (i < rankA && j < rankB) {
    int64_t dimA = a[i];
    int64_t dimB = 1;
    while (dimB < dimA && j < rankB)
      dimB *= b[j++];
    if (dimA != dimB)
      break;
    ++i;

    // Handle the case when trailing dimensions are of size 1.
    // Include them into the contiguous sequence.
    if (i < rankA && llvm::all_of(a.slice(i), isOne))
      i = rankA;
    if (j < rankB && llvm::all_of(b.slice(j), isOne))
      j = rankB;
  }

  return i == rankA && j == rankB;
}

static LogicalResult verifyVectorShapeCast(Operation *op,
                                           VectorType sourceVectorType,
                                           VectorType resultVectorType) {
  // Check that element type is the same.
  if (sourceVectorType.getElementType() != resultVectorType.getElementType())
    return op->emitOpError("source/result vectors must have same element type");
  auto sourceShape = sourceVectorType.getShape();
  auto resultShape = resultVectorType.getShape();

  // Check that product of source dim sizes matches product of result dim sizes.
  int64_t sourceDimProduct = std::accumulate(
      sourceShape.begin(), sourceShape.end(), 1LL, std::multiplies<int64_t>{});
  int64_t resultDimProduct = std::accumulate(
      resultShape.begin(), resultShape.end(), 1LL, std::multiplies<int64_t>{});
  if (sourceDimProduct != resultDimProduct)
    return op->emitOpError("source/result number of elements must match");

  // Check that expanding/contracting rank cases.
  unsigned sourceRank = sourceVectorType.getRank();
  unsigned resultRank = resultVectorType.getRank();
  if (sourceRank < resultRank) {
    if (!isValidShapeCast(sourceShape, resultShape))
      return op->emitOpError("invalid shape cast");
  } else if (sourceRank > resultRank) {
    if (!isValidShapeCast(resultShape, sourceShape))
      return op->emitOpError("invalid shape cast");
  }
  return success();
}

LogicalResult ShapeCastOp::verify() {
  auto sourceVectorType =
      llvm::dyn_cast_or_null<VectorType>(getSource().getType());
  auto resultVectorType =
      llvm::dyn_cast_or_null<VectorType>(getResult().getType());

  // Check if source/result are of vector type.
  if (sourceVectorType && resultVectorType)
    return verifyVectorShapeCast(*this, sourceVectorType, resultVectorType);

  return success();
}

OpFoldResult ShapeCastOp::fold(FoldAdaptor adaptor) {
  // No-op shape cast.
  if (getSource().getType() == getResult().getType())
    return getSource();

  // Canceling shape casts.
  if (auto otherOp = getSource().getDefiningOp<ShapeCastOp>()) {
    if (getResult().getType() == otherOp.getSource().getType())
      return otherOp.getSource();

    // Only allows valid transitive folding.
    VectorType srcType = llvm::cast<VectorType>(otherOp.getSource().getType());
    VectorType resultType = llvm::cast<VectorType>(getResult().getType());
    if (srcType.getRank() < resultType.getRank()) {
      if (!isValidShapeCast(srcType.getShape(), resultType.getShape()))
        return {};
    } else if (srcType.getRank() > resultType.getRank()) {
      if (!isValidShapeCast(resultType.getShape(), srcType.getShape()))
        return {};
    } else {
      return {};
    }

    setOperand(otherOp.getSource());
    return getResult();
  }

  // Cancelling broadcast and shape cast ops.
  if (auto bcastOp = getSource().getDefiningOp<BroadcastOp>()) {
    if (bcastOp.getSourceType() == getType())
      return bcastOp.getSource();
  }

  return {};
}

namespace {
// Pattern to rewrite a ShapeCast(splat ConstantOp) -> ConstantOp.
class ShapeCastConstantFolder final : public OpRewritePattern<ShapeCastOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ShapeCastOp shapeCastOp,
                                PatternRewriter &rewriter) const override {
    auto constantOp =
        shapeCastOp.getSource().getDefiningOp<arith::ConstantOp>();
    if (!constantOp)
      return failure();
    // Only handle splat for now.
    auto dense = llvm::dyn_cast<SplatElementsAttr>(constantOp.getValue());
    if (!dense)
      return failure();
    auto newAttr =
        DenseElementsAttr::get(llvm::cast<VectorType>(shapeCastOp.getType()),
                               dense.getSplatValue<Attribute>());
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(shapeCastOp, newAttr);
    return success();
  }
};

/// Pattern to rewrite a ShapeCast(Broadcast) -> Broadcast.
/// This only applies when the shape of the broadcast source is a suffix of the
/// shape of the result (i.e. when broadcast without reshape is expressive
/// enough to capture the result in a single op).
class ShapeCastBroadcastFolder final : public OpRewritePattern<ShapeCastOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ShapeCastOp shapeCastOp,
                                PatternRewriter &rewriter) const override {
    auto broadcastOp =
        shapeCastOp.getSource().getDefiningOp<vector::BroadcastOp>();
    if (!broadcastOp)
      return failure();

    auto broadcastSourceVectorType =
        llvm::dyn_cast<VectorType>(broadcastOp.getSourceType());
    auto broadcastSourceShape = broadcastSourceVectorType
                                    ? broadcastSourceVectorType.getShape()
                                    : ArrayRef<int64_t>{};
    auto shapeCastTargetShape = shapeCastOp.getResultVectorType().getShape();

    // Bail if `broadcastSourceShape` is not a suffix of the result.
    bool isSuffix = (broadcastSourceShape == shapeCastTargetShape.take_back(
                                                 broadcastSourceShape.size()));
    if (!isSuffix)
      return failure();

    rewriter.replaceOpWithNewOp<vector::BroadcastOp>(
        shapeCastOp, shapeCastOp.getResultVectorType(),
        broadcastOp.getSource());
    return success();
  }
};

} // namespace

void ShapeCastOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                              MLIRContext *context) {
  results.add<ShapeCastConstantFolder, ShapeCastBroadcastFolder>(context);
}

//===----------------------------------------------------------------------===//
// VectorBitCastOp
//===----------------------------------------------------------------------===//

LogicalResult BitCastOp::verify() {
  auto sourceVectorType = getSourceVectorType();
  auto resultVectorType = getResultVectorType();

  for (int64_t i = 0, e = sourceVectorType.getRank() - 1; i < e; i++) {
    if (sourceVectorType.getDimSize(i) != resultVectorType.getDimSize(i))
      return emitOpError("dimension size mismatch at: ") << i;
  }

  DataLayout dataLayout = DataLayout::closest(*this);
  auto sourceElementBits =
      dataLayout.getTypeSizeInBits(sourceVectorType.getElementType());
  auto resultElementBits =
      dataLayout.getTypeSizeInBits(resultVectorType.getElementType());

  if (sourceVectorType.getRank() == 0) {
    if (sourceElementBits != resultElementBits)
      return emitOpError("source/result bitwidth of the 0-D vector element "
                         "types must be equal");
  } else if (sourceElementBits * sourceVectorType.getShape().back() !=
             resultElementBits * resultVectorType.getShape().back()) {
    return emitOpError(
        "source/result bitwidth of the minor 1-D vectors must be equal");
  }

  return success();
}

OpFoldResult BitCastOp::fold(FoldAdaptor adaptor) {
  // Nop cast.
  if (getSource().getType() == getResult().getType())
    return getSource();

  // Canceling bitcasts.
  if (auto otherOp = getSource().getDefiningOp<BitCastOp>()) {
    if (getResult().getType() == otherOp.getSource().getType())
      return otherOp.getSource();

    setOperand(otherOp.getSource());
    return getResult();
  }

  Attribute sourceConstant = adaptor.getSource();
  if (!sourceConstant)
    return {};

  Type srcElemType = getSourceVectorType().getElementType();
  Type dstElemType = getResultVectorType().getElementType();

  if (auto floatPack = llvm::dyn_cast<DenseFPElementsAttr>(sourceConstant)) {
    if (floatPack.isSplat()) {
      auto splat = floatPack.getSplatValue<FloatAttr>();

      // Casting fp16 into fp32.
      if (srcElemType.isF16() && dstElemType.isF32()) {
        uint32_t bits = static_cast<uint32_t>(
            splat.getValue().bitcastToAPInt().getZExtValue());
        // Duplicate the 16-bit pattern.
        bits = (bits << 16) | (bits & 0xffff);
        APInt intBits(32, bits);
        APFloat floatBits(llvm::APFloat::IEEEsingle(), intBits);
        return DenseElementsAttr::get(getResultVectorType(), floatBits);
      }
    }
  }

  if (auto intPack = llvm::dyn_cast<DenseIntElementsAttr>(sourceConstant)) {
    if (intPack.isSplat()) {
      auto splat = intPack.getSplatValue<IntegerAttr>();

      if (llvm::isa<IntegerType>(dstElemType)) {
        uint64_t srcBitWidth = srcElemType.getIntOrFloatBitWidth();
        uint64_t dstBitWidth = dstElemType.getIntOrFloatBitWidth();

        // Casting to a larger integer bit width.
        if (dstBitWidth > srcBitWidth && dstBitWidth % srcBitWidth == 0) {
          APInt intBits = splat.getValue().zext(dstBitWidth);

          // Duplicate the lower width element.
          for (uint64_t i = 0; i < dstBitWidth / srcBitWidth - 1; i++)
            intBits = (intBits << srcBitWidth) | intBits;
          return DenseElementsAttr::get(getResultVectorType(), intBits);
        }
      }
    }
  }

  return {};
}

//===----------------------------------------------------------------------===//
// TypeCastOp
//===----------------------------------------------------------------------===//

static SmallVector<int64_t, 8> extractShape(MemRefType memRefType) {
  auto vectorType = llvm::dyn_cast<VectorType>(memRefType.getElementType());
  SmallVector<int64_t, 8> res(memRefType.getShape().begin(),
                              memRefType.getShape().end());
  if (vectorType)
    res.append(vectorType.getShape().begin(), vectorType.getShape().end());
  return res;
}

/// Build the canonical memRefType with a single vector.
/// E.g. memref<4 x 5 x vector<6 x f32>> -> memref<vector<4 x 5 x 6 x f32>>.
void TypeCastOp::build(OpBuilder &builder, OperationState &result,
                       Value source) {
  result.addOperands(source);
  MemRefType memRefType = llvm::cast<MemRefType>(source.getType());
  VectorType vectorType =
      VectorType::get(extractShape(memRefType),
                      getElementTypeOrSelf(getElementTypeOrSelf(memRefType)));
  result.addTypes(MemRefType::get({}, vectorType, MemRefLayoutAttrInterface(),
                                  memRefType.getMemorySpace()));
}

LogicalResult TypeCastOp::verify() {
  MemRefType canonicalType = canonicalizeStridedLayout(getMemRefType());
  if (!canonicalType.getLayout().isIdentity())
    return emitOpError("expects operand to be a memref with identity layout");
  if (!getResultMemRefType().getLayout().isIdentity())
    return emitOpError("expects result to be a memref with identity layout");
  if (getResultMemRefType().getMemorySpace() !=
      getMemRefType().getMemorySpace())
    return emitOpError("expects result in same memory space");

  auto sourceType = getMemRefType();
  auto resultType = getResultMemRefType();
  if (getElementTypeOrSelf(getElementTypeOrSelf(sourceType)) !=
      getElementTypeOrSelf(getElementTypeOrSelf(resultType)))
    return emitOpError(
               "expects result and operand with same underlying scalar type: ")
           << resultType;
  if (extractShape(sourceType) != extractShape(resultType))
    return emitOpError(
               "expects concatenated result and operand shapes to be equal: ")
           << resultType;
  return success();
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

void vector::TransposeOp::build(OpBuilder &builder, OperationState &result,
                                Value vector, ArrayRef<int64_t> transp) {
  VectorType vt = llvm::cast<VectorType>(vector.getType());
  SmallVector<int64_t, 4> transposedShape(vt.getRank());
  SmallVector<bool, 4> transposedScalableDims(vt.getRank());
  for (unsigned i = 0; i < transp.size(); ++i) {
    transposedShape[i] = vt.getShape()[transp[i]];
    transposedScalableDims[i] = vt.getScalableDims()[transp[i]];
  }

  result.addOperands(vector);
  result.addTypes(VectorType::get(transposedShape, vt.getElementType(),
                                  transposedScalableDims));
  result.addAttribute(TransposeOp::getTranspAttrName(result.name),
                      builder.getI64ArrayAttr(transp));
}

OpFoldResult vector::TransposeOp::fold(FoldAdaptor adaptor) {
  // Eliminate splat constant transpose ops.
  if (auto attr =
          llvm::dyn_cast_if_present<DenseElementsAttr>(adaptor.getVector()))
    if (attr.isSplat())
      return attr.reshape(getResultVectorType());

  // Eliminate identity transpose ops. This happens when the dimensions of the
  // input vector remain in their original order after the transpose operation.
  SmallVector<int64_t, 4> transp;
  getTransp(transp);

  // Check if the permutation of the dimensions contains sequential values:
  // {0, 1, 2, ...}.
  for (int64_t i = 0, e = transp.size(); i < e; i++) {
    if (transp[i] != i)
      return {};
  }

  return getVector();
}

LogicalResult vector::TransposeOp::verify() {
  VectorType vectorType = getSourceVectorType();
  VectorType resultType = getResultVectorType();
  int64_t rank = resultType.getRank();
  if (vectorType.getRank() != rank)
    return emitOpError("vector result rank mismatch: ") << rank;
  // Verify transposition array.
  auto transpAttr = getTransp().getValue();
  int64_t size = transpAttr.size();
  if (rank != size)
    return emitOpError("transposition length mismatch: ") << size;
  SmallVector<bool, 8> seen(rank, false);
  for (const auto &ta : llvm::enumerate(transpAttr)) {
    int64_t i = llvm::cast<IntegerAttr>(ta.value()).getInt();
    if (i < 0 || i >= rank)
      return emitOpError("transposition index out of range: ") << i;
    if (seen[i])
      return emitOpError("duplicate position index: ") << i;
    seen[i] = true;
    if (resultType.getDimSize(ta.index()) != vectorType.getDimSize(i))
      return emitOpError("dimension size mismatch at: ") << i;
  }
  return success();
}

std::optional<SmallVector<int64_t, 4>> TransposeOp::getShapeForUnroll() {
  return llvm::to_vector<4>(getResultVectorType().getShape());
}

namespace {

// Rewrites two back-to-back TransposeOp operations into a single TransposeOp.
class TransposeFolder final : public OpRewritePattern<vector::TransposeOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(vector::TransposeOp transposeOp,
                                PatternRewriter &rewriter) const override {
    // Wrapper around vector::TransposeOp::getTransp() for cleaner code.
    auto getPermutation = [](vector::TransposeOp transpose) {
      SmallVector<int64_t, 4> permutation;
      transpose.getTransp(permutation);
      return permutation;
    };

    // Composes two permutations: result[i] = permutation1[permutation2[i]].
    auto composePermutations = [](ArrayRef<int64_t> permutation1,
                                  ArrayRef<int64_t> permutation2) {
      SmallVector<int64_t, 4> result;
      for (auto index : permutation2)
        result.push_back(permutation1[index]);
      return result;
    };

    // Return if the input of 'transposeOp' is not defined by another transpose.
    vector::TransposeOp parentTransposeOp =
        transposeOp.getVector().getDefiningOp<vector::TransposeOp>();
    if (!parentTransposeOp)
      return failure();

    SmallVector<int64_t, 4> permutation = composePermutations(
        getPermutation(parentTransposeOp), getPermutation(transposeOp));
    // Replace 'transposeOp' with a new transpose operation.
    rewriter.replaceOpWithNewOp<vector::TransposeOp>(
        transposeOp, transposeOp.getResult().getType(),
        parentTransposeOp.getVector(),
        vector::getVectorSubscriptAttr(rewriter, permutation));
    return success();
  }
};

// Folds transpose(broadcast(<scalar>)) into brodcast(<scalar>).
struct FoldTransposedScalarBroadcast final
    : public OpRewritePattern<vector::TransposeOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(vector::TransposeOp transposeOp,
                                PatternRewriter &rewriter) const override {
    auto bcastOp = transposeOp.getVector().getDefiningOp<vector::BroadcastOp>();
    if (!bcastOp)
      return failure();

    auto srcVectorType = llvm::dyn_cast<VectorType>(bcastOp.getSourceType());
    if (!srcVectorType || srcVectorType.getNumElements() == 1) {
      rewriter.replaceOpWithNewOp<vector::BroadcastOp>(
          transposeOp, transposeOp.getResultVectorType(), bcastOp.getSource());
      return success();
    }

    return failure();
  }
};

// Folds transpose(splat x : src_type) : res_type into splat x : res_type.
class FoldTransposeSplat final : public OpRewritePattern<TransposeOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(TransposeOp transposeOp,
                                PatternRewriter &rewriter) const override {
    auto splatOp = transposeOp.getVector().getDefiningOp<vector::SplatOp>();
    if (!splatOp)
      return failure();

    rewriter.replaceOpWithNewOp<vector::SplatOp>(
        transposeOp, transposeOp.getResultVectorType(), splatOp.getInput());
    return success();
  }
};

/// Folds transpose(create_mask) into a new transposed create_mask.
class FoldTransposeCreateMask final : public OpRewritePattern<TransposeOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(TransposeOp transpOp,
                                PatternRewriter &rewriter) const override {
    Value transposeSrc = transpOp.getVector();
    auto createMaskOp = transposeSrc.getDefiningOp<vector::CreateMaskOp>();
    auto constantMaskOp = transposeSrc.getDefiningOp<vector::ConstantMaskOp>();
    if (!createMaskOp && !constantMaskOp)
      return failure();

    // Get the transpose permutation and apply it to the vector.create_mask or
    // vector.constant_mask operands.
    SmallVector<int64_t> permutation;
    transpOp.getTransp(permutation);

    if (createMaskOp) {
      auto maskOperands = createMaskOp.getOperands();
      SmallVector<Value> newOperands(maskOperands.begin(), maskOperands.end());
      applyPermutationToVector(newOperands, permutation);

      rewriter.replaceOpWithNewOp<vector::CreateMaskOp>(
          transpOp, transpOp.getResultVectorType(), newOperands);
      return success();
    }

    // ConstantMaskOp case.
    auto maskDimSizes = constantMaskOp.getMaskDimSizes();
    SmallVector<Attribute> newMaskDimSizes(maskDimSizes.getValue());
    applyPermutationToVector(newMaskDimSizes, permutation);

    rewriter.replaceOpWithNewOp<vector::ConstantMaskOp>(
        transpOp, transpOp.getResultVectorType(),
        ArrayAttr::get(transpOp.getContext(), newMaskDimSizes));
    return success();
  }
};

} // namespace

void vector::TransposeOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.add<FoldTransposeCreateMask, FoldTransposedScalarBroadcast,
              TransposeFolder, FoldTransposeSplat>(context);
}

void vector::TransposeOp::getTransp(SmallVectorImpl<int64_t> &results) {
  populateFromInt64AttrArray(getTransp(), results);
}

//===----------------------------------------------------------------------===//
// ConstantMaskOp
//===----------------------------------------------------------------------===//

LogicalResult ConstantMaskOp::verify() {
  auto resultType = llvm::cast<VectorType>(getResult().getType());
  // Check the corner case of 0-D vectors first.
  if (resultType.getRank() == 0) {
    if (getMaskDimSizes().size() != 1)
      return emitError("array attr must have length 1 for 0-D vectors");
    auto dim = llvm::cast<IntegerAttr>(getMaskDimSizes()[0]).getInt();
    if (dim != 0 && dim != 1)
      return emitError("mask dim size must be either 0 or 1 for 0-D vectors");
    return success();
  }

  // Verify that array attr size matches the rank of the vector result.
  if (static_cast<int64_t>(getMaskDimSizes().size()) != resultType.getRank())
    return emitOpError(
        "must specify array attr of size equal vector result rank");
  // Verify that each array attr element is in bounds of corresponding vector
  // result dimension size.
  auto resultShape = resultType.getShape();
  SmallVector<int64_t, 4> maskDimSizes;
  for (const auto &it : llvm::enumerate(getMaskDimSizes())) {
    int64_t attrValue = llvm::cast<IntegerAttr>(it.value()).getInt();
    if (attrValue < 0 || attrValue > resultShape[it.index()])
      return emitOpError(
          "array attr of size out of bounds of vector result dimension size");
    maskDimSizes.push_back(attrValue);
  }
  // Verify that if one mask dim size is zero, they all should be zero (because
  // the mask region is a conjunction of each mask dimension interval).
  bool anyZeros = llvm::is_contained(maskDimSizes, 0);
  bool allZeros = llvm::all_of(maskDimSizes, [](int64_t s) { return s == 0; });
  if (anyZeros && !allZeros)
    return emitOpError("expected all mask dim sizes to be zeros, "
                       "as a result of conjunction with zero mask dim");
  // Verify that if the mask type is scalable, dimensions should be zero because
  // constant scalable masks can only be defined for the "none set" or "all set"
  // cases, and there is no VLA way to define an "all set" case for
  // `vector.constant_mask`. In the future, a convention could be established
  // to decide if a specific dimension value could be considered as "all set".
  if (resultType.isScalable() &&
      llvm::cast<IntegerAttr>(getMaskDimSizes()[0]).getInt() != 0)
    return emitOpError("expected mask dim sizes for scalable masks to be 0");
  return success();
}

//===----------------------------------------------------------------------===//
// CreateMaskOp
//===----------------------------------------------------------------------===//

void CreateMaskOp::build(OpBuilder &builder, OperationState &result,
                         VectorType type,
                         ArrayRef<OpFoldResult> mixedOperands) {
  SmallVector<Value> operands =
      getValueOrCreateConstantIndexOp(builder, result.location, mixedOperands);
  build(builder, result, type, operands);
}

LogicalResult CreateMaskOp::verify() {
  auto vectorType = llvm::cast<VectorType>(getResult().getType());
  // Verify that an operand was specified for each result vector each dimension.
  if (vectorType.getRank() == 0) {
    if (getNumOperands() != 1)
      return emitOpError(
          "must specify exactly one operand for 0-D create_mask");
  } else if (getNumOperands() !=
             llvm::cast<VectorType>(getResult().getType()).getRank()) {
    return emitOpError(
        "must specify an operand for each result vector dimension");
  }
  return success();
}

namespace {

// Pattern to rewrite a CreateMaskOp with a ConstantMaskOp.
class CreateMaskFolder final : public OpRewritePattern<CreateMaskOp> {
public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(CreateMaskOp createMaskOp,
                                PatternRewriter &rewriter) const override {
    // Return if any of 'createMaskOp' operands are not defined by a constant.
    auto isNotDefByConstant = [](Value operand) {
      return !getConstantIntValue(operand).has_value();
    };
    if (llvm::any_of(createMaskOp.getOperands(), isNotDefByConstant))
      return failure();

    // CreateMaskOp for scalable vectors can be folded only if all dimensions
    // are negative or zero.
    if (auto vType = llvm::dyn_cast<VectorType>(createMaskOp.getType())) {
      if (vType.isScalable())
        for (auto opDim : createMaskOp.getOperands()) {
          APInt intVal;
          if (matchPattern(opDim, m_ConstantInt(&intVal)) &&
              intVal.isStrictlyPositive())
            return failure();
        }
    }

    // Gather constant mask dimension sizes.
    SmallVector<int64_t, 4> maskDimSizes;
    maskDimSizes.reserve(createMaskOp->getNumOperands());
    for (auto [operand, maxDimSize] : llvm::zip_equal(
             createMaskOp.getOperands(), createMaskOp.getType().getShape())) {
      int64_t dimSize = getConstantIntValue(operand).value();
      dimSize = std::min(dimSize, maxDimSize);
      // If one of dim sizes is zero, set all dims to zero.
      if (dimSize <= 0) {
        maskDimSizes.assign(createMaskOp.getType().getRank(), 0);
        break;
      }
      maskDimSizes.push_back(dimSize);
    }
    // Replace 'createMaskOp' with ConstantMaskOp.
    rewriter.replaceOpWithNewOp<ConstantMaskOp>(
        createMaskOp, createMaskOp.getResult().getType(),
        vector::getVectorSubscriptAttr(rewriter, maskDimSizes));
    return success();
  }
};

} // namespace

void CreateMaskOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                               MLIRContext *context) {
  results.add<CreateMaskFolder>(context);
}

//===----------------------------------------------------------------------===//
// MaskOp
//===----------------------------------------------------------------------===//

void MaskOp::build(
    OpBuilder &builder, OperationState &result, Value mask,
    Operation *maskableOp,
    function_ref<void(OpBuilder &, Operation *)> maskRegionBuilder) {
  assert(maskRegionBuilder &&
         "builder callback for 'maskRegion' must be present");

  result.addOperands(mask);
  OpBuilder::InsertionGuard guard(builder);
  Region *maskRegion = result.addRegion();
  builder.createBlock(maskRegion);
  maskRegionBuilder(builder, maskableOp);
}

void MaskOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTypes,
    Value mask, Operation *maskableOp,
    function_ref<void(OpBuilder &, Operation *)> maskRegionBuilder) {
  build(builder, result, resultTypes, mask, /*passthru=*/Value(), maskableOp,
        maskRegionBuilder);
}

void MaskOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTypes,
    Value mask, Value passthru, Operation *maskableOp,
    function_ref<void(OpBuilder &, Operation *)> maskRegionBuilder) {
  build(builder, result, mask, maskableOp, maskRegionBuilder);
  if (passthru)
    result.addOperands(passthru);
  result.addTypes(resultTypes);
}

ParseResult MaskOp::parse(OpAsmParser &parser, OperationState &result) {
  // Create the op region.
  result.regions.reserve(1);
  Region &maskRegion = *result.addRegion();

  auto &builder = parser.getBuilder();

  // Parse all the operands.
  OpAsmParser::UnresolvedOperand mask;
  if (parser.parseOperand(mask))
    return failure();

  // Optional passthru operand.
  OpAsmParser::UnresolvedOperand passthru;
  ParseResult parsePassthru = parser.parseOptionalComma();
  if (parsePassthru.succeeded() && parser.parseOperand(passthru))
    return failure();

  // Parse op region.
  if (parser.parseRegion(maskRegion, /*arguments=*/{}, /*argTypes=*/{}))
    return failure();

  MaskOp::ensureTerminator(maskRegion, builder, result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  // Parse all the types.
  Type maskType;
  if (parser.parseColonType(maskType))
    return failure();

  SmallVector<Type> resultTypes;
  if (parser.parseOptionalArrowTypeList(resultTypes))
    return failure();
  result.types.append(resultTypes);

  // Resolve operands.
  if (parser.resolveOperand(mask, maskType, result.operands))
    return failure();

  if (parsePassthru.succeeded())
    if (parser.resolveOperand(passthru, resultTypes[0], result.operands))
      return failure();

  return success();
}

void mlir::vector::MaskOp::print(OpAsmPrinter &p) {
  p << " " << getMask();
  if (getPassthru())
    p << ", " << getPassthru();

  // Print single masked operation and skip terminator.
  p << " { ";
  Block *singleBlock = &getMaskRegion().getBlocks().front();
  if (singleBlock && !singleBlock->getOperations().empty())
    p.printCustomOrGenericOp(&singleBlock->front());
  p << " }";

  p.printOptionalAttrDict(getOperation()->getAttrs());

  p << " : " << getMask().getType();
  if (getNumResults() > 0)
    p << " -> " << getResultTypes();
}

void MaskOp::ensureTerminator(Region &region, Builder &builder, Location loc) {
  OpTrait::SingleBlockImplicitTerminator<vector::YieldOp>::Impl<
      MaskOp>::ensureTerminator(region, builder, loc);
  // Keep the default yield terminator if the number of masked operations is not
  // the expected. This case will trigger a verification failure.
  Block &block = region.front();
  if (block.getOperations().size() != 2)
    return;

  // Replace default yield terminator with a new one that returns the results
  // from the masked operation.
  OpBuilder opBuilder(builder.getContext());
  Operation *maskedOp = &block.front();
  Operation *oldYieldOp = &block.back();
  assert(isa<vector::YieldOp>(oldYieldOp) && "Expected vector::YieldOp");

  // Empty vector.mask op.
  if (maskedOp == oldYieldOp)
    return;

  opBuilder.setInsertionPoint(oldYieldOp);
  opBuilder.create<vector::YieldOp>(loc, maskedOp->getResults());
  oldYieldOp->dropAllReferences();
  oldYieldOp->erase();
}

LogicalResult MaskOp::verify() {
  // Structural checks.
  Block &block = getMaskRegion().getBlocks().front();
  if (block.getOperations().empty())
    return emitOpError("expects a terminator within the mask region");
  if (block.getOperations().size() > 2)
    return emitOpError("expects only one operation to mask");

  // Terminator checks.
  auto terminator = dyn_cast<vector::YieldOp>(block.back());
  if (!terminator)
    return emitOpError("expects a terminator within the mask region");

  if (terminator->getNumOperands() != getNumResults())
    return emitOpError(
        "expects number of results to match mask region yielded values");

  auto maskableOp = dyn_cast<MaskableOpInterface>(block.front());
  // Empty vector.mask. Nothing else to check.
  if (!maskableOp)
    return success();

  // Result checks.
  if (maskableOp->getNumResults() != getNumResults())
    return emitOpError("expects number of results to match maskable operation "
                       "number of results");

  if (!llvm::equal(maskableOp->getResultTypes(), getResultTypes()))
    return emitOpError(
        "expects result type to match maskable operation result type");

  if (llvm::count_if(maskableOp->getResultTypes(),
                     [](Type t) { return llvm::isa<VectorType>(t); }) > 1)
    return emitOpError("multiple vector results not supported");

  // Mask checks.
  Type expectedMaskType = maskableOp.getExpectedMaskType();
  if (getMask().getType() != expectedMaskType)
    return emitOpError("expects a ")
           << expectedMaskType << " mask for the maskable operation";

  // Passthru checks.
  Value passthru = getPassthru();
  if (passthru) {
    if (!maskableOp.supportsPassthru())
      return emitOpError(
          "doesn't expect a passthru argument for this maskable operation");

    if (maskableOp->getNumResults() != 1)
      return emitOpError("expects result when passthru argument is provided");

    if (passthru.getType() != maskableOp->getResultTypes()[0])
      return emitOpError("expects passthru type to match result type");
  }

  return success();
}

/// Folds vector.mask ops with an all-true mask.
LogicalResult MaskOp::fold(FoldAdaptor adaptor,
                           SmallVectorImpl<OpFoldResult> &results) {
  MaskFormat maskFormat = getMaskFormat(getMask());
  if (isEmpty())
    return failure();

  if (maskFormat != MaskFormat::AllTrue)
    return failure();

  // Move maskable operation outside of the `vector.mask` region.
  Operation *maskableOp = getMaskableOp();
  maskableOp->dropAllUses();
  maskableOp->moveBefore(getOperation());

  results.push_back(maskableOp->getResult(0));
  return success();
}

// Elides empty vector.mask operations with or without return values. Propagates
// the yielded values by the vector.yield terminator, if any, or erases the op,
// otherwise.
class ElideEmptyMaskOp : public OpRewritePattern<MaskOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(MaskOp maskOp,
                                PatternRewriter &rewriter) const override {
    auto maskingOp = cast<MaskingOpInterface>(maskOp.getOperation());
    if (maskingOp.getMaskableOp())
      return failure();

    if (!maskOp.isEmpty())
      return failure();

    Block *block = maskOp.getMaskBlock();
    auto terminator = cast<vector::YieldOp>(block->front());
    if (terminator.getNumOperands() == 0)
      rewriter.eraseOp(maskOp);
    else
      rewriter.replaceOp(maskOp, terminator.getOperands());

    return success();
  }
};

void MaskOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                         MLIRContext *context) {
  results.add<ElideEmptyMaskOp>(context);
}

// MaskingOpInterface definitions.

/// Returns the operation masked by this 'vector.mask'.
Operation *MaskOp::getMaskableOp() {
  Block *block = getMaskBlock();
  if (block->getOperations().size() < 2)
    return nullptr;

  return &block->front();
}

/// Returns true if 'vector.mask' has a passthru value.
bool MaskOp::hasPassthru() { return getPassthru() != Value(); }

//===----------------------------------------------------------------------===//
// ScanOp
//===----------------------------------------------------------------------===//

LogicalResult ScanOp::verify() {
  VectorType srcType = getSourceType();
  VectorType initialType = getInitialValueType();
  // Check reduction dimension < rank.
  int64_t srcRank = srcType.getRank();
  int64_t reductionDim = getReductionDim();
  if (reductionDim >= srcRank)
    return emitOpError("reduction dimension ")
           << reductionDim << " has to be less than " << srcRank;

  // Check that rank(initial_value) = rank(src) - 1.
  int64_t initialValueRank = initialType.getRank();
  if (initialValueRank != srcRank - 1)
    return emitOpError("initial value rank ")
           << initialValueRank << " has to be equal to " << srcRank - 1;

  // Check shapes of initial value and src.
  ArrayRef<int64_t> srcShape = srcType.getShape();
  ArrayRef<int64_t> initialValueShapes = initialType.getShape();
  SmallVector<int64_t> expectedShape;
  for (int i = 0; i < srcRank; i++) {
    if (i != reductionDim)
      expectedShape.push_back(srcShape[i]);
  }
  if (!llvm::equal(initialValueShapes, expectedShape)) {
    return emitOpError("incompatible input/initial value shapes");
  }

  // Verify supported reduction kind.
  Type eltType = getDestType().getElementType();
  if (!isSupportedCombiningKind(getKind(), eltType))
    return emitOpError("unsupported reduction type ")
           << eltType << " for kind '" << stringifyCombiningKind(getKind())
           << "'";

  return success();
}

void mlir::vector::populateVectorToVectorCanonicalizationPatterns(
    RewritePatternSet &patterns, PatternBenefit benefit) {
  patterns
      .add<CreateMaskFolder, MaskedLoadFolder, MaskedStoreFolder, GatherFolder,
           ScatterFolder, ExpandLoadFolder, CompressStoreFolder,
           StridedSliceConstantMaskFolder, TransposeFolder>(
          patterns.getContext(), benefit);
}

//===----------------------------------------------------------------------===//
// SplatOp
//===----------------------------------------------------------------------===//

OpFoldResult SplatOp::fold(FoldAdaptor adaptor) {
  auto constOperand = adaptor.getInput();
  if (!constOperand.isa_and_nonnull<IntegerAttr, FloatAttr>())
    return {};

  // SplatElementsAttr::get treats single value for second arg as being a splat.
  return SplatElementsAttr::get(getType(), {constOperand});
}

//===----------------------------------------------------------------------===//
// WarpExecuteOnLane0Op
//===----------------------------------------------------------------------===//

void WarpExecuteOnLane0Op::print(OpAsmPrinter &p) {
  p << "(" << getLaneid() << ")";

  SmallVector<StringRef> coreAttr = {getWarpSizeAttrName()};
  auto warpSizeAttr = getOperation()->getAttr(getWarpSizeAttrName());
  p << "[" << llvm::cast<IntegerAttr>(warpSizeAttr).getInt() << "]";

  if (!getArgs().empty())
    p << " args(" << getArgs() << " : " << getArgs().getTypes() << ")";
  if (!getResults().empty())
    p << " -> (" << getResults().getTypes() << ')';
  p << " ";
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/true,
                /*printBlockTerminators=*/!getResults().empty());
  p.printOptionalAttrDict(getOperation()->getAttrs(), coreAttr);
}

ParseResult WarpExecuteOnLane0Op::parse(OpAsmParser &parser,
                                        OperationState &result) {
  // Create the region.
  result.regions.reserve(1);
  Region *warpRegion = result.addRegion();

  auto &builder = parser.getBuilder();
  OpAsmParser::UnresolvedOperand laneId;

  // Parse predicate operand.
  if (parser.parseLParen() ||
      parser.parseOperand(laneId, /*allowResultNumber=*/false) ||
      parser.parseRParen())
    return failure();

  int64_t warpSize;
  if (parser.parseLSquare() || parser.parseInteger(warpSize) ||
      parser.parseRSquare())
    return failure();
  result.addAttribute(getWarpSizeAttrName(OperationName(getOperationName(),
                                                        builder.getContext())),
                      builder.getI64IntegerAttr(warpSize));

  if (parser.resolveOperand(laneId, builder.getIndexType(), result.operands))
    return failure();

  llvm::SMLoc inputsOperandsLoc;
  SmallVector<OpAsmParser::UnresolvedOperand> inputsOperands;
  SmallVector<Type> inputTypes;
  if (succeeded(parser.parseOptionalKeyword("args"))) {
    if (parser.parseLParen())
      return failure();

    inputsOperandsLoc = parser.getCurrentLocation();
    if (parser.parseOperandList(inputsOperands) ||
        parser.parseColonTypeList(inputTypes) || parser.parseRParen())
      return failure();
  }
  if (parser.resolveOperands(inputsOperands, inputTypes, inputsOperandsLoc,
                             result.operands))
    return failure();

  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();
  // Parse the region.
  if (parser.parseRegion(*warpRegion, /*arguments=*/{},
                         /*argTypes=*/{}))
    return failure();
  WarpExecuteOnLane0Op::ensureTerminator(*warpRegion, builder, result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void WarpExecuteOnLane0Op::getSuccessorRegions(
    std::optional<unsigned> index, SmallVectorImpl<RegionSuccessor> &regions) {
  if (index) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  // The warp region is always executed
  regions.push_back(RegionSuccessor(&getWarpRegion()));
}

void WarpExecuteOnLane0Op::build(OpBuilder &builder, OperationState &result,
                                 TypeRange resultTypes, Value laneId,
                                 int64_t warpSize) {
  build(builder, result, resultTypes, laneId, warpSize,
        /*operands=*/std::nullopt, /*argTypes=*/std::nullopt);
}

void WarpExecuteOnLane0Op::build(OpBuilder &builder, OperationState &result,
                                 TypeRange resultTypes, Value laneId,
                                 int64_t warpSize, ValueRange args,
                                 TypeRange blockArgTypes) {
  result.addOperands(laneId);
  result.addAttribute(getAttributeNames()[0],
                      builder.getI64IntegerAttr(warpSize));
  result.addTypes(resultTypes);
  result.addOperands(args);
  assert(args.size() == blockArgTypes.size());
  OpBuilder::InsertionGuard guard(builder);
  Region *warpRegion = result.addRegion();
  Block *block = builder.createBlock(warpRegion);
  for (auto [type, arg] : llvm::zip_equal(blockArgTypes, args))
    block->addArgument(type, arg.getLoc());
}

/// Helper check if the distributed vector type is consistent with the expanded
/// type and distributed size.
static LogicalResult verifyDistributedType(Type expanded, Type distributed,
                                           int64_t warpSize, Operation *op) {
  // If the types matches there is no distribution.
  if (expanded == distributed)
    return success();
  auto expandedVecType = llvm::dyn_cast<VectorType>(expanded);
  auto distributedVecType = llvm::dyn_cast<VectorType>(distributed);
  if (!expandedVecType || !distributedVecType)
    return op->emitOpError("expected vector type for distributed operands.");
  if (expandedVecType.getRank() != distributedVecType.getRank() ||
      expandedVecType.getElementType() != distributedVecType.getElementType())
    return op->emitOpError(
        "expected distributed vectors to have same rank and element type.");
  bool foundDistributedDim = false;
  for (int64_t i = 0, e = expandedVecType.getRank(); i < e; i++) {
    if (expandedVecType.getDimSize(i) == distributedVecType.getDimSize(i))
      continue;
    if (expandedVecType.getDimSize(i) ==
        distributedVecType.getDimSize(i) * warpSize) {
      if (foundDistributedDim)
        return op->emitOpError()
               << "expected only one dimension to be distributed from "
               << expandedVecType << " to " << distributedVecType;
      foundDistributedDim = true;
      continue;
    }
    return op->emitOpError() << "incompatible distribution dimensions from "
                             << expandedVecType << " to " << distributedVecType;
  }
  return success();
}

LogicalResult WarpExecuteOnLane0Op::verify() {
  if (getArgs().size() != getWarpRegion().getNumArguments())
    return emitOpError(
        "expected same number op arguments and block arguments.");
  auto yield =
      cast<YieldOp>(getWarpRegion().getBlocks().begin()->getTerminator());
  if (yield.getNumOperands() != getNumResults())
    return emitOpError(
        "expected same number of yield operands and return values.");
  int64_t warpSize = getWarpSize();
  for (auto [regionArg, arg] :
       llvm::zip_equal(getWarpRegion().getArguments(), getArgs())) {
    if (failed(verifyDistributedType(regionArg.getType(), arg.getType(),
                                     warpSize, getOperation())))
      return failure();
  }
  for (auto [yieldOperand, result] :
       llvm::zip_equal(yield.getOperands(), getResults())) {
    if (failed(verifyDistributedType(yieldOperand.getType(), result.getType(),
                                     warpSize, getOperation())))
      return failure();
  }
  return success();
}

bool WarpExecuteOnLane0Op::areTypesCompatible(Type lhs, Type rhs) {
  return succeeded(
      verifyDistributedType(lhs, rhs, getWarpSize(), getOperation()));
}

Value mlir::vector::makeArithReduction(OpBuilder &b, Location loc,
                                       CombiningKind kind, Value v1, Value acc,
                                       Value mask) {
  Type t1 = getElementTypeOrSelf(v1.getType());
  Type tAcc = getElementTypeOrSelf(acc.getType());
  Value result;

  switch (kind) {
  case CombiningKind::ADD:
    if (t1.isIntOrIndex() && tAcc.isIntOrIndex())
      result = b.createOrFold<arith::AddIOp>(loc, v1, acc);
    else if (llvm::isa<FloatType>(t1) && llvm::isa<FloatType>(tAcc))
      result = b.createOrFold<arith::AddFOp>(loc, v1, acc);
    else
      llvm_unreachable("invalid value types for ADD reduction");
    break;
  case CombiningKind::AND:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::AndIOp>(loc, v1, acc);
    break;
  case CombiningKind::MAXF:
    assert(llvm::isa<FloatType>(t1) && llvm::isa<FloatType>(tAcc) &&
           "expected float values");
    result = b.createOrFold<arith::MaxFOp>(loc, v1, acc);
    break;
  case CombiningKind::MINF:
    assert(llvm::isa<FloatType>(t1) && llvm::isa<FloatType>(tAcc) &&
           "expected float values");
    result = b.createOrFold<arith::MinFOp>(loc, v1, acc);
    break;
  case CombiningKind::MAXSI:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::MaxSIOp>(loc, v1, acc);
    break;
  case CombiningKind::MINSI:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::MinSIOp>(loc, v1, acc);
    break;
  case CombiningKind::MAXUI:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::MaxUIOp>(loc, v1, acc);
    break;
  case CombiningKind::MINUI:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::MinUIOp>(loc, v1, acc);
    break;
  case CombiningKind::MUL:
    if (t1.isIntOrIndex() && tAcc.isIntOrIndex())
      result = b.createOrFold<arith::MulIOp>(loc, v1, acc);
    else if (llvm::isa<FloatType>(t1) && llvm::isa<FloatType>(tAcc))
      result = b.createOrFold<arith::MulFOp>(loc, v1, acc);
    else
      llvm_unreachable("invalid value types for MUL reduction");
    break;
  case CombiningKind::OR:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::OrIOp>(loc, v1, acc);
    break;
  case CombiningKind::XOR:
    assert(t1.isIntOrIndex() && tAcc.isIntOrIndex() && "expected int values");
    result = b.createOrFold<arith::XOrIOp>(loc, v1, acc);
    break;
  };

  assert(result && "unknown CombiningKind");
  return selectPassthru(b, mask, result, acc);
}

//===----------------------------------------------------------------------===//
// Vector Masking Utilities
//===----------------------------------------------------------------------===//

/// Create the vector.yield-ended region of a vector.mask op with `maskableOp`
/// as masked operation.
void mlir::vector::createMaskOpRegion(OpBuilder &builder,
                                      Operation *maskableOp) {
  assert(maskableOp->getBlock() && "MaskableOp must be inserted into a block");
  Block *insBlock = builder.getInsertionBlock();
  // Create a block and move the op to that block.
  insBlock->getOperations().splice(
      insBlock->begin(), maskableOp->getBlock()->getOperations(), maskableOp);
  builder.create<YieldOp>(maskableOp->getLoc(), maskableOp->getResults());
}

/// Creates a vector.mask operation around a maskable operation. Returns the
/// vector.mask operation if the mask provided is valid. Otherwise, returns
/// the maskable operation itself.
Operation *mlir::vector::maskOperation(OpBuilder &builder,
                                       Operation *maskableOp, Value mask,
                                       Value passthru) {
  if (!mask)
    return maskableOp;
  if (passthru)
    return builder.create<MaskOp>(maskableOp->getLoc(),
                                  maskableOp->getResultTypes(), mask, passthru,
                                  maskableOp, createMaskOpRegion);
  return builder.create<MaskOp>(maskableOp->getLoc(),
                                maskableOp->getResultTypes(), mask, maskableOp,
                                createMaskOpRegion);
}

/// Creates a vector select operation that picks values from `newValue` or
/// `passthru` for each result vector lane based on `mask`. This utility is used
/// to propagate the pass-thru value of vector.mask or for cases where only the
/// pass-thru value propagation is needed. VP intrinsics do not support
/// pass-thru values and every mask-out lane is set to poison. LLVM backends are
/// usually able to match op + select patterns and fold them into a native
/// target instructions.
Value mlir::vector::selectPassthru(OpBuilder &builder, Value mask,
                                   Value newValue, Value passthru) {
  if (!mask)
    return newValue;

  return builder.create<arith::SelectOp>(newValue.getLoc(), newValue.getType(),
                                         mask, newValue, passthru);
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_ATTRDEF_CLASSES
#include "mlir/Dialect/Vector/IR/VectorOpsAttrDefs.cpp.inc"

#define GET_OP_CLASSES
#include "mlir/Dialect/Vector/IR/VectorOps.cpp.inc"
