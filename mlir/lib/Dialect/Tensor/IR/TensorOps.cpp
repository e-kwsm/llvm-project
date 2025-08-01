//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/Complex/IR/Complex.h"
#include "mlir/Dialect/Linalg/IR/RelayoutOpInterface.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Utils/IndexingUtils.h"
#include "mlir/Dialect/Utils/ReshapeOpsUtils.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributeInterfaces.h"
#include "mlir/IR/BuiltinTypeInterfaces.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/DestinationStyleOpInterface.h"
#include "mlir/Interfaces/InferIntRangeInterface.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "mlir/Interfaces/Utils/InferIntRangeCommon.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MathExtras.h"
#include <optional>

using namespace mlir;
using namespace mlir::tensor;

using llvm::divideCeilSigned;
using llvm::divideFloorSigned;
using llvm::mod;

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *TensorDialect::materializeConstant(OpBuilder &builder,
                                              Attribute value, Type type,
                                              Location loc) {
  if (auto op = arith::ConstantOp::materialize(builder, value, type, loc))
    return op;
  if (complex::ConstantOp::isBuildableWith(value, type))
    return complex::ConstantOp::create(builder, loc, type,
                                       llvm::cast<ArrayAttr>(value));
  return nullptr;
}

OpFoldResult tensor::getMixedSize(OpBuilder &builder, Location loc, Value value,
                                  int64_t dim) {
  auto tensorType = llvm::cast<RankedTensorType>(value.getType());
  if (tensorType.isDynamicDim(dim))
    return builder.createOrFold<tensor::DimOp>(loc, value, dim);

  return builder.getIndexAttr(tensorType.getDimSize(dim));
}

SmallVector<OpFoldResult> tensor::getMixedSizes(OpBuilder &builder,
                                                Location loc, Value value) {
  auto tensorType = llvm::cast<RankedTensorType>(value.getType());
  SmallVector<OpFoldResult> result;
  for (int64_t i = 0; i < tensorType.getRank(); ++i)
    result.push_back(getMixedSize(builder, loc, value, i));
  return result;
}

FailureOr<Value> tensor::getOrCreateDestination(OpBuilder &b, Location loc,
                                                OpResult opResult) {
  auto tensorType = llvm::dyn_cast<TensorType>(opResult.getType());
  assert(tensorType && "expected tensor type");

  // If the op has a destination, it implements DestinationStyleOpInterface and
  // we can query the destination operand from that interface.
  auto destOp = opResult.getDefiningOp<DestinationStyleOpInterface>();
  if (destOp)
    return destOp.getTiedOpOperand(opResult)->get();

  // Otherwise, create a new destination tensor with the same shape.
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(opResult.getDefiningOp());

  // Compute sizes.
  SmallVector<OpFoldResult> mixedSizes;
  if (!tensorType.hasStaticShape()) {
    // Dynamic shape: Query ReifyRankedShapedTypeOpInterface.
    ReifiedRankedShapedTypeDims reifiedShapes;
    if (failed(reifyResultShapes(b, opResult.getDefiningOp(), reifiedShapes)))
      return failure();
    mixedSizes = reifiedShapes[opResult.getResultNumber()];
  } else {
    // Static shape: Take static sizes directly.
    for (int64_t sz : tensorType.getShape())
      mixedSizes.push_back(b.getIndexAttr(sz));
  }

  // Create empty tensor.
  Value emptyTensor =
      tensor::EmptyOp::create(b, loc, mixedSizes, tensorType.getElementType());
  return emptyTensor;
}

LogicalResult tensor::getOrCreateDestinations(OpBuilder &b, Location loc,
                                              Operation *op,
                                              SmallVector<Value> &result) {
  for (OpResult opResult : op->getResults()) {
    if (llvm::isa<TensorType>(opResult.getType())) {
      FailureOr<Value> destination = getOrCreateDestination(b, loc, opResult);
      if (failed(destination))
        return failure();
      result.push_back(*destination);
    }
  }
  return success();
}

bool tensor::isSameTypeWithoutEncoding(Type tp1, Type tp2) {
  if (auto rtp1 = llvm::dyn_cast<RankedTensorType>(tp1)) {
    if (auto rtp2 = llvm::dyn_cast<RankedTensorType>(tp2))
      return rtp1.getShape() == rtp2.getShape() &&
             rtp1.getElementType() == rtp2.getElementType();
    return false;
  }
  return tp1 == tp2; // default implementation
}

/// Compute the dropped dimensions of a rank-reducing tensor.extract_slice op or
/// rank-extending tensor.insert_slice op.
static llvm::SmallBitVector getDroppedDims(ArrayRef<int64_t> reducedShape,
                                           ArrayRef<OpFoldResult> mixedSizes) {
  llvm::SmallBitVector droppedDims(mixedSizes.size());
  int64_t shapePos = reducedShape.size() - 1;

  for (const auto &size : enumerate(llvm::reverse(mixedSizes))) {
    size_t idx = mixedSizes.size() - size.index() - 1;
    // Rank-reduced dims must have a static unit dimension.
    bool isStaticUnitSize =
        isa<Attribute>(size.value()) &&
        llvm::cast<IntegerAttr>(cast<Attribute>(size.value())).getInt() == 1;

    if (shapePos < 0) {
      // There are no more dims in the reduced shape. All remaining sizes must
      // be rank-reduced dims.
      assert(isStaticUnitSize && "expected unit dim");
      droppedDims.set(idx);
      continue;
    }

    // Dim is preserved if the size is not a static 1.
    if (!isStaticUnitSize) {
      --shapePos;
      continue;
    }

    // Dim is preserved if the reduced shape dim is also 1.
    if (reducedShape[shapePos] == 1) {
      --shapePos;
      continue;
    }

    // Otherwise: Dim is dropped.
    droppedDims.set(idx);
  }

  assert(shapePos < 0 && "dimension mismatch");
  return droppedDims;
}

/// Given a ranked tensor type and a range of values that defines its dynamic
/// dimension sizes, turn all dynamic sizes that have a constant value into
/// static dimension sizes.
static RankedTensorType
foldDynamicToStaticDimSizes(RankedTensorType type, ValueRange dynamicSizes,
                            SmallVector<Value> &foldedDynamicSizes) {
  SmallVector<int64_t> staticShape(type.getShape());
  assert(type.getNumDynamicDims() == dynamicSizes.size() &&
         "incorrect number of dynamic sizes");

  // Compute new static and dynamic sizes.
  unsigned ctr = 0;
  for (int64_t i = 0, e = type.getRank(); i < e; ++i) {
    if (type.isDynamicDim(i)) {
      Value dynamicSize = dynamicSizes[ctr++];
      std::optional<int64_t> cst = getConstantIntValue(dynamicSize);
      if (cst.has_value()) {
        // Dynamic size must be non-negative.
        if (cst.value() < 0) {
          foldedDynamicSizes.push_back(dynamicSize);
          continue;
        }
        staticShape[i] = *cst;
      } else {
        foldedDynamicSizes.push_back(dynamicSize);
      }
    }
  }

  return RankedTensorType::get(staticShape, type.getElementType(),
                               type.getEncoding());
}

//===----------------------------------------------------------------------===//
// BitcastOp
//===----------------------------------------------------------------------===//

bool BitcastOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  Type a = inputs.front(), b = outputs.front();
  auto aT = dyn_cast<TensorType>(a);
  auto bT = dyn_cast<TensorType>(b);
  if (!aT || !bT)
    return false;

  if (aT.getElementTypeBitWidth() != bT.getElementTypeBitWidth())
    return false;

  return succeeded(verifyCompatibleShape(aT, bT));
}

namespace {

/// Replaces chains of two tensor.bitcast operations by a single tensor.bitcast
/// operation.
struct ChainedTensorBitcast : public OpRewritePattern<BitcastOp> {
  using OpRewritePattern<BitcastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(BitcastOp tensorBitcast,
                                PatternRewriter &rewriter) const final {
    auto tensorBitcastOperand =
        tensorBitcast.getOperand().getDefiningOp<BitcastOp>();
    if (!tensorBitcastOperand)
      return failure();

    auto resultType = cast<TensorType>(tensorBitcast.getType());
    rewriter.replaceOpWithNewOp<BitcastOp>(tensorBitcast, resultType,
                                           tensorBitcastOperand.getOperand());
    return success();
  }
};

} // namespace

void BitcastOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<ChainedTensorBitcast>(context);
}

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

void CastOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "cast");
}

/// Returns true if `target` is a ranked tensor type that preserves static
/// information available in the `source` ranked tensor type.
bool mlir::tensor::preservesStaticInformation(Type source, Type target) {
  auto sourceType = llvm::dyn_cast<RankedTensorType>(source);
  auto targetType = llvm::dyn_cast<RankedTensorType>(target);

  // Requires RankedTensorType.
  if (!sourceType || !targetType)
    return false;

  // Requires same elemental type.
  if (sourceType.getElementType() != targetType.getElementType())
    return false;

  // Requires same rank.
  if (sourceType.getRank() != targetType.getRank())
    return false;

  // Requires same encoding.
  if (sourceType.getEncoding() != targetType.getEncoding())
    return false;

  // If cast is towards more static sizes along any dimension, don't fold.
  for (auto t : llvm::zip(sourceType.getShape(), targetType.getShape())) {
    if (ShapedType::isStatic(std::get<0>(t)) &&
        ShapedType::isDynamic(std::get<1>(t)))
      return false;
  }

  return true;
}

/// Determines whether tensor::CastOp casts to a more dynamic version of the
/// source tensor. This is useful to fold a tensor.cast into a consuming op and
/// implement canonicalization patterns for ops in different dialects that may
/// consume the results of tensor.cast operations. Such foldable tensor.cast
/// operations are typically inserted as `slice` ops and are canonicalized,
/// to preserve the type compatibility of their uses.
///
/// Returns true when all conditions are met:
/// 1. source and result are ranked tensors with same element type and rank.
/// 2. the tensor type has more static information than the result
///
/// Example:
/// ```mlir
///   %1 = tensor.cast %0 : tensor<8x16xf32> to tensor<?x?xf32>
///   %2 = consumer %1 ... : tensor<?x?xf32> ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %2 = consumer %0 ... : tensor<8x16xf32> ...
/// ```
bool mlir::tensor::canFoldIntoConsumerOp(CastOp castOp) {
  if (!castOp)
    return false;

  // Can fold if the source of cast has at least as much static information as
  // its results.
  return preservesStaticInformation(castOp.getType(),
                                    castOp.getSource().getType());
}

/// Determines whether the tensor::CastOp casts to a more static version of the
/// source tensor. This is useful to fold into a producing op and implement
/// canonicalization patterns with the `tensor.cast` op as the root, but
/// producer being from different dialects. Returns true when all conditions are
/// met:
/// 1. source and result and ranked tensors with same element type and rank.
/// 2. the result type has more static information than the source.
///
/// Example:
/// ```mlir
///   %1 = producer ... : tensor<?x?xf32>
///   %2 = tensor.cast %1 : tensor<?x?xf32> to tensor<8x16xf32>
/// ```
///
/// can be canonicalized to :
///
/// ```mlir
///   %2 = producer ... : tensor<8x16xf32>
/// ```
/// Not all ops might be canonicalizable this way, but for those that can be,
/// this method provides a check that it is worth doing the canonicalization.
bool mlir::tensor::canFoldIntoProducerOp(CastOp castOp) {
  if (!castOp)
    return false;
  return preservesStaticInformation(castOp.getSource().getType(),
                                    castOp.getType());
}

bool mlir::tensor::hasFoldableTensorCastOperand(Operation *op) {
  return llvm::any_of(op->getOpOperands(), [&](OpOperand &opOperand) {
    if (llvm::isa<BlockArgument>(opOperand.get()))
      return false;
    auto castOp = opOperand.get().getDefiningOp<tensor::CastOp>();
    return castOp && canFoldIntoConsumerOp(castOp);
  });
}

SmallVector<Value> mlir::tensor::getUpdatedOperandsAfterCastOpFolding(
    DestinationStyleOpInterface op, SmallVector<Type> &newResTy) {
  SmallVector<Value> newOperands;
  newOperands.reserve(op->getNumOperands());

  assert(hasFoldableTensorCastOperand(op) && "No foldable CastOp operands!");

  // Assumes that the result has dpsInits followed by nonDpsInits.
  int64_t dpsInitIdx = 0;
  for (OpOperand &opOperand : op->getOpOperands()) {
    auto tensorCastOp = opOperand.get().getDefiningOp<tensor::CastOp>();
    bool fold = canFoldIntoConsumerOp(tensorCastOp);
    newOperands.push_back(fold ? tensorCastOp.getOperand() : opOperand.get());
    if (op.isDpsInit(&opOperand) &&
        !llvm::isa<MemRefType>(newOperands.back().getType()))
      newResTy[dpsInitIdx++] = newOperands.back().getType();
  }
  return newOperands;
}

/// Performs folding of any operand of `op` if it comes from a tensor::CastOp
/// that can be folded.
LogicalResult mlir::tensor::foldTensorCast(Operation *op) {
  bool folded = false;
  for (OpOperand &operand : op->getOpOperands()) {
    auto castOp = operand.get().getDefiningOp<tensor::CastOp>();
    if (castOp && tensor::canFoldIntoConsumerOp(castOp)) {
      operand.set(castOp.getOperand());
      folded = true;
    }
  }
  return success(folded);
}

bool CastOp::areCastCompatible(TypeRange inputs, TypeRange outputs) {
  if (inputs.size() != 1 || outputs.size() != 1)
    return false;
  Type a = inputs.front(), b = outputs.front();
  auto aT = llvm::dyn_cast<TensorType>(a);
  auto bT = llvm::dyn_cast<TensorType>(b);
  if (!aT || !bT)
    return false;

  if (aT.getElementType() != bT.getElementType())
    return false;

  return succeeded(verifyCompatibleShape(aT, bT));
}

/// Compute a TensorType that has the joined shape knowledge of the two
/// given TensorTypes. The element types need to match.
static TensorType joinShapes(TensorType one, TensorType two) {
  assert(one.getElementType() == two.getElementType());

  if (!one.hasRank())
    return two;
  if (!two.hasRank())
    return one;

  int64_t rank = one.getRank();
  if (rank != two.getRank())
    return {};

  SmallVector<int64_t, 4> join;
  join.reserve(rank);
  for (int64_t i = 0; i < rank; ++i) {
    if (one.isDynamicDim(i)) {
      join.push_back(two.getDimSize(i));
      continue;
    }
    if (two.isDynamicDim(i)) {
      join.push_back(one.getDimSize(i));
      continue;
    }
    if (one.getDimSize(i) != two.getDimSize(i))
      return {};
    join.push_back(one.getDimSize(i));
  }
  return RankedTensorType::get(join, one.getElementType());
}

namespace {

/// Replaces chains of two tensor.cast operations by a single tensor.cast
/// operation if doing so does not remove runtime constraints.
struct ChainedTensorCast : public OpRewritePattern<CastOp> {
  using OpRewritePattern<CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CastOp tensorCast,
                                PatternRewriter &rewriter) const final {
    auto tensorCastOperand = tensorCast.getOperand().getDefiningOp<CastOp>();

    if (!tensorCastOperand)
      return failure();

    auto sourceType =
        llvm::cast<TensorType>(tensorCastOperand.getOperand().getType());
    auto intermediateType = llvm::cast<TensorType>(tensorCastOperand.getType());
    auto resultType = llvm::cast<TensorType>(tensorCast.getType());

    // We can remove the intermediate cast if joining all three produces the
    // same result as just joining the source and result shapes.
    auto firstJoin =
        joinShapes(joinShapes(sourceType, intermediateType), resultType);

    // The join might not exist if the cast sequence would fail at runtime.
    if (!firstJoin)
      return failure();

    // The newJoin always exists if the above join exists, it might just contain
    // less information. If so, we cannot drop the intermediate cast, as doing
    // so would remove runtime checks.
    auto newJoin = joinShapes(sourceType, resultType);
    if (firstJoin != newJoin)
      return failure();

    rewriter.replaceOpWithNewOp<CastOp>(tensorCast, resultType,
                                        tensorCastOperand.getOperand());
    return success();
  }
};

/// Fold tensor.cast into tesor.extract_slice producer.
/// Example:
/// ```
///  %0 = tensor.extract_slice %arg0[%o, 0] [%s, 512] [1, 1] :
///    tensor<128x512xf32> to tensor<?x512xf32>
///  %1 = tensor.cast %0 : tensor<?x512xf32> to tensor<16x512xf32>
/// ```
/// ->
/// ```
/// %1 = tensor.extract_slice %arg0[%o, 0] [16, 512] [1, 1] :
///   tensor<128x512xf32> to tensor<16x512xf32>
/// ```
struct TensorCastExtractSlice : public OpRewritePattern<CastOp> {
  using OpRewritePattern<CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CastOp tensorCast,
                                PatternRewriter &rewriter) const final {
    auto extractOperand =
        tensorCast.getOperand().getDefiningOp<ExtractSliceOp>();

    // Cannot fold cast to unranked tensor.
    auto rankedResultType =
        llvm::dyn_cast<RankedTensorType>(tensorCast.getType());
    if (!rankedResultType)
      return failure();

    if (!extractOperand || !canFoldIntoProducerOp(tensorCast) ||
        rankedResultType.getShape() ==
            llvm::cast<RankedTensorType>(tensorCast.getSource().getType())
                .getShape())
      return failure();

    SmallVector<OpFoldResult, 4> sizes = extractOperand.getMixedSizes();
    auto dimMask = computeRankReductionMask(
        extractOperand.getStaticSizes(), extractOperand.getType().getShape());
    size_t dimIndex = 0;
    for (size_t i = 0, e = sizes.size(); i < e; i++) {
      if (dimMask && dimMask->count(i))
        continue;
      int64_t dim = rankedResultType.getShape()[dimIndex++];
      if (ShapedType::isDynamic(dim))
        continue;
      sizes[i] = rewriter.getIndexAttr(dim);
    }

    rewriter.replaceOpWithNewOp<ExtractSliceOp>(
        tensorCast, rankedResultType, extractOperand.getSource(),
        extractOperand.getMixedOffsets(), sizes,
        extractOperand.getMixedStrides());
    return success();
  }
};

} // namespace

void CastOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                         MLIRContext *context) {
  results.add<ChainedTensorCast, TensorCastExtractSlice>(context);
}

//===----------------------------------------------------------------------===//
// ConcatOp
//===----------------------------------------------------------------------===//

RankedTensorType ConcatOp::inferResultType(int64_t dim, TypeRange inputTypes) {
  assert(!inputTypes.empty() && "cannot concatenate 0 tensors");
  auto tensorTypes =
      llvm::to_vector<4>(llvm::map_range(inputTypes, [](Type type) {
        return llvm::cast<RankedTensorType>(type);
      }));
  int64_t concatRank = tensorTypes[0].getRank();

  // The concatenation dim must be in the range [0, rank).
  assert(dim >= 0 && dim < concatRank && "Invalid concatenation dim");

  SmallVector<int64_t> sizes(concatRank);
  for (int64_t i = 0, e = concatRank; i < e; ++i) {
    if (i == dim)
      continue;
    SaturatedInteger size;
    for (auto tensorType : tensorTypes)
      size = *size.desaturate(SaturatedInteger::wrap(tensorType.getDimSize(i)));
    sizes[i] = size.asInteger();
  }
  auto concatSize = SaturatedInteger::wrap(0);
  for (auto tensorType : tensorTypes)
    concatSize =
        concatSize + SaturatedInteger::wrap(tensorType.getDimSize(dim));
  sizes[dim] = concatSize.asInteger();
  return RankedTensorType::get(sizes, tensorTypes[0].getElementType());
}

void ConcatOp::build(OpBuilder &builder, OperationState &result, int64_t dim,
                     ValueRange inputs) {
  FailureOr<RankedTensorType> resultType =
      inferResultType(dim, inputs.getTypes());
  assert(succeeded(resultType) && "failed to infer concatenation result type");
  build(builder, result, *resultType, dim, inputs);
}

LogicalResult ConcatOp::verify() {
  if (getInputs().size() < 1)
    return emitOpError("requires at least one input");

  SmallVector<RankedTensorType> inputTypes;
  for (auto input : getInputs())
    inputTypes.push_back(cast<RankedTensorType>(input.getType()));

  RankedTensorType resultType = getResultType();
  int64_t resultRank = getRank();
  if (llvm::any_of(inputTypes, [resultRank](RankedTensorType type) {
        return type.getRank() != resultRank;
      }))
    return emitOpError("rank of concatenated inputs must match result rank");

  Type resultElementType = resultType.getElementType();
  if (llvm::any_of(inputTypes, [&](RankedTensorType type) {
        return type.getElementType() != resultElementType;
      }))
    return emitOpError("inputs and result element type must match");

  int64_t dim = getDim();
  if (dim >= resultRank)
    return emitOpError("concatenation dim must be less than the tensor rank");

  SmallVector<int64_t> sizes(resultRank);
  for (int64_t i = 0, e = resultRank; i < e; ++i) {
    if (i == dim)
      continue;
    SaturatedInteger size;
    for (auto tensorType : inputTypes) {
      FailureOr<SaturatedInteger> maybeSize =
          size.desaturate(SaturatedInteger::wrap(tensorType.getDimSize(i)));
      if (failed(maybeSize))
        return emitOpError("static concatenation size mismatch along ")
               << "non-concatenated dimension " << i;
      size = *maybeSize;
    }
    sizes[i] = size.asInteger();
  }
  auto concatSize = SaturatedInteger::wrap(0);
  for (auto tensorType : inputTypes)
    concatSize =
        concatSize + SaturatedInteger::wrap(tensorType.getDimSize(dim));
  sizes[dim] = concatSize.asInteger();
  auto inferredResultType =
      RankedTensorType::get(sizes, inputTypes[0].getElementType());

  for (auto [inferredSize, actualSize] :
       llvm::zip_equal(inferredResultType.getShape(), resultType.getShape())) {
    bool hasDynamic = ShapedType::isDynamic(inferredSize) ||
                      ShapedType::isDynamic(actualSize);
    if (!hasDynamic && inferredSize != actualSize)
      return emitOpError("result type ")
             << resultType << "does not match inferred shape "
             << inferredResultType << " static sizes";
  }

  return success();
}

FailureOr<SmallVector<Value>> ConcatOp::decomposeOperation(OpBuilder &builder) {
  size_t numInputs = getInputs().size();
  uint64_t concatDim = getDim();

  SmallVector<SmallVector<OpFoldResult>> inputShapes;
  inputShapes.reserve(numInputs);
  SmallVector<OpFoldResult> concatOffsets;
  concatOffsets.reserve(numInputs);
  SmallVector<OpFoldResult> outputShape;

  AffineExpr addExpr =
      builder.getAffineSymbolExpr(0) + builder.getAffineSymbolExpr(1);
  OpFoldResult zero = builder.getIndexAttr(0);
  Location loc = getLoc();
  for (auto [index, input] : llvm::enumerate(getInputs())) {
    SmallVector<OpFoldResult> inputShape =
        tensor::getMixedSizes(builder, input.getLoc(), input);
    if (index == 0) {
      outputShape = inputShape;
      concatOffsets.push_back(zero);
    } else {
      concatOffsets.push_back(outputShape[concatDim]);
      outputShape[concatDim] = affine::makeComposedFoldedAffineApply(
          builder, loc, addExpr,
          {outputShape[concatDim], inputShape[concatDim]});
    }
    inputShapes.emplace_back(std::move(inputShape));
  }

  Value replacement = tensor::EmptyOp::create(builder, loc, outputShape,
                                              getType().getElementType());

  int64_t rank = getType().getRank();
  OpFoldResult one = builder.getIndexAttr(1);
  SmallVector<OpFoldResult> strides(rank, one);
  SmallVector<OpFoldResult> offsets(rank, zero);
  for (auto [index, input] : llvm::enumerate(getInputs())) {
    offsets[concatDim] = concatOffsets[index];
    auto insertSlice = tensor::InsertSliceOp::create(
        builder, loc, input, replacement, offsets, inputShapes[index], strides);
    replacement = insertSlice.getResult();
  }
  if (replacement.getType() != getType()) {
    replacement = tensor::CastOp::create(builder, loc, getType(), replacement);
  }
  return SmallVector<Value>{replacement};
}

LogicalResult
ConcatOp::reifyResultShapes(OpBuilder &builder,
                            ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  ValueRange inputs = getInputs();
  int64_t dim = getDim();
  RankedTensorType inferredResultType = inferResultType(dim, inputs.getTypes());

  Value init = inputs[0];
  int64_t rank = getType().getRank();

  reifiedReturnShapes.resize(1, SmallVector<OpFoldResult>(rank));

  // Pre-populate the result sizes with as much static information as possible
  // from the given result type, as well as the inferred result type, otherwise
  // use the dim sizes from the first input.
  for (int64_t i = 0; i < rank; ++i) {
    if (i == dim)
      continue;
    if (!getType().isDynamicDim(i)) {
      reifiedReturnShapes[0][i] = builder.getIndexAttr(getType().getDimSize(i));
    } else if (!inferredResultType.isDynamicDim(i)) {
      reifiedReturnShapes[0][i] = getValueOrCreateConstantIndexOp(
          builder, getLoc(),
          builder.getIndexAttr(inferredResultType.getDimSize(i)));
    } else {
      reifiedReturnShapes[0][i] =
          tensor::DimOp::create(builder, init.getLoc(), init, i).getResult();
    }
  }

  if (getType().isDynamicDim(dim)) {
    // Take the sum of the input sizes along the concatenated dim.
    AffineExpr sum = builder.getAffineDimExpr(0);
    SmallVector<OpFoldResult> sizes = {
        builder.createOrFold<tensor::DimOp>(init.getLoc(), init, dim)};
    for (auto [idx, input] : llvm::enumerate(inputs.drop_front())) {
      sum = sum + builder.getAffineDimExpr(idx + 1);
      sizes.push_back(
          builder.createOrFold<tensor::DimOp>(input.getLoc(), input, dim));
    }
    reifiedReturnShapes[0][dim] = getValueOrCreateConstantIndexOp(
        builder, getLoc(),
        affine::makeComposedFoldedAffineApply(builder, getLoc(), sum, sizes));
  } else {
    // If the result shape is static along the concatenated dim, use the static
    // shape.
    reifiedReturnShapes[0][dim] =
        builder.getIndexAttr(getType().getDimSize(dim));
  }
  return success();
}

void ConcatOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "concat");
}

OpFoldResult ConcatOp::fold(FoldAdaptor) {
  ValueRange inputs = getInputs();
  if (inputs.size() == 1 && inputs[0].getType() == getResultType())
    return inputs[0];
  return {};
}

namespace {
/// Fold a concat op with a single input to a cast.
struct SingleInputConcatOp : public OpRewritePattern<ConcatOp> {
  using OpRewritePattern<ConcatOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ConcatOp concatOp,
                                PatternRewriter &rewriter) const override {
    if (concatOp.getInputs().size() != 1)
      return failure();
    rewriter.replaceOpWithNewOp<CastOp>(concatOp, concatOp.getResultType(),
                                        concatOp.getInputs()[0]);
    return success();
  }
};

/// Propagate static shapes into the operands of a `tensor.concat`.
///
/// `tensor.concat` requires every operand to match on all dimensions except the
/// concatenation dimension. If one operand is already static in those
/// dimensions, the other operands may safely be refined to that same static
/// shape.
///
/// Example:
///
/// ```mlir
///   %2 = tensor.concat dim(0) %0, %1: (tensor<?x12xi32>, tensor<?x?xi32>) ->
///        tensor<?x12xi32>
/// ```
/// ->
/// ```mlir
///   %cast = tensor.cast %1 : tensor<?x?xi32> to tensor<?x12xi32>
///   %2 = tensor.concat dim(0) %0, %cast :
///        (tensor<?x12xi32>, tensor<?x12xi32>) -> tensor<?x12xi32>
/// ```
struct InferConcatOperandTypes : public OpRewritePattern<ConcatOp> {
  using OpRewritePattern<ConcatOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ConcatOp concatOp,
                                PatternRewriter &rewriter) const override {
    int64_t dim = concatOp.getDim();
    RankedTensorType inferredResultType =
        ConcatOp::inferResultType(dim, concatOp->getOperandTypes());

    // Find operands for which a more static shape can be inferred.
    LogicalResult matched = failure();
    // Inferred operand shapes are identical in every dimension except the
    // concatenation dimension.
    SmallVector<int64_t> inferredOperandShape(inferredResultType.getShape());
    for (auto [operandIdx, operandType] :
         llvm::enumerate(concatOp->getOperandTypes())) {
      // Compute inferred type for operand.
      inferredOperandShape[dim] =
          cast<RankedTensorType>(operandType).getDimSize(dim);
      auto inferredOperandType = RankedTensorType::get(
          inferredOperandShape, inferredResultType.getElementType());

      // Check if inferred type is more static.
      if (!preservesStaticInformation(inferredOperandType, operandType)) {
        matched = success();

        // Use refined operand type and create cast from original operand.
        auto castOp =
            CastOp::create(rewriter, concatOp->getLoc(), inferredOperandType,
                           concatOp.getOperand(operandIdx));
        rewriter.modifyOpInPlace(concatOp, [=, operandIdx = operandIdx] {
          concatOp->setOperand(operandIdx, castOp->getResult(0));
        });
      }
    }

    return matched;
  }
};

// Ensure `tensor.concat`'s result type is at least as static as can be inferred
// from its operand types.
///
/// Example:
/// ```mlir
///   %2 = tensor.concat dim(0) %0, %1: (tensor<?x12xi32>, tensor<?x12xi32>) ->
///   tensor<?x?xi32>
/// ```
/// ->
/// ```mlir
///   %2 = tensor.concat dim(0) %0, %cast : (tensor<?x12xi32>, tensor<?x12xi32>)
///   -> tensor<?x12xi32> %cast = tensor.cast %2 : tensor<?x12xi32> to
///   tensor<?x?xi32>
/// ```
struct InferConcatResultType : public OpRewritePattern<ConcatOp> {
  using OpRewritePattern<ConcatOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ConcatOp concatOp,
                                PatternRewriter &rewriter) const override {
    int64_t dim = concatOp.getDim();
    RankedTensorType inferredResultType =
        ConcatOp::inferResultType(dim, concatOp->getOperandTypes());

    // The result type should be at least as static as inferred result type.
    if (preservesStaticInformation(inferredResultType,
                                   concatOp.getResultType())) {
      return failure();
    }

    auto newConcatOp =
        ConcatOp::create(rewriter, concatOp->getLoc(), inferredResultType, dim,
                         concatOp->getOperands());
    rewriter.replaceOpWithNewOp<CastOp>(concatOp, concatOp.getResultType(),
                                        newConcatOp);

    return success();
  }
};
} // namespace

void ConcatOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                           MLIRContext *context) {
  results
      .add<SingleInputConcatOp, InferConcatOperandTypes, InferConcatResultType>(
          context);
}

//===----------------------------------------------------------------------===//
// DimOp
//===----------------------------------------------------------------------===//

void DimOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "dim");
}

void DimOp::build(OpBuilder &builder, OperationState &result, Value source,
                  int64_t index) {
  auto loc = result.location;
  Value indexValue = arith::ConstantIndexOp::create(builder, loc, index);
  build(builder, result, source, indexValue);
}

std::optional<int64_t> DimOp::getConstantIndex() {
  return getConstantIntValue(getIndex());
}

Speculation::Speculatability DimOp::getSpeculatability() {
  auto constantIndex = getConstantIndex();
  if (!constantIndex)
    return Speculation::NotSpeculatable;

  auto rankedSourceType = dyn_cast<RankedTensorType>(getSource().getType());
  if (!rankedSourceType)
    return Speculation::NotSpeculatable;

  if (rankedSourceType.getRank() <= constantIndex)
    return Speculation::NotSpeculatable;

  return Speculation::Speculatable;
}

void DimOp::inferResultRangesFromOptional(ArrayRef<IntegerValueRange> argRanges,
                                          SetIntLatticeFn setResultRange) {
  setResultRange(getResult(),
                 intrange::inferShapedDimOpInterface(*this, argRanges[1]));
}

OpFoldResult DimOp::fold(FoldAdaptor adaptor) {
  // All forms of folding require a known index.
  auto index = llvm::dyn_cast_if_present<IntegerAttr>(adaptor.getIndex());
  if (!index)
    return {};

  // Folding for unranked types (UnrankedTensorType) is not supported.
  auto tensorType = llvm::dyn_cast<RankedTensorType>(getSource().getType());
  if (!tensorType)
    return {};

  // Out of bound indices produce undefined behavior but are still valid IR.
  // Don't choke on them.
  int64_t indexVal = index.getInt();
  if (indexVal < 0 || indexVal >= tensorType.getRank())
    return {};

  // Fold if the shape extent along the given index is known.
  if (!tensorType.isDynamicDim(index.getInt())) {
    Builder builder(getContext());
    return builder.getIndexAttr(tensorType.getShape()[index.getInt()]);
  }

  Operation *definingOp = getSource().getDefiningOp();

  // Fold dim to the operand of tensor.generate.
  if (auto fromElements = dyn_cast_or_null<tensor::GenerateOp>(definingOp)) {
    auto resultType =
        llvm::cast<RankedTensorType>(fromElements.getResult().getType());
    // The case where the type encodes the size of the dimension is handled
    // above.
    assert(ShapedType::isDynamic(resultType.getShape()[index.getInt()]));

    // Find the operand of the fromElements that corresponds to this index.
    auto dynExtents = fromElements.getDynamicExtents().begin();
    for (auto dim : resultType.getShape().take_front(index.getInt()))
      if (ShapedType::isDynamic(dim))
        dynExtents++;

    return Value{*dynExtents};
  }

  // The size at the given index is now known to be a dynamic size.
  unsigned unsignedIndex = index.getValue().getZExtValue();

  if (auto sliceOp = dyn_cast_or_null<tensor::ExtractSliceOp>(definingOp)) {
    // Fold only for non-rank reduced ops. For the rank-reduced version, rely on
    // `resolve-shaped-type-result-dims` pass.
    if (sliceOp.getType().getRank() == sliceOp.getSourceType().getRank() &&
        sliceOp.isDynamicSize(unsignedIndex)) {
      return {sliceOp.getDynamicSize(unsignedIndex)};
    }
  }

  // dim(cast) -> dim
  if (succeeded(foldTensorCast(*this)))
    return getResult();

  return {};
}

namespace {
/// Fold dim of a cast into the dim of the source of the tensor cast.
struct DimOfCastOp : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DimOp dimOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = dimOp.getSource().getDefiningOp<CastOp>();
    if (!castOp)
      return failure();
    Value newSource = castOp.getOperand();
    rewriter.replaceOpWithNewOp<DimOp>(dimOp, newSource, dimOp.getIndex());
    return success();
  }
};

/// Fold dim of a destination passing style op into the dim of the corresponding
/// init.
struct DimOfDestStyleOp : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DimOp dimOp,
                                PatternRewriter &rewriter) const override {
    auto source = dimOp.getSource();
    auto destOp = source.getDefiningOp<DestinationStyleOpInterface>();
    if (!destOp)
      return failure();

    auto resultIndex = cast<OpResult>(source).getResultNumber();
    auto *initOperand = destOp.getDpsInitOperand(resultIndex);

    rewriter.modifyOpInPlace(
        dimOp, [&]() { dimOp.getSourceMutable().assign(initOperand->get()); });
    return success();
  }
};

/// Fold dim of a tensor reshape operation to a extract into the reshape's shape
/// operand.
struct DimOfReshapeOp : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DimOp dim,
                                PatternRewriter &rewriter) const override {
    auto reshape = dim.getSource().getDefiningOp<ReshapeOp>();

    if (!reshape)
      return failure();

    // Since tensors are immutable we don't need to worry about where to place
    // the extract call
    rewriter.setInsertionPointAfter(dim);
    Location loc = dim.getLoc();
    Value extract =
        ExtractOp::create(rewriter, loc, reshape.getShape(), dim.getIndex());
    if (extract.getType() != dim.getType())
      extract =
          arith::IndexCastOp::create(rewriter, loc, dim.getType(), extract);
    rewriter.replaceOp(dim, extract);
    return success();
  }
};
} // namespace

void DimOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                        MLIRContext *context) {
  results.add<DimOfCastOp, DimOfDestStyleOp, DimOfReshapeOp>(context);
}

//===----------------------------------------------------------------------===//
// EmptyOp
//===----------------------------------------------------------------------===//

void EmptyOp::build(OpBuilder &builder, OperationState &result,
                    ArrayRef<int64_t> staticShape, Type elementType,
                    Attribute encoding) {
  assert(none_of(staticShape, ShapedType::isDynamic) &&
         "expected only static sizes");
  build(builder, result, staticShape, elementType, ValueRange{}, encoding);
}

void EmptyOp::build(OpBuilder &builder, OperationState &result,
                    ArrayRef<int64_t> staticShape, Type elementType,
                    ValueRange dynamicSizes, Attribute encoding) {
  auto tensorType = RankedTensorType::get(staticShape, elementType, encoding);
  build(builder, result, tensorType, dynamicSizes);
}

void EmptyOp::build(OpBuilder &builder, OperationState &result,
                    ArrayRef<OpFoldResult> sizes, Type elementType,
                    Attribute encoding) {
  SmallVector<int64_t> staticShape;
  SmallVector<Value> dynamicSizes;
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticShape);
  build(builder, result, staticShape, elementType, dynamicSizes, encoding);
}

LogicalResult EmptyOp::verify() {
  if (getType().getNumDynamicDims() != getDynamicSizes().size())
    return emitOpError("incorrect number of dynamic sizes, has ")
           << getDynamicSizes().size() << ", expected "
           << getType().getNumDynamicDims();
  return success();
}

LogicalResult
EmptyOp::reifyResultShapes(OpBuilder &builder,
                           ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<OpFoldResult>(getType().getRank()));
  unsigned ctr = 0;
  for (int64_t i = 0; i < getType().getRank(); ++i) {
    if (getType().isDynamicDim(i)) {
      reifiedReturnShapes[0][i] = getDynamicSizes()[ctr++];
    } else {
      reifiedReturnShapes[0][i] = builder.getIndexAttr(getType().getDimSize(i));
    }
  }
  return success();
}

Value EmptyOp::getDynamicSize(unsigned idx) {
  assert(getType().isDynamicDim(idx) && "expected dynamic dim");
  unsigned ctr = 0;
  for (int64_t i = 0; i < static_cast<int64_t>(idx); ++i)
    if (getType().isDynamicDim(i))
      ++ctr;
  return getDynamicSizes()[ctr];
}

SmallVector<OpFoldResult> EmptyOp::getMixedSizes() {
  SmallVector<OpFoldResult> result;
  unsigned ctr = 0;
  OpBuilder b(getContext());
  for (int64_t i = 0; i < getType().getRank(); ++i) {
    if (getType().isDynamicDim(i)) {
      result.push_back(getDynamicSizes()[ctr++]);
    } else {
      result.push_back(b.getIndexAttr(getType().getShape()[i]));
    }
  }
  return result;
}

namespace {
/// Change the type of the result of a `tensor.empty` by making the result
/// type statically sized along dimensions that in the original operation were
/// defined as dynamic, but the size was defined using a `constant` op. For
/// example
///
///  %c5 = arith.constant 5: index
///  %0 = tensor.empty(%arg0, %c5) : tensor<?x?xf32>
///
///  to
///
///  %0 = tensor.empty(%arg0) : tensor<?x5xf32>
struct ReplaceEmptyTensorStaticShapeDims : OpRewritePattern<EmptyOp> {
  using OpRewritePattern<EmptyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(EmptyOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value> foldedDynamicSizes;
    RankedTensorType foldedTensorType = foldDynamicToStaticDimSizes(
        op.getType(), op.getDynamicSizes(), foldedDynamicSizes);

    // Stop here if no dynamic size was promoted to static.
    if (foldedTensorType == op.getType())
      return failure();

    auto newOp = EmptyOp::create(rewriter, op.getLoc(), foldedTensorType,
                                 foldedDynamicSizes);
    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, op.getType(), newOp);
    return success();
  }
};

struct FoldEmptyTensorWithDimOp : public OpRewritePattern<DimOp> {
  using OpRewritePattern<DimOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::DimOp dimOp,
                                PatternRewriter &rewriter) const override {
    std::optional<int64_t> maybeConstantIndex = dimOp.getConstantIndex();
    auto emptyTensorOp = dimOp.getSource().getDefiningOp<EmptyOp>();
    if (!emptyTensorOp || !maybeConstantIndex)
      return failure();
    auto emptyTensorType = emptyTensorOp.getType();
    if (*maybeConstantIndex < 0 ||
        *maybeConstantIndex >= emptyTensorType.getRank() ||
        !emptyTensorType.isDynamicDim(*maybeConstantIndex))
      return failure();
    rewriter.replaceOp(dimOp,
                       emptyTensorOp.getDynamicSize(*maybeConstantIndex));
    return success();
  }
};

/// Canonicalize
///
/// ```mlir
///   %0 = tensor.empty(%d0, %d1) : tensor<?x?xf32>
///   %1 = tensor.cast %0 : tensor<?x?xf32> to tensor<4x?xf32>
/// ```
///
/// into
///
/// ```mlir
///   %0 = tensor.empty(%d1) : tensor<4x?xf32>
/// ```
///
/// This assumes the input program is correct in terms of its shape. So it is
/// safe to assume that `%d0` is in fact 4.
struct FoldEmptyTensorWithCastOp : public OpRewritePattern<CastOp> {
  using OpRewritePattern<CastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CastOp castOp,
                                PatternRewriter &rewriter) const override {
    if (!canFoldIntoProducerOp(castOp))
      return failure();
    auto producer = castOp.getSource().getDefiningOp<EmptyOp>();
    if (!producer)
      return failure();

    auto resultType =
        llvm::cast<RankedTensorType>(castOp->getResult(0).getType());
    ArrayRef<int64_t> resultShape = resultType.getShape();
    SmallVector<OpFoldResult> currMixedSizes = producer.getMixedSizes();
    SmallVector<OpFoldResult> newMixedSizes;
    newMixedSizes.reserve(currMixedSizes.size());
    assert(resultShape.size() == currMixedSizes.size() &&
           "mismatch in result shape and sizes of empty op");
    for (auto it : llvm::zip(resultShape, currMixedSizes)) {
      int64_t newDim = std::get<0>(it);
      OpFoldResult currDim = std::get<1>(it);
      // Case 1: The empty tensor dim is static. Check that the tensor cast
      // result dim matches.
      if (auto attr = llvm::dyn_cast_if_present<Attribute>(currDim)) {
        if (ShapedType::isDynamic(newDim) ||
            newDim != llvm::cast<IntegerAttr>(attr).getInt()) {
          // Something is off, the cast result shape cannot be more dynamic
          // than the empty tensor result shape (enforced by
          // `canFoldIntoProducer`). Abort for now.
          return rewriter.notifyMatchFailure(
              producer, "mismatch in static value of shape of empty tensor "
                        "result and cast result");
        }
        newMixedSizes.push_back(attr);
        continue;
      }

      // Case 2 : The tensor cast shape is static, but empty tensor result
      // shape is dynamic.
      if (ShapedType::isStatic(newDim)) {
        newMixedSizes.push_back(rewriter.getIndexAttr(newDim));
        continue;
      }

      // Case 3 : The tensor cast shape is dynamic and empty tensor result
      // shape is dynamic. Use the dynamic value from the empty tensor op.
      newMixedSizes.push_back(currDim);
    }

    // TODO: Do not drop tensor encoding.
    rewriter.replaceOpWithNewOp<EmptyOp>(castOp, newMixedSizes,
                                         resultType.getElementType());
    return success();
  }
};

} // namespace

void EmptyOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                          MLIRContext *context) {
  results.add<FoldEmptyTensorWithCastOp, FoldEmptyTensorWithDimOp,
              ReplaceEmptyTensorStaticShapeDims>(context);
}

//===----------------------------------------------------------------------===//
// ExtractOp
//===----------------------------------------------------------------------===//

namespace {

/// Canonicalizes the pattern of the form
///
/// %val = tensor.cast %source : : tensor<?xi32> to tensor<2xi32>
/// %extracted_element = tensor.extract %val[%c0] : tensor<2xi32>
///
/// to
///
/// %extracted_element = tensor.extract %source[%c0] : tensor<?xi32>
struct ExtractFromTensorCast : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extract,
                                PatternRewriter &rewriter) const final {
    auto tensorCast = extract.getTensor().getDefiningOp<tensor::CastOp>();
    if (!tensorCast)
      return failure();
    if (!llvm::isa<RankedTensorType>(tensorCast.getSource().getType()))
      return failure();
    rewriter.replaceOpWithNewOp<tensor::ExtractOp>(
        extract, tensorCast.getSource(), extract.getIndices());
    return success();
  }
};

/// Canonicalizes the pattern of the form
///
/// %val = tensor.collapse_shape %src[[0, 1]] : tensor<3x4xf64> into
/// tensor<12xf64>
/// %extracted_element = tensor.extract %val[%c10] :
/// tensor<12xf64>
///
/// to
///
/// %extracted_element = tensor.extract %src[%c2, %c2] : tensor<3x4xf64>
struct ExtractFromCollapseShape : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extractOp,
                                PatternRewriter &rewriter) const final {
    auto collapseOp =
        extractOp.getTensor().getDefiningOp<tensor::CollapseShapeOp>();
    if (!collapseOp)
      return failure();
    if (!collapseOp.getSrcType().hasStaticShape())
      return failure();

    auto sourceSizes = collapseOp.getSrcType().getShape();

    SmallVector<Value> indices(extractOp.getIndices().begin(),
                               extractOp.getIndices().end());
    SmallVector<Value> sourceIndices;
    for (auto [index, group] :
         llvm::zip(indices, collapseOp.getReassociationIndices())) {
      assert(!group.empty() && "association indices groups cannot be empty");
      auto groupSize = group.size();

      if (groupSize == 1) {
        sourceIndices.push_back(index);
        continue;
      }

      SmallVector<int64_t> basis =
          llvm::map_to_vector(group, [&](int64_t d) { return sourceSizes[d]; });
      auto delinearize = affine::AffineDelinearizeIndexOp::create(
          rewriter, extractOp.getLoc(), index, basis, /*hasOuterBound=*/true);
      llvm::append_range(sourceIndices, delinearize.getResults());
    }
    if (collapseOp.getReassociationIndices().empty()) {
      auto zeroAffineMap = rewriter.getConstantAffineMap(0);
      int64_t srcRank =
          cast<RankedTensorType>(collapseOp.getSrcType()).getRank();
      OpFoldResult ofr = affine::makeComposedFoldedAffineApply(
          rewriter, extractOp.getLoc(), zeroAffineMap,
          ArrayRef<OpFoldResult>{});
      for (int64_t i = 0; i < srcRank; i++) {
        sourceIndices.push_back(
            getValueOrCreateConstantIndexOp(rewriter, extractOp.getLoc(), ofr));
      }
    }

    rewriter.replaceOpWithNewOp<tensor::ExtractOp>(
        extractOp, collapseOp.getSrc(), sourceIndices);
    return success();
  }
};

} // namespace

void ExtractOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "extracted");
}

LogicalResult ExtractOp::verify() {
  // Verify the # indices match if we have a ranked type.
  auto tensorType = llvm::cast<RankedTensorType>(getTensor().getType());
  if (tensorType.getRank() != static_cast<int64_t>(getIndices().size()))
    return emitOpError("incorrect number of indices for extract_element");
  return success();
}

/// If we have an ExtractOp consuming an InsertOp with the same
/// indices, we can return the InsertOp's scalar directly.
// TODO: This only checks the immediate producer; extend to go up the
// insert/extract chain if the slices are disjoint.
static Value foldExtractAfterInsert(ExtractOp extractOp) {
  auto insertOp = extractOp.getTensor().getDefiningOp<InsertOp>();

  auto isSame = [](Value a, Value b) {
    return getAsOpFoldResult(a) == getAsOpFoldResult(b);
  };
  if (insertOp && insertOp.getScalar().getType() == extractOp.getType() &&
      llvm::equal(insertOp.getIndices(), extractOp.getIndices(), isSame))
    return insertOp.getScalar();

  return {};
}

OpFoldResult ExtractOp::fold(FoldAdaptor adaptor) {
  if (Attribute tensor = adaptor.getTensor()) {
    // If this is a splat elements attribute, simply return the value.
    // All of the elements of a splat attribute are the same.
    if (auto splatTensor = llvm::dyn_cast<SplatElementsAttr>(tensor))
      return splatTensor.getSplatValue<Attribute>();

    // If this is a dense resource elements attribute, return.
    if (isa<DenseResourceElementsAttr>(tensor))
      return {};
  }

  // Collect the constant indices into the tensor.
  SmallVector<uint64_t, 8> indices;
  for (Attribute indice : adaptor.getIndices()) {
    if (!indice || !llvm::isa<IntegerAttr>(indice))
      return {};
    indices.push_back(llvm::cast<IntegerAttr>(indice).getInt());
  }

  // Fold extract(from_elements(...)).
  if (auto fromElementsOp = getTensor().getDefiningOp<FromElementsOp>()) {
    auto tensorType = llvm::cast<RankedTensorType>(fromElementsOp.getType());
    auto rank = tensorType.getRank();
    assert(static_cast<int64_t>(indices.size()) == tensorType.getRank() &&
           "rank mismatch");
    int flatIndex = 0;
    int stride = 1;
    for (int i = rank - 1; i >= 0; --i) {
      flatIndex += indices[i] * stride;
      stride *= tensorType.getDimSize(i);
    }
    // Prevent out of bounds accesses. This can happen in invalid code that
    // will never execute.
    if (static_cast<int>(fromElementsOp.getElements().size()) <= flatIndex ||
        flatIndex < 0)
      return {};
    return fromElementsOp.getElements()[flatIndex];
  }

  // If this is an elements attribute, query the value at the given indices.
  if (Attribute tensor = adaptor.getTensor()) {
    auto elementsAttr = llvm::dyn_cast<ElementsAttr>(tensor);
    if (elementsAttr && elementsAttr.isValidIndex(indices))
      return elementsAttr.getValues<Attribute>()[indices];
  }

  if (Value result = foldExtractAfterInsert(*this))
    return result;

  return {};
}

void ExtractOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                            MLIRContext *context) {
  results.add<ExtractFromTensorCast>(context);
}

void mlir::tensor::populateFoldCollapseExtractPatterns(
    RewritePatternSet &patterns) {
  patterns.add<ExtractFromCollapseShape>(patterns.getContext());
}

//===----------------------------------------------------------------------===//
// FromElementsOp
//===----------------------------------------------------------------------===//

void FromElementsOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "from_elements");
}

void FromElementsOp::build(OpBuilder &builder, OperationState &result,
                           ValueRange elements) {
  assert(!elements.empty() && "expected at least one element");
  Type resultType = RankedTensorType::get(
      {static_cast<int64_t>(elements.size())}, elements.front().getType());
  build(builder, result, resultType, elements);
}

OpFoldResult FromElementsOp::fold(FoldAdaptor adaptor) {
  if (!llvm::is_contained(adaptor.getElements(), nullptr))
    return DenseElementsAttr::get(getType(), adaptor.getElements());
  return {};
}

namespace {

// Pushes the index_casts that occur before extractions to after the extract.
// This minimizes type conversion in some cases and enables the extract
// canonicalizer. This changes:
//
// %cast = arith.index_cast %tensor : tensor<1xi32> to tensor<1xindex>
// %extract = tensor.extract %cast[%index] : tensor<1xindex>
//
// to the following:
//
// %extract = tensor.extract %tensor[%index] : tensor<1xindex>
// %cast = arith.index_cast %extract : i32 to index
//
// to just %element.
//
// Consider expanding this to a template and handle all tensor cast
// operations.
struct ExtractElementFromIndexCast
    : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extract,
                                PatternRewriter &rewriter) const final {
    Location loc = extract.getLoc();
    auto indexCast = extract.getTensor().getDefiningOp<arith::IndexCastOp>();
    if (!indexCast)
      return failure();

    Type elementTy = getElementTypeOrSelf(indexCast.getIn());

    auto newExtract = tensor::ExtractOp::create(
        rewriter, loc, elementTy, indexCast.getIn(), extract.getIndices());

    rewriter.replaceOpWithNewOp<arith::IndexCastOp>(extract, extract.getType(),
                                                    newExtract);

    return success();
  }
};

} // namespace

void FromElementsOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  results.add<ExtractElementFromIndexCast>(context);
}

//===----------------------------------------------------------------------===//
// GatherOp
//===----------------------------------------------------------------------===//

void GatherOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "gather");
}

/// Return the inferred result type for a gatherOp where:
///   - sourceType is the type of the source tensor gathered from
///   - indicesType is the type of the indices used to gather
///   - gatherDims are the dims along which the gather occurs.
/// Return a full rank or ranked-reduced variant of the type depending on
/// the value of rankReduced.
///
/// The leading dimensions of the index tensor give the result tensor its
/// leading dimensions.
/// The trailing dimensions of the result tensor are obtained from the source
/// tensor by setting the dimensions specified in gather_dims to `1` (if
/// rankedReduced is false), or skipping them (otherwise).
RankedTensorType GatherOp::inferResultType(RankedTensorType sourceType,
                                           RankedTensorType indicesType,
                                           ArrayRef<int64_t> gatherDims,
                                           bool rankReduced) {
  SmallVector<int64_t> resultShape(indicesType.getShape().drop_back());
  resultShape.reserve(resultShape.size() + sourceType.getRank());
  for (int64_t idx : llvm::seq<int64_t>(0, sourceType.getRank())) {
    if (llvm::binary_search(gatherDims, idx)) {
      if (!rankReduced)
        resultShape.push_back(1);
      continue;
    }
    resultShape.push_back(sourceType.getDimSize(idx));
  }
  return RankedTensorType::Builder(sourceType).setShape(resultShape);
}

static LogicalResult
verifyGatherOrScatterDims(Operation *op, ArrayRef<int64_t> dims,
                          ArrayRef<int64_t> indices, int64_t rank,
                          StringRef gatherOrScatter, StringRef sourceOrDest) {
  if (dims.empty())
    return op->emitOpError(gatherOrScatter) << "_dims must be non-empty";

  int64_t numGatherDims = dims.size();
  if (numGatherDims > rank)
    return op->emitOpError(gatherOrScatter)
           << "_dims overflow " << sourceOrDest << " rank";
  if (indices.empty() || indices.back() != numGatherDims)
    return op->emitOpError(gatherOrScatter)
           << "_dims length must match the size of last dimension of indices";
  for (int64_t val : dims) {
    if (val < 0)
      return op->emitOpError(gatherOrScatter)
             << "_dims value must be non-negative";
    if (val >= rank)
      return op->emitOpError(gatherOrScatter)
             << "_dims value must be smaller than " << sourceOrDest << " rank";
  }
  for (int64_t i = 1; i < numGatherDims; ++i) {
    if (dims[i - 1] >= dims[i])
      return op->emitOpError(gatherOrScatter)
             << "_dims values must be strictly increasing";
  }
  return success();
}

LogicalResult GatherOp::verify() {
  int64_t sourceRank = getSourceType().getRank();
  ArrayRef<int64_t> gatherDims = getGatherDims();
  if (failed(verifyGatherOrScatterDims(getOperation(), gatherDims,
                                       getIndicesType().getShape(), sourceRank,
                                       "gather", "source")))
    return failure();

  RankedTensorType expectedResultType = GatherOp::inferResultType(
      getSourceType(), getIndicesType(), gatherDims, /*rankReduced=*/false);
  RankedTensorType expectedRankReducedResultType = GatherOp::inferResultType(
      getSourceType(), getIndicesType(), gatherDims, /*rankReduced=*/true);
  if (getResultType() != expectedResultType &&
      getResultType() != expectedRankReducedResultType) {
    return emitOpError("result type "
                       "mismatch: "
                       "expected ")
           << expectedResultType << " or its rank-reduced variant "
           << expectedRankReducedResultType << " (got: " << getResultType()
           << ")";
  }

  return success();
}

OpFoldResult GatherOp::fold(FoldAdaptor adaptor) {
  if (OpFoldResult reshapedSource = reshapeConstantSource(
          llvm::dyn_cast_if_present<DenseElementsAttr>(adaptor.getSource()),
          getResult().getType()))
    return reshapedSource;
  return {};
}

//===----------------------------------------------------------------------===//
// InsertOp
//===----------------------------------------------------------------------===//

void InsertOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "inserted");
}

LogicalResult InsertOp::verify() {
  // Verify the # indices match if we have a ranked type.
  auto destType = llvm::cast<RankedTensorType>(getDest().getType());
  if (destType.getRank() != static_cast<int64_t>(getIndices().size()))
    return emitOpError("incorrect number of indices");
  return success();
}

OpFoldResult InsertOp::fold(FoldAdaptor adaptor) {
  Attribute scalar = adaptor.getScalar();
  Attribute dest = adaptor.getDest();
  if (scalar && dest)
    if (auto splatDest = llvm::dyn_cast<SplatElementsAttr>(dest))
      if (scalar == splatDest.getSplatValue<Attribute>())
        return dest;
  return {};
}

//===----------------------------------------------------------------------===//
// GenerateOp
//===----------------------------------------------------------------------===//

void GenerateOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "generated");
}

LogicalResult GenerateOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<OpFoldResult>(getType().getRank()));
  int idx = 0;
  for (auto dim : llvm::seq<int64_t>(0, getType().getRank())) {
    if (getType().isDynamicDim(dim)) {
      reifiedReturnShapes[0][dim] = getOperand(idx++);
    } else {
      reifiedReturnShapes[0][dim] =
          builder.getIndexAttr(getType().getDimSize(dim));
    }
  }
  return success();
}

LogicalResult GenerateOp::verify() {
  // Ensure that the tensor type has as many dynamic dimensions as are
  // specified by the operands.
  RankedTensorType resultType = llvm::cast<RankedTensorType>(getType());
  if (getNumOperands() != resultType.getNumDynamicDims())
    return emitError("must have as many index operands as dynamic extents "
                     "in the result type");
  return success();
}

LogicalResult GenerateOp::verifyRegions() {
  RankedTensorType resultTy = llvm::cast<RankedTensorType>(getType());
  // Ensure that region arguments span the index space.
  if (!llvm::all_of(getBody().getArgumentTypes(),
                    [](Type ty) { return ty.isIndex(); }))
    return emitError("all body arguments must be index");
  if (getBody().getNumArguments() != resultTy.getRank())
    return emitError("must have one body argument per input dimension");

  // Ensure that the region yields an element of the right type.
  auto yieldOp = cast<YieldOp>(getBody().getBlocks().front().getTerminator());

  if (yieldOp.getValue().getType() != resultTy.getElementType())
    return emitOpError(
        "body must be terminated with a `yield` operation of the tensor "
        "element type");

  return success();
}

void GenerateOp::build(
    OpBuilder &b, OperationState &result, Type resultTy,
    ValueRange dynamicExtents,
    function_ref<void(OpBuilder &, Location, ValueRange)> bodyBuilder) {
  build(b, result, resultTy, dynamicExtents);

  // Build and populate body.
  OpBuilder::InsertionGuard guard(b);
  Region *bodyRegion = result.regions.front().get();
  auto rank = llvm::cast<RankedTensorType>(resultTy).getRank();
  SmallVector<Type, 2> argumentTypes(rank, b.getIndexType());
  SmallVector<Location, 2> argumentLocs(rank, result.location);
  Block *bodyBlock =
      b.createBlock(bodyRegion, bodyRegion->end(), argumentTypes, argumentLocs);
  bodyBuilder(b, result.location, bodyBlock->getArguments());
}

namespace {

/// Canonicalizes tensor.generate operations with a constant
/// operand into the equivalent operation with the operand expressed in the
/// result type, instead. We also insert a type cast to make sure that the
/// resulting IR is still well-typed.
struct StaticTensorGenerate : public OpRewritePattern<GenerateOp> {
  using OpRewritePattern<GenerateOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(GenerateOp generateOp,
                                PatternRewriter &rewriter) const final {
    SmallVector<Value> foldedDynamicSizes;
    RankedTensorType foldedTensorType = foldDynamicToStaticDimSizes(
        generateOp.getType(), generateOp.getDynamicExtents(),
        foldedDynamicSizes);

    // Stop here if no dynamic size was promoted to static.
    if (foldedTensorType == generateOp.getType())
      return failure();

    auto loc = generateOp.getLoc();
    auto newOp =
        GenerateOp::create(rewriter, loc, foldedTensorType, foldedDynamicSizes);
    rewriter.inlineRegionBefore(generateOp.getBody(), newOp.getBody(),
                                newOp.getBody().begin());
    rewriter.replaceOpWithNewOp<tensor::CastOp>(generateOp,
                                                generateOp.getType(), newOp);
    return success();
  }
};

/// Canonicalizes the pattern of the form
///
/// %tensor = tensor.generate %x {
///   ^bb0(%arg0: index):
///   <computation>
///   yield %1 : index
/// } : tensor<?xindex>
/// %extracted_element = tensor.extract %tensor[%c0] : tensor<?xi32>
///
/// to just <computation> with %arg0 replaced by %c0. We only do this if the
/// tensor.generate operation has no side-effects.
struct ExtractFromTensorGenerate : public OpRewritePattern<tensor::ExtractOp> {
  using OpRewritePattern<tensor::ExtractOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractOp extract,
                                PatternRewriter &rewriter) const final {
    auto tensorFromElements = extract.getTensor().getDefiningOp<GenerateOp>();
    if (!tensorFromElements || !wouldOpBeTriviallyDead(tensorFromElements))
      return failure();

    IRMapping mapping;
    Block *body = &tensorFromElements.getBody().front();
    mapping.map(body->getArguments(), extract.getIndices());
    for (auto &op : body->without_terminator())
      rewriter.clone(op, mapping);

    auto yield = cast<YieldOp>(body->getTerminator());

    rewriter.replaceOp(extract, mapping.lookupOrDefault(yield.getValue()));
    return success();
  }
};

} // namespace

void GenerateOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                             MLIRContext *context) {
  // TODO: Move extract pattern to tensor::ExtractOp.
  results.add<ExtractFromTensorGenerate, StaticTensorGenerate>(context);
}

//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

void RankOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "rank");
}

OpFoldResult RankOp::fold(FoldAdaptor adaptor) {
  // Constant fold rank when the rank of the operand is known.
  auto type = getOperand().getType();
  auto shapedType = llvm::dyn_cast<ShapedType>(type);
  if (shapedType && shapedType.hasRank())
    return IntegerAttr::get(IndexType::get(getContext()), shapedType.getRank());
  return IntegerAttr();
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

void ReshapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "reshape");
}

static int64_t getNumElements(ShapedType type) {
  int64_t numElements = 1;
  for (auto dim : type.getShape())
    numElements *= dim;
  return numElements;
}

LogicalResult ReshapeOp::verify() {
  TensorType operandType = llvm::cast<TensorType>(getSource().getType());
  TensorType resultType = llvm::cast<TensorType>(getResult().getType());

  if (operandType.getElementType() != resultType.getElementType())
    return emitOpError("element types of source and destination tensor "
                       "types should be the same");

  int64_t shapeSize =
      llvm::cast<RankedTensorType>(getShape().getType()).getDimSize(0);
  auto resultRankedType = llvm::dyn_cast<RankedTensorType>(resultType);
  auto operandRankedType = llvm::dyn_cast<RankedTensorType>(operandType);

  if (resultRankedType) {
    if (operandRankedType && resultRankedType.hasStaticShape() &&
        operandRankedType.hasStaticShape()) {
      if (getNumElements(operandRankedType) != getNumElements(resultRankedType))
        return emitOpError("source and destination tensor should have the "
                           "same number of elements");
    }
    if (ShapedType::isDynamic(shapeSize))
      return emitOpError("cannot use shape operand with dynamic length to "
                         "reshape to statically-ranked tensor type");
    if (shapeSize != resultRankedType.getRank())
      return emitOpError(
          "length of shape operand differs from the result's tensor rank");
  }
  return success();
}

OpFoldResult ReshapeOp::fold(FoldAdaptor adaptor) {
  if (OpFoldResult reshapedSource = reshapeConstantSource(
          llvm::dyn_cast_if_present<DenseElementsAttr>(adaptor.getSource()),
          getResult().getType()))
    return reshapedSource;

  // If the producer of operand 'source' is another 'tensor.reshape' op, use the
  // producer's input instead as the original tensor to reshape. This could
  // render such producer dead code.
  if (auto reshapeOpProducer = getSource().getDefiningOp<ReshapeOp>()) {
    getSourceMutable().assign(reshapeOpProducer.getSource());
    return getResult();
  }

  auto source = getSource();
  auto sourceTy = dyn_cast<RankedTensorType>(source.getType());
  auto resultTy = dyn_cast<RankedTensorType>(getType());
  if (!sourceTy || !resultTy || sourceTy != resultTy)
    return {};

  // If the source and result are both 0D or 1D tensors and have the same type,
  // the reshape has no effect, even if the tensor is dynamically shaped.
  if (sourceTy.getRank() <= 1)
    return source;

  if (auto fromElements = getShape().getDefiningOp<tensor::FromElementsOp>()) {
    auto elements = fromElements.getElements();
    bool dynamicNoop =
        sourceTy.getRank() == static_cast<int64_t>(elements.size());
    for (int id = 0, s = elements.size(); id < s && dynamicNoop; ++id) {
      auto element = elements[id];

      if (auto cst = getConstantIntValue(element)) {
        dynamicNoop &= cst.value() == sourceTy.getDimSize(id);
        continue;
      }

      if (auto dimOp = element.getDefiningOp<tensor::DimOp>()) {
        dynamicNoop &= dimOp.getSource() == source;

        auto cst = getConstantIntValue(dimOp.getIndex());
        dynamicNoop &=
            cst.has_value() && cst.value() == static_cast<int64_t>(id);
        continue;
      }

      dynamicNoop = false;
      break;
    }

    if (dynamicNoop)
      return source;
  }

  return {};
}

//===----------------------------------------------------------------------===//
// Reassociative reshape ops
//===----------------------------------------------------------------------===//

void CollapseShapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "collapsed");
}

void ExpandShapeOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "expanded");
}

int64_t ExpandShapeOp::getCorrespondingSourceDim(int64_t resultDim) {
  assert(resultDim >= 0 && resultDim < getResultType().getRank() &&
         "invalid resultDim");
  for (const auto &it : llvm::enumerate(getReassociationIndices()))
    if (llvm::is_contained(it.value(), resultDim))
      return it.index();
  llvm_unreachable("could not find reassociation group");
}

FailureOr<SmallVector<OpFoldResult>>
ExpandShapeOp::inferOutputShape(OpBuilder &b, Location loc,
                                RankedTensorType expandedType,
                                ArrayRef<ReassociationIndices> reassociation,
                                ArrayRef<OpFoldResult> inputShape) {
  std::optional<SmallVector<OpFoldResult>> outputShape =
      inferExpandShapeOutputShape(b, loc, expandedType, reassociation,
                                  inputShape);
  if (!outputShape)
    return failure();
  return *outputShape;
}

SmallVector<OpFoldResult> ExpandShapeOp::getMixedOutputShape() {
  return getMixedValues(getStaticOutputShape(), getOutputShape(), getContext());
}

void ExpandShapeOp::build(OpBuilder &builder, OperationState &result,
                          Type resultType, Value src,
                          ArrayRef<ReassociationIndices> reassociation,
                          ArrayRef<OpFoldResult> outputShape) {
  auto [staticOutputShape, dynamicOutputShape] =
      decomposeMixedValues(SmallVector<OpFoldResult>(outputShape));
  build(builder, result, cast<RankedTensorType>(resultType), src,
        getReassociationIndicesAttribute(builder, reassociation),
        dynamicOutputShape, staticOutputShape);
}

void ExpandShapeOp::build(OpBuilder &builder, OperationState &result,
                          Type resultType, Value src,
                          ArrayRef<ReassociationIndices> reassociation) {
  SmallVector<OpFoldResult> inputShape =
      getMixedSizes(builder, result.location, src);
  auto tensorResultTy = cast<RankedTensorType>(resultType);
  FailureOr<SmallVector<OpFoldResult>> outputShape = inferOutputShape(
      builder, result.location, tensorResultTy, reassociation, inputShape);
  SmallVector<OpFoldResult> outputShapeOrEmpty;
  if (succeeded(outputShape)) {
    outputShapeOrEmpty = *outputShape;
  }
  build(builder, result, tensorResultTy, src, reassociation,
        outputShapeOrEmpty);
}

SmallVector<AffineMap, 4> CollapseShapeOp::getReassociationMaps() {
  return getSymbolLessAffineMaps(getReassociationExprs());
}
SmallVector<ReassociationExprs, 4> CollapseShapeOp::getReassociationExprs() {
  return convertReassociationIndicesToExprs(getContext(),
                                            getReassociationIndices());
}

SmallVector<AffineMap, 4> ExpandShapeOp::getReassociationMaps() {
  return getSymbolLessAffineMaps(getReassociationExprs());
}
SmallVector<ReassociationExprs, 4> ExpandShapeOp::getReassociationExprs() {
  return convertReassociationIndicesToExprs(getContext(),
                                            getReassociationIndices());
}

RankedTensorType CollapseShapeOp::inferCollapsedType(
    RankedTensorType type, SmallVector<ReassociationIndices> reassociation) {
  return inferCollapsedType(
      type, getSymbolLessAffineMaps(convertReassociationIndicesToExprs(
                type.getContext(), reassociation)));
}

/// Compute the RankedTensorType obtained by applying `reassociation` to
/// `type`.
RankedTensorType
CollapseShapeOp::inferCollapsedType(RankedTensorType type,
                                    ArrayRef<AffineMap> reassociation) {
  auto shape = type.getShape();
  SmallVector<int64_t, 4> newShape;
  newShape.reserve(reassociation.size());

  // Use the fact that reassociation is valid to simplify the logic: only use
  // each map's rank.
  assert(isReassociationValid(reassociation) && "invalid reassociation");
  unsigned currentDim = 0;
  for (AffineMap m : reassociation) {
    unsigned dim = m.getNumResults();
    auto band = shape.slice(currentDim, dim);
    int64_t size = 1;
    if (llvm::is_contained(band, ShapedType::kDynamic))
      size = ShapedType::kDynamic;
    else
      for (unsigned d = 0; d < dim; ++d)
        size *= shape[currentDim + d];
    newShape.push_back(size);
    currentDim += dim;
  }

  return RankedTensorType::get(newShape, type.getElementType());
}

void CollapseShapeOp::build(OpBuilder &b, OperationState &result, Value src,
                            ArrayRef<ReassociationIndices> reassociation,
                            ArrayRef<NamedAttribute> attrs) {
  auto resultType = inferCollapsedType(
      llvm::cast<RankedTensorType>(src.getType()),
      getSymbolLessAffineMaps(
          convertReassociationIndicesToExprs(b.getContext(), reassociation)));
  result.addAttribute(getReassociationAttrStrName(),
                      getReassociationIndicesAttribute(b, reassociation));
  build(b, result, resultType, src, attrs);
}

template <typename TensorReshapeOp, bool isExpansion = std::is_same<
                                        TensorReshapeOp, ExpandShapeOp>::value>
static LogicalResult verifyTensorReshapeOp(TensorReshapeOp op,
                                           RankedTensorType expandedType,
                                           RankedTensorType collapsedType) {
  if (failed(
          verifyReshapeLikeTypes(op, expandedType, collapsedType, isExpansion)))
    return failure();

  auto maps = op.getReassociationMaps();
  RankedTensorType expectedType =
      CollapseShapeOp::inferCollapsedType(expandedType, maps);
  if (!isSameTypeWithoutEncoding(collapsedType, expectedType))
    return op.emitOpError("expected collapsed type to be ")
           << expectedType << ", but got " << collapsedType;
  return success();
}

LogicalResult ExpandShapeOp::verify() {
  auto srcType = getSrcType();
  auto resultType = getResultType();

  if ((int64_t)getStaticOutputShape().size() != resultType.getRank())
    return emitOpError("expected number of static shape dims to be equal to "
                       "the output rank (")
           << resultType.getRank() << ") but found "
           << getStaticOutputShape().size() << " inputs instead";

  if ((int64_t)getOutputShape().size() !=
      llvm::count(getStaticOutputShape(), ShapedType::kDynamic))
    return emitOpError("mismatch in dynamic dims in output_shape and "
                       "static_output_shape: static_output_shape has ")
           << llvm::count(getStaticOutputShape(), ShapedType::kDynamic)
           << " dynamic dims while output_shape has " << getOutputShape().size()
           << " values";

  return verifyTensorReshapeOp(*this, resultType, srcType);
}

LogicalResult CollapseShapeOp::verify() {
  return verifyTensorReshapeOp(*this, getSrcType(), getResultType());
}

namespace {
/// Reshape of a splat constant can be replaced with a constant of the result
/// type.
template <typename TensorReshapeOp>
struct FoldReshapeWithConstant : OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    DenseElementsAttr attr;
    if (!matchPattern(reshapeOp.getSrc(), m_Constant(&attr)))
      return failure();
    if (!attr || !attr.isSplat())
      return failure();
    DenseElementsAttr newAttr = DenseElementsAttr::getFromRawBuffer(
        reshapeOp.getResultType(), attr.getRawData());
    rewriter.replaceOpWithNewOp<arith::ConstantOp>(reshapeOp, newAttr);
    return success();
  }
};

// Folds TensorReshapeOp(splat x : src_type) : res_type into splat x : res_type.
template <typename TensorReshapeOp>
class FoldReshapeWithSplat : public OpRewritePattern<TensorReshapeOp> {
public:
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    auto splatOp = reshapeOp.getSrc().template getDefiningOp<tensor::SplatOp>();
    if (!splatOp || !splatOp.getAggregate().getType().hasStaticShape())
      return failure();

    rewriter.replaceOpWithNewOp<tensor::SplatOp>(
        reshapeOp, reshapeOp.getResultType(), splatOp.getInput());
    return success();
  }
};

/// Reshape of a FromElements can be replaced with a FromElements of the
/// result type
template <typename TensorReshapeOp>
struct FoldReshapeWithFromElements : OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    auto fromElements =
        reshapeOp.getSrc().template getDefiningOp<FromElementsOp>();
    if (!fromElements)
      return failure();

    auto shapedTy = llvm::cast<ShapedType>(reshapeOp.getType());

    if (!shapedTy.hasStaticShape())
      return failure();

    rewriter.replaceOpWithNewOp<FromElementsOp>(reshapeOp, reshapeOp.getType(),
                                                fromElements.getElements());
    return success();
  }
};

// Fold CastOp into CollapseShapeOp when adding static information.
struct FoldCollapseOfCastOp : public OpRewritePattern<CollapseShapeOp> {
  using OpRewritePattern<CollapseShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(CollapseShapeOp collapseShapeOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = collapseShapeOp.getSrc().getDefiningOp<tensor::CastOp>();
    if (!tensor::canFoldIntoConsumerOp(castOp))
      return failure();

    RankedTensorType srcType =
        llvm::cast<RankedTensorType>(castOp.getSource().getType());
    RankedTensorType newResultType = CollapseShapeOp::inferCollapsedType(
        srcType, collapseShapeOp.getReassociationMaps());

    if (newResultType == collapseShapeOp.getResultType()) {
      rewriter.modifyOpInPlace(collapseShapeOp, [&]() {
        collapseShapeOp.getSrcMutable().assign(castOp.getSource());
      });
    } else {
      auto newOp = CollapseShapeOp::create(rewriter, collapseShapeOp.getLoc(),
                                           newResultType, castOp.getSource(),
                                           collapseShapeOp.getReassociation());
      rewriter.replaceOpWithNewOp<tensor::CastOp>(
          collapseShapeOp, collapseShapeOp.getResultType(), newOp);
    }
    return success();
  }
};

/// Fold/sink a producer `tensor.cast` with a consumer `tensor.expand_shape` by
/// matching constant output_shape operands of the expand. This makes the
/// `tensor.expand_shape` more static and creates a consumer cast that can be
/// propagated further.
struct ConvertToStaticExpandShape : public OpRewritePattern<ExpandShapeOp> {
  using OpRewritePattern<ExpandShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExpandShapeOp expandOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = expandOp.getSrc().getDefiningOp<CastOp>();
    if (!canFoldIntoConsumerOp(castOp))
      return failure();

    ArrayRef<int64_t> castSrcShape = castOp.getSource().getType().getShape();
    SmallVector<ReassociationIndices, 4> reassoc =
        expandOp.getReassociationIndices();

    SmallVector<int64_t> newOutputShape(expandOp.getResultType().getShape());
    SmallVector<Value> dynamicOutputShape;
    auto outputIt = expandOp.getOutputShape().begin();

    for (const auto &[inputDim, innerReassoc] : llvm::enumerate(reassoc)) {
      for (uint64_t outDim : innerReassoc) {
        if (ShapedType::isStatic(newOutputShape[outDim]))
          continue;

        // If the cast's src type is dynamic, don't infer any of the
        // corresponding expanded dimensions. `tensor.expand_shape` requires at
        // least one of the expanded dimensions to be dynamic if the input is
        // dynamic.
        Value val = *outputIt;
        ++outputIt;
        if (ShapedType::isDynamic(castSrcShape[inputDim])) {
          dynamicOutputShape.push_back(val);
          continue;
        }

        APInt cst;
        if (matchPattern(val, m_ConstantInt(&cst))) {
          newOutputShape[outDim] = cst.getSExtValue();
        } else {
          dynamicOutputShape.push_back(val);
        }
      }
    }

    // Couldn't match any values, nothing to change
    if (expandOp.getOutputShape().size() == dynamicOutputShape.size())
      return failure();

    // Calculate the input shape from the output
    SmallVector<int64_t> newInputShape(expandOp.getSrcType().getRank(), 1l);
    for (auto inDim : llvm::seq<int>(0, newInputShape.size())) {
      for (auto outDim : reassoc[inDim]) {
        auto ofr = newOutputShape[outDim];
        if (ShapedType::isDynamic(ofr)) {
          newInputShape[inDim] = ShapedType::kDynamic;
          break;
        }
        newInputShape[inDim] *= ofr;
      }
    }

    SmallVector<OpFoldResult> outputOfr =
        getMixedValues(newOutputShape, dynamicOutputShape, rewriter);
    auto inputType = RankedTensorType::get(
        newInputShape, expandOp.getSrcType().getElementType());
    auto outputType = RankedTensorType::get(
        newOutputShape, expandOp.getSrcType().getElementType());
    auto inputCast = CastOp::create(rewriter, expandOp.getLoc(), inputType,
                                    expandOp.getSrc());
    auto newExpand = ExpandShapeOp::create(
        rewriter, expandOp.getLoc(), outputType, inputCast.getResult(),
        expandOp.getReassociationIndices(), outputOfr);
    rewriter.replaceOpWithNewOp<CastOp>(expandOp, expandOp.getType(),
                                        newExpand.getResult());
    return success();
  }
};
} // namespace

void ExpandShapeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<
      ComposeReassociativeReshapeOps<ExpandShapeOp, ReshapeOpKind::kExpand>,
      ComposeExpandOfCollapseOp<ExpandShapeOp, CollapseShapeOp>,
      ConvertToStaticExpandShape, FoldReshapeWithConstant<ExpandShapeOp>,
      FoldReshapeWithSplat<ExpandShapeOp>,
      FoldReshapeWithFromElements<ExpandShapeOp>>(context);
}

void CollapseShapeOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                  MLIRContext *context) {
  results.add<
      ComposeReassociativeReshapeOps<CollapseShapeOp, ReshapeOpKind::kCollapse>,
      ComposeCollapseOfExpandOp<CollapseShapeOp, ExpandShapeOp, CastOp,
                                tensor::DimOp, RankedTensorType>,
      FoldReshapeWithConstant<CollapseShapeOp>,
      FoldReshapeWithSplat<CollapseShapeOp>,
      FoldReshapeWithFromElements<CollapseShapeOp>, FoldCollapseOfCastOp>(
      context);
}

OpFoldResult ExpandShapeOp::fold(FoldAdaptor adaptor) {
  return foldReshapeOp<ExpandShapeOp, CollapseShapeOp>(*this,
                                                       adaptor.getOperands());
}

OpFoldResult CollapseShapeOp::fold(FoldAdaptor adaptor) {
  return foldReshapeOp<CollapseShapeOp, ExpandShapeOp>(*this,
                                                       adaptor.getOperands());
}

//===----------------------------------------------------------------------===//
// ExtractSliceOp
//===----------------------------------------------------------------------===//

void ExtractSliceOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "extracted_slice");
}

/// An extract_slice result type can be inferred, when it is not
/// rank-reduced, from the source type and the static representation of
/// offsets, sizes and strides. Special sentinels encode the dynamic case.
RankedTensorType ExtractSliceOp::inferResultType(
    RankedTensorType sourceTensorType, ArrayRef<int64_t> staticOffsets,
    ArrayRef<int64_t> staticSizes, ArrayRef<int64_t> staticStrides) {
  // An extract_slice op may specify only a leading subset of offset/sizes/
  // strides in which case we complete with offset=0, sizes from memref type
  // and strides=1.
  assert(static_cast<int64_t>(staticSizes.size()) ==
             sourceTensorType.getRank() &&
         "unexpected staticSizes not equal to rank of source");
  return RankedTensorType::get(staticSizes, sourceTensorType.getElementType(),
                               sourceTensorType.getEncoding());
}

RankedTensorType ExtractSliceOp::inferResultType(
    RankedTensorType sourceTensorType, ArrayRef<OpFoldResult> offsets,
    ArrayRef<OpFoldResult> sizes, ArrayRef<OpFoldResult> strides) {
  SmallVector<int64_t> staticSizes;
  std::tie(staticSizes, std::ignore) = decomposeMixedValues(sizes);
  assert(static_cast<int64_t>(staticSizes.size()) ==
             sourceTensorType.getRank() &&
         "unexpected staticSizes not equal to rank of source");
  return RankedTensorType::get(staticSizes, sourceTensorType.getElementType(),
                               sourceTensorType.getEncoding());
}

/// If the rank is reduced (i.e. the desiredResultRank is smaller than the
/// number of sizes), drop as many size 1 as needed to produce an inferred
/// type with the desired rank.
///
/// Note that there may be multiple ways to compute this rank-reduced type:
///   e.g. 1x6x1 can rank-reduce to either 1x6 or 6x1 2-D tensors.
///
/// To disambiguate, this function always drops the first 1 sizes occurrences.
RankedTensorType ExtractSliceOp::inferCanonicalRankReducedResultType(
    unsigned desiredResultRank, RankedTensorType sourceRankedTensorType,
    ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
    ArrayRef<int64_t> strides) {
  // Type inferred in the absence of rank-reducing behavior.
  auto inferredType = llvm::cast<RankedTensorType>(
      inferResultType(sourceRankedTensorType, offsets, sizes, strides));
  int rankDiff = inferredType.getRank() - desiredResultRank;
  if (rankDiff > 0) {
    auto shape = inferredType.getShape();
    llvm::SmallBitVector dimsToProject =
        getPositionsOfShapeOne(rankDiff, shape);
    SmallVector<int64_t> projectedShape;
    // Best effort rank-reducing: drop 1s in order.
    for (unsigned pos = 0, e = shape.size(); pos < e; ++pos)
      if (!dimsToProject.test(pos))
        projectedShape.push_back(shape[pos]);
    inferredType =
        RankedTensorType::get(projectedShape, inferredType.getElementType());
  }
  return inferredType;
}

RankedTensorType ExtractSliceOp::inferCanonicalRankReducedResultType(
    unsigned desiredResultRank, RankedTensorType sourceRankedTensorType,
    ArrayRef<OpFoldResult> offsets, ArrayRef<OpFoldResult> sizes,
    ArrayRef<OpFoldResult> strides) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  return ExtractSliceOp::inferCanonicalRankReducedResultType(
      desiredResultRank, sourceRankedTensorType, staticOffsets, staticSizes,
      staticStrides);
}

/// Build an ExtractSliceOp with mixed static and dynamic entries and custom
/// result type. If the type passed is nullptr, it is inferred.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result,
                           RankedTensorType resultType, Value source,
                           ArrayRef<OpFoldResult> offsets,
                           ArrayRef<OpFoldResult> sizes,
                           ArrayRef<OpFoldResult> strides,
                           ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  auto sourceRankedTensorType = llvm::cast<RankedTensorType>(source.getType());
  // Structuring implementation this way avoids duplication between builders.
  if (!resultType) {
    resultType = llvm::cast<RankedTensorType>(ExtractSliceOp::inferResultType(
        sourceRankedTensorType, staticOffsets, staticSizes, staticStrides));
  }
  result.addAttributes(attrs);
  build(b, result, resultType, source, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getDenseI64ArrayAttr(staticOffsets),
        b.getDenseI64ArrayAttr(staticSizes),
        b.getDenseI64ArrayAttr(staticStrides));
}

/// Build an ExtractSliceOp with mixed static and dynamic entries and inferred
/// result type.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                           ArrayRef<OpFoldResult> offsets,
                           ArrayRef<OpFoldResult> sizes,
                           ArrayRef<OpFoldResult> strides,
                           ArrayRef<NamedAttribute> attrs) {
  build(b, result, RankedTensorType(), source, offsets, sizes, strides, attrs);
}

/// Build an ExtractSliceOp with mixed static and dynamic entries packed into
/// a Range vector.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                           ArrayRef<Range> ranges,
                           ArrayRef<NamedAttribute> attrs) {
  auto [offsets, sizes, strides] = getOffsetsSizesAndStrides(ranges);
  build(b, result, RankedTensorType(), source, offsets, sizes, strides, attrs);
}

/// Build an ExtractSliceOp with dynamic entries and custom result type. If
/// the type passed is nullptr, it is inferred.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result,
                           RankedTensorType resultType, Value source,
                           ValueRange offsets, ValueRange sizes,
                           ValueRange strides, ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, resultType, source, offsetValues, sizeValues, strideValues);
}

/// Build an ExtractSliceOp with dynamic entries and inferred result type.
void ExtractSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                           ValueRange offsets, ValueRange sizes,
                           ValueRange strides, ArrayRef<NamedAttribute> attrs) {
  build(b, result, RankedTensorType(), source, offsets, sizes, strides, attrs);
}

static LogicalResult produceSliceErrorMsg(SliceVerificationResult result,
                                          Operation *op,
                                          RankedTensorType expectedType) {
  switch (result) {
  case SliceVerificationResult::Success:
    return success();
  case SliceVerificationResult::RankTooLarge:
    return op->emitError("expected rank to be smaller or equal to ")
           << "the other rank. ";
  case SliceVerificationResult::SizeMismatch:
    return op->emitError("expected type to be ")
           << expectedType << " or a rank-reduced version. (size mismatch) ";
  case SliceVerificationResult::ElemTypeMismatch:
    return op->emitError("expected element type to be ")
           << expectedType.getElementType();
  default:
    llvm_unreachable("unexpected extract_slice op verification result");
  }
}

/// Verifier for ExtractSliceOp.
LogicalResult ExtractSliceOp::verify() {
  RankedTensorType sourceType = getSourceType();

  // Verify result type against inferred type.
  RankedTensorType expectedType = ExtractSliceOp::inferResultType(
      sourceType, getMixedOffsets(), getMixedSizes(), getMixedStrides());
  SliceVerificationResult result = isRankReducedType(expectedType, getType());
  if (result != SliceVerificationResult::Success)
    return produceSliceErrorMsg(result, *this, expectedType);

  // Verify that offsets, sizes, strides do not run out-of-bounds with respect
  // to the source tensor.
  SliceBoundsVerificationResult boundsResult = verifyInBoundsSlice(
      sourceType.getShape(), getStaticOffsets(), getStaticSizes(),
      getStaticStrides(), /*generateErrorMessage=*/true);
  if (!boundsResult.isValid)
    return getOperation()->emitError(boundsResult.errorMessage);

  return success();
}

llvm::SmallBitVector ExtractSliceOp::getDroppedDims() {
  return ::getDroppedDims(getType().getShape(), getMixedSizes());
}

FailureOr<Value>
ExtractSliceOp::rankReduceIfNeeded(OpBuilder &b, Location loc, Value value,
                                   ArrayRef<int64_t> desiredShape) {
  auto sourceTensorType = llvm::dyn_cast<RankedTensorType>(value.getType());
  assert(sourceTensorType && "not a ranked tensor type");
  auto sourceShape = sourceTensorType.getShape();
  if (sourceShape.equals(desiredShape))
    return value;
  auto maybeRankReductionMask =
      mlir::computeRankReductionMask(sourceShape, desiredShape);
  if (!maybeRankReductionMask)
    return failure();
  return createCanonicalRankReducingExtractSliceOp(
      b, loc, value,
      RankedTensorType::Builder(sourceTensorType).setShape(desiredShape));
}

LogicalResult ExtractSliceOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1);
  reifiedReturnShapes[0].reserve(getType().getRank());
  SmallVector<OpFoldResult> mixedSizes = getMixedSizes();
  llvm::SmallBitVector droppedDims = getDroppedDims();
  for (const auto &size : enumerate(mixedSizes)) {
    if (droppedDims.test(size.index()))
      continue;
    reifiedReturnShapes[0].push_back(size.value());
  }
  return success();
}

namespace {
/// Pattern to rewrite an extract_slice op with tensor::Cast arguments.
/// This essentially pushes memref_cast past its consuming slice when
/// `canFoldIntoConsumerOp` is true.
///
/// Example:
/// ```
///   %0 = tensor.cast %V : tensor<16x16xf32> to tensor<?x?xf32>
///   %1 = tensor.extract_slice %0[0, 0][3, 4][1, 1] : tensor<?x?xf32> to
///   tensor<3x4xf32>
/// ```
/// is rewritten into:
/// ```
///   %0 = tensor.extract_slice %V[0, 0][3, 4][1, 1] : tensor<16x16xf32> to
///   tensor<3x4xf32> %1 = tensor.cast %0: tensor<3x4xf32> to tensor<3x4xf32>
/// ```
class ExtractSliceOpCastFolder final : public OpRewritePattern<ExtractSliceOp> {
public:
  using OpRewritePattern<ExtractSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    // Any constant operand, just return to let the constant folder kick in.
    if (llvm::any_of(sliceOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    auto castOp = sliceOp.getSource().getDefiningOp<CastOp>();
    if (!castOp)
      return failure();

    if (!canFoldIntoConsumerOp(castOp))
      return failure();

    // Pattern does not apply if the produced op would not verify.
    SliceBoundsVerificationResult sliceResult = verifyInBoundsSlice(
        cast<RankedTensorType>(castOp.getSource().getType()).getShape(),
        sliceOp.getStaticOffsets(), sliceOp.getStaticSizes(),
        sliceOp.getStaticStrides());
    if (!sliceResult.isValid)
      return failure();

    // Create folded extract.
    Location loc = sliceOp.getLoc();
    Value newResult = ExtractSliceOp::create(
        rewriter, loc, sliceOp.getType(), castOp.getSource(),
        sliceOp.getOffsets(), sliceOp.getSizes(), sliceOp.getStrides(),
        sliceOp.getStaticOffsets(), sliceOp.getStaticSizes(),
        sliceOp.getStaticStrides());
    rewriter.replaceOp(sliceOp, newResult);
    return success();
  }
};

/// Slice elements from `values` into `outValues`. `counts` represents the
/// numbers of elements to stride in the original values for each dimension.
/// The output values can be used to construct a DenseElementsAttr.
template <typename IterTy, typename ElemTy>
static void sliceElements(IterTy values, ArrayRef<int64_t> counts,
                          ArrayRef<int64_t> offsets, ArrayRef<int64_t> sizes,
                          ArrayRef<int64_t> strides,
                          llvm::SmallVectorImpl<ElemTy> *outValues) {
  assert(offsets.size() == sizes.size());
  assert(offsets.size() == strides.size());
  if (offsets.empty())
    return;

  int64_t offset = offsets.front();
  int64_t size = sizes.front();
  int64_t stride = strides.front();
  if (offsets.size() == 1) {
    for (int64_t i = 0; i < size; ++i, offset += stride)
      outValues->push_back(*(values + offset));

    return;
  }

  for (int64_t i = 0; i < size; ++i, offset += stride) {
    auto begin = values + offset * counts.front();
    sliceElements<IterTy, ElemTy>(begin, counts.drop_front(),
                                  offsets.drop_front(), sizes.drop_front(),
                                  strides.drop_front(), outValues);
  }
}

/// Fold arith.constant and tensor.extract_slice into arith.constant. The
/// folded operation might introduce more constant data; Users can control
/// their heuristics by the control function.
class ConstantOpExtractSliceFolder final
    : public OpRewritePattern<ExtractSliceOp> {
public:
  using OpRewritePattern<ExtractSliceOp>::OpRewritePattern;

  ConstantOpExtractSliceFolder(MLIRContext *context,
                               ControlConstantExtractSliceFusionFn controlFn)
      : OpRewritePattern<ExtractSliceOp>(context),
        controlFn(std::move(controlFn)) {}

  LogicalResult matchAndRewrite(ExtractSliceOp op,
                                PatternRewriter &rewriter) const override {
    DenseElementsAttr attr;
    if (!matchPattern(op.getSource(), m_Constant(&attr)))
      return failure();

    // A constant splat is handled by fold().
    if (attr.isSplat())
      return failure();

    // Dynamic result shape is not supported.
    auto sourceType = llvm::cast<ShapedType>(op.getSource().getType());
    auto resultType = llvm::cast<ShapedType>(op.getResult().getType());
    if (!sourceType.hasStaticShape() || !resultType.hasStaticShape())
      return failure();

    // Customized control over the folding.
    if (!controlFn(op))
      return failure();

    int64_t count = sourceType.getNumElements();
    if (count == 0)
      return failure();

    // Check if there are any dynamic parts, which are not supported.
    auto offsets = op.getStaticOffsets();
    if (llvm::is_contained(offsets, ShapedType::kDynamic))
      return failure();
    auto sizes = op.getStaticSizes();
    if (llvm::is_contained(sizes, ShapedType::kDynamic))
      return failure();
    auto strides = op.getStaticStrides();
    if (llvm::is_contained(strides, ShapedType::kDynamic))
      return failure();

    // Compute the stride for each dimension.
    SmallVector<int64_t> counts;
    ArrayRef<int64_t> shape = sourceType.getShape();
    counts.reserve(shape.size());
    for (int64_t v : shape) {
      count = count / v;
      counts.push_back(count);
    }

    // New attribute constructed by the sliced values.
    DenseElementsAttr newAttr;

    if (auto elems = llvm::dyn_cast<DenseIntElementsAttr>(attr)) {
      SmallVector<APInt> outValues;
      outValues.reserve(sourceType.getNumElements());
      sliceElements<DenseElementsAttr::IntElementIterator, APInt>(
          elems.begin(), counts, offsets, sizes, strides, &outValues);
      newAttr = DenseElementsAttr::get(resultType, outValues);
    } else if (auto elems = llvm::dyn_cast<DenseFPElementsAttr>(attr)) {
      SmallVector<APFloat> outValues;
      outValues.reserve(sourceType.getNumElements());
      sliceElements<DenseElementsAttr::FloatElementIterator, APFloat>(
          elems.begin(), counts, offsets, sizes, strides, &outValues);
      newAttr = DenseElementsAttr::get(resultType, outValues);
    }

    if (newAttr) {
      rewriter.replaceOpWithNewOp<arith::ConstantOp>(op, resultType, newAttr);
      return success();
    }

    return failure();
  }

private:
  /// This additionally controls whether the fold happens or not. Users can
  /// impose their heuristics in the function.
  ControlConstantExtractSliceFusionFn controlFn;
};

} // namespace

void mlir::tensor::populateFoldConstantExtractSlicePatterns(
    RewritePatternSet &patterns,
    const ControlConstantExtractSliceFusionFn &controlFn) {
  patterns.add<ConstantOpExtractSliceFolder>(patterns.getContext(), controlFn);
}

/// Return the canonical type of the result of an extract_slice op.
struct SliceReturnTypeCanonicalizer {
  RankedTensorType operator()(ExtractSliceOp op,
                              ArrayRef<OpFoldResult> mixedOffsets,
                              ArrayRef<OpFoldResult> mixedSizes,
                              ArrayRef<OpFoldResult> mixedStrides) {
    return ExtractSliceOp::inferCanonicalRankReducedResultType(
        op.getType().getRank(), op.getSourceType(), mixedOffsets, mixedSizes,
        mixedStrides);
  }
};

/// A canonicalizer wrapper to replace ExtractSliceOps.
struct SliceCanonicalizer {
  void operator()(PatternRewriter &rewriter, ExtractSliceOp op,
                  ExtractSliceOp newOp) {
    Value replacement = newOp.getResult();
    if (replacement.getType() != op.getType())
      replacement = tensor::CastOp::create(rewriter, op.getLoc(), op.getType(),
                                           replacement);
    rewriter.replaceOp(op, replacement);
  }
};

void ExtractSliceOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                 MLIRContext *context) {
  results.add<
      OpWithOffsetSizesAndStridesConstantArgumentFolder<
          ExtractSliceOp, SliceReturnTypeCanonicalizer, SliceCanonicalizer>,
      ExtractSliceOpCastFolder>(context);
}

//
static LogicalResult
foldIdentityOffsetSizeAndStrideOpInterface(OffsetSizeAndStrideOpInterface op,
                                           ShapedType shapedType) {
  OpBuilder b(op.getContext());
  for (OpFoldResult ofr : op.getMixedOffsets())
    if (getConstantIntValue(ofr) != static_cast<int64_t>(0))
      return failure();
  // Rank-reducing noops only need to inspect the leading dimensions:
  // llvm::zip is appropriate.
  auto shape = shapedType.getShape();
  for (auto it : llvm::zip(op.getMixedSizes(), shape))
    if (getConstantIntValue(std::get<0>(it)) != std::get<1>(it))
      return failure();
  for (OpFoldResult ofr : op.getMixedStrides())
    if (getConstantIntValue(ofr) != static_cast<int64_t>(1))
      return failure();
  return success();
}

/// If we have an ExtractSliceOp consuming an InsertSliceOp with the same
/// slice, we can return the InsertSliceOp's source directly.
// TODO: This only checks the immediate producer; extend to go up the
// insert/extract chain if the slices are disjoint.
static Value foldExtractAfterInsertSlice(ExtractSliceOp extractOp) {
  auto insertOp = extractOp.getSource().getDefiningOp<InsertSliceOp>();

  auto isSame = [](OpFoldResult a, OpFoldResult b) { return a == b; };
  if (insertOp && insertOp.getSource().getType() == extractOp.getType() &&
      insertOp.isSameAs(extractOp, isSame))
    return insertOp.getSource();

  return {};
}

OpFoldResult ExtractSliceOp::fold(FoldAdaptor adaptor) {
  if (OpFoldResult reshapedSource = reshapeConstantSource(
          llvm::dyn_cast_if_present<SplatElementsAttr>(adaptor.getSource()),
          getResult().getType()))
    return reshapedSource;
  if (getSourceType() == getType() &&
      succeeded(foldIdentityOffsetSizeAndStrideOpInterface(*this, getType())))
    return this->getSource();
  if (Value slice = foldExtractAfterInsertSlice(*this))
    return slice;

  return OpFoldResult();
}

Value mlir::tensor::createCanonicalRankReducingExtractSliceOp(
    OpBuilder &b, Location loc, Value tensor, RankedTensorType targetType) {
  auto rankedTensorType = llvm::cast<RankedTensorType>(tensor.getType());
  unsigned rank = rankedTensorType.getRank();
  SmallVector<OpFoldResult> offsets(rank, b.getIndexAttr(0));
  SmallVector<OpFoldResult> sizes = getMixedSizes(b, loc, tensor);
  SmallVector<OpFoldResult> strides(rank, b.getIndexAttr(1));
  return b.createOrFold<tensor::ExtractSliceOp>(loc, targetType, tensor,
                                                offsets, sizes, strides);
}

//===----------------------------------------------------------------------===//
// InsertSliceOp
//===----------------------------------------------------------------------===//

void InsertSliceOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "inserted_slice");
}

// Build a InsertSliceOp with mixed static and dynamic entries.
void InsertSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                          Value dest, ArrayRef<OpFoldResult> offsets,
                          ArrayRef<OpFoldResult> sizes,
                          ArrayRef<OpFoldResult> strides,
                          ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  result.addAttributes(attrs);
  build(b, result, dest.getType(), source, dest, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getDenseI64ArrayAttr(staticOffsets),
        b.getDenseI64ArrayAttr(staticSizes),
        b.getDenseI64ArrayAttr(staticStrides));
}

/// Build an InsertSliceOp with mixed static and dynamic entries packed into a
/// Range vector.
void InsertSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                          Value dest, ArrayRef<Range> ranges,
                          ArrayRef<NamedAttribute> attrs) {
  auto [offsets, sizes, strides] = getOffsetsSizesAndStrides(ranges);
  build(b, result, source, dest, offsets, sizes, strides, attrs);
}

// Build a InsertSliceOp with dynamic entries.
void InsertSliceOp::build(OpBuilder &b, OperationState &result, Value source,
                          Value dest, ValueRange offsets, ValueRange sizes,
                          ValueRange strides, ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, source, dest, offsetValues, sizeValues, strideValues);
}

/// Rank-reducing type verification for both InsertSliceOp and
/// ParallelInsertSliceOp.
static SliceVerificationResult verifyInsertSliceOp(
    RankedTensorType srcType, RankedTensorType dstType,
    ArrayRef<int64_t> staticOffsets, ArrayRef<int64_t> staticSizes,
    ArrayRef<int64_t> staticStrides, RankedTensorType *expectedType = nullptr) {
  // insert_slice is the inverse of extract_slice, use the same type
  // inference.
  RankedTensorType expected = ExtractSliceOp::inferResultType(
      dstType, staticOffsets, staticSizes, staticStrides);
  if (expectedType)
    *expectedType = expected;
  return isRankReducedType(expected, srcType);
}

/// Verifier for InsertSliceOp.
LogicalResult InsertSliceOp::verify() {
  // Verify result type against inferred type.
  RankedTensorType expectedType;
  SliceVerificationResult result =
      verifyInsertSliceOp(getSourceType(), getType(), getStaticOffsets(),
                          getStaticSizes(), getStaticStrides(), &expectedType);
  if (result != SliceVerificationResult::Success)
    return produceSliceErrorMsg(result, *this, expectedType);

  // Verify that offsets, sizes, strides do not run out-of-bounds with respect
  // to the destination tensor.
  SliceBoundsVerificationResult boundsResult = verifyInBoundsSlice(
      getDestType().getShape(), getStaticOffsets(), getStaticSizes(),
      getStaticStrides(), /*generateErrorMessage=*/true);
  if (!boundsResult.isValid)
    return getOperation()->emitError(boundsResult.errorMessage);

  return success();
}

/// If we have two consecutive InsertSliceOp writing to the same slice, we
/// can mutate the second InsertSliceOp's destination to the first one's.
///
/// Example:
///
/// ```mlir
///   %0 = tensor.insert_slice %slice0 into %input[0, 0] [64, 64] [1, 1]
///   %1 = tensor.insert_slice %slice1 into %0[0, 0] [64, 64] [1, 1]
/// ```
///
/// folds into:
///
/// ```mlir
///   %1 = tensor.insert_slice %slice1 into %input[0, 0] [64, 64] [1, 1]
/// ```
///
/// This pattern works with both InsertSliceOp and ParallelInsertSliceOp.
static LogicalResult foldInsertAfterInsertSlice(InsertSliceOp insertOp) {
  auto prevInsertOp = insertOp.getDest().getDefiningOp<InsertSliceOp>();

  auto isSame = [](OpFoldResult a, OpFoldResult b) { return a == b; };
  if (!prevInsertOp ||
      prevInsertOp.getSource().getType() != insertOp.getSource().getType() ||
      !prevInsertOp.isSameAs(insertOp, isSame))
    return failure();

  insertOp.getDestMutable().assign(prevInsertOp.getDest());
  return success();
}

/// Folds round-trip extract/insert slice op pairs.
/// Example:
/// ```mlir
/// %0 = tensor.extract_slice %val[0, 0, 0, 0] [1, 1, 2, 4] [1, 1, 1, 1]
/// %1 = tensor.insert_slice %0 into %val[0, 0, 0, 0] [1, 1, 2, 4] [1, 1, 1, 1]
/// ```
/// can be folded into %val.
static Value foldInsertAfterExtractSlice(InsertSliceOp insertOp) {
  auto extractOp = insertOp.getSource().getDefiningOp<ExtractSliceOp>();

  auto isSame = [](OpFoldResult a, OpFoldResult b) { return a == b; };
  if (!extractOp || extractOp.getSource() != insertOp.getDest() ||
      !extractOp.isSameAs(insertOp, isSame))
    return nullptr;

  return extractOp.getSource();
}

OpFoldResult InsertSliceOp::fold(FoldAdaptor) {
  if (getSourceType().hasStaticShape() && getType().hasStaticShape() &&
      getSourceType() == getType() &&
      succeeded(foldIdentityOffsetSizeAndStrideOpInterface(*this, getType())))
    return this->getSource();
  if (succeeded(foldInsertAfterInsertSlice(*this)))
    return getResult();
  if (auto result = foldInsertAfterExtractSlice(*this))
    return result;
  if (llvm::any_of(getMixedSizes(), isZeroInteger))
    return getDest();
  return OpFoldResult();
}

LogicalResult InsertSliceOp::reifyResultShapes(
    OpBuilder &builder, ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<OpFoldResult>(getType().getRank()));
  reifiedReturnShapes[0] = tensor::getMixedSizes(builder, getLoc(), getDest());
  return success();
}

namespace {
/// Pattern to rewrite a insert_slice op with constant arguments.
///
/// This pattern works with both InsertSliceOp and ParallelInsertSliceOp.
template <typename InsertOpTy>
class InsertSliceOpConstantArgumentFolder final
    : public OpRewritePattern<InsertOpTy> {
public:
  using OpRewritePattern<InsertOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<OpFoldResult> mixedOffsets(insertSliceOp.getMixedOffsets());
    SmallVector<OpFoldResult> mixedSizes(insertSliceOp.getMixedSizes());
    SmallVector<OpFoldResult> mixedStrides(insertSliceOp.getMixedStrides());

    // No constant operands were folded, just return;
    if (failed(foldDynamicOffsetSizeList(mixedOffsets)) &&
        failed(foldDynamicOffsetSizeList(mixedSizes)) &&
        failed(foldDynamicStrideList(mixedStrides)))
      return failure();

    // Pattern does not apply if the produced op would not verify.
    SliceBoundsVerificationResult sliceResult =
        verifyInBoundsSlice(insertSliceOp.getDest().getType().getShape(),
                            mixedOffsets, mixedSizes, mixedStrides);
    if (!sliceResult.isValid)
      return failure();

    // Create the new op in canonical form.
    auto sourceType = ExtractSliceOp::inferCanonicalRankReducedResultType(
        insertSliceOp.getSourceType().getRank(), insertSliceOp.getDestType(),
        mixedOffsets, mixedSizes, mixedStrides);
    Value toInsert = insertSliceOp.getSource();
    if (sourceType != insertSliceOp.getSourceType()) {
      OpBuilder::InsertionGuard g(rewriter);
      // The only difference between InsertSliceOp and ParallelInsertSliceOp
      // is that the insertion point is just before the ParallelCombiningOp in
      // the parallel case.
      if (std::is_same<InsertOpTy, ParallelInsertSliceOp>::value)
        rewriter.setInsertionPoint(insertSliceOp->getParentOp());
      toInsert = tensor::CastOp::create(rewriter, insertSliceOp.getLoc(),
                                        sourceType, toInsert);
    }
    rewriter.replaceOpWithNewOp<InsertOpTy>(
        insertSliceOp, toInsert, insertSliceOp.getDest(), mixedOffsets,
        mixedSizes, mixedStrides);
    return success();
  }
};

/// Fold tensor_casts with insert_slice operations. If the source or
/// destination tensor is a tensor_cast that removes static type information,
/// the cast is folded into the insert_slice operation. E.g.:
///
/// ```mlir
///   %1 = tensor.cast %0 : tensor<8x16xf32> to tensor<?x?xf32>
///   %2 = tensor.insert_slice %1 into ... : tensor<?x?xf32> into ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %2 = tensor.insert_slice %0 into ... : tensor<8x16xf32> into ...
/// ```
///
/// Note: When folding a cast on the destination tensor, the result of the
/// insert_slice operation is casted to ensure that the type of the result did
/// not change.
///
/// This pattern works with both InsertSliceOp and ParallelInsertSliceOp.
template <typename InsertOpTy>
struct InsertSliceOpCastFolder final : public OpRewritePattern<InsertOpTy> {
  using OpRewritePattern<InsertOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    if (llvm::any_of(insertSliceOp.getOperands(), [](Value operand) {
          return matchPattern(operand, matchConstantIndex());
        }))
      return failure();

    auto getSourceOfCastOp = [](Value v) -> std::optional<Value> {
      auto castOp = v.getDefiningOp<tensor::CastOp>();
      if (!castOp || !canFoldIntoConsumerOp(castOp))
        return std::nullopt;
      return castOp.getSource();
    };
    std::optional<Value> sourceCastSource =
        getSourceOfCastOp(insertSliceOp.getSource());
    std::optional<Value> destCastSource =
        getSourceOfCastOp(insertSliceOp.getDest());
    if (!sourceCastSource && !destCastSource)
      return failure();

    auto src =
        (sourceCastSource ? *sourceCastSource : insertSliceOp.getSource());
    auto dst = (destCastSource ? *destCastSource : insertSliceOp.getDest());
    auto srcType = llvm::dyn_cast<RankedTensorType>(src.getType());
    auto dstType = llvm::dyn_cast<RankedTensorType>(dst.getType());
    if (!srcType || !dstType)
      return failure();

    // The tensor.cast source could have additional static information not seen
    // in the insert slice op static sizes, so we ignore dynamic dims when
    // computing the rank reduction mask.
    SmallVector<int64_t> staticSizes(insertSliceOp.getStaticSizes());
    auto rankReductionMask = computeRankReductionMask(
        staticSizes, srcType.getShape(), /*matchDynamic=*/true);
    if (!rankReductionMask.has_value())
      return failure();
    // Replace dimensions in the insert slice op with corresponding static dims
    // from the cast source type. If the insert slice sizes have static dims
    // that are not static in the tensor.cast source (i.e., when the cast op
    // casts a dynamic dim to static), the dim should not be replaced, and the
    // pattern will fail later in `verifyInsertSliceOp`.
    SmallVector<OpFoldResult> mixedSizes(insertSliceOp.getMixedSizes());
    int64_t rankReducedIdx = 0;
    for (auto [idx, size] : enumerate(staticSizes)) {
      if (!rankReductionMask.value().contains(idx) &&
          !srcType.isDynamicDim(rankReducedIdx)) {
        mixedSizes[idx] = getAsIndexOpFoldResult(
            rewriter.getContext(), srcType.getDimSize(rankReducedIdx));
        size = srcType.getDimSize(rankReducedIdx++);
      }
    }

    // Pattern does not apply if the produced op would not verify.
    if (verifyInsertSliceOp(srcType, dstType, insertSliceOp.getStaticOffsets(),
                            staticSizes, insertSliceOp.getStaticStrides()) !=
        SliceVerificationResult::Success)
      return failure();
    SliceBoundsVerificationResult sliceResult =
        verifyInBoundsSlice(dstType.getShape(), insertSliceOp.getMixedOffsets(),
                            mixedSizes, insertSliceOp.getMixedStrides());
    if (!sliceResult.isValid)
      return failure();

    Operation *replacement =
        InsertOpTy::create(rewriter, insertSliceOp.getLoc(), src, dst,
                           insertSliceOp.getMixedOffsets(), mixedSizes,
                           insertSliceOp.getMixedStrides());

    // In the parallel case there is no result and so nothing to cast.
    bool isParallelInsert =
        std::is_same<InsertOpTy, ParallelInsertSliceOp>::value;
    if (!isParallelInsert && dst.getType() != insertSliceOp.getDestType()) {
      replacement = tensor::CastOp::create(rewriter, insertSliceOp.getLoc(),
                                           insertSliceOp.getDestType(),
                                           replacement->getResult(0));
    }
    rewriter.replaceOp(insertSliceOp, replacement->getResults());
    return success();
  }
};

/// If additional static type information can be deduced from a insert_slice's
/// size operands, insert an explicit cast of the op's source operand. This
/// enables other canonicalization patterns that are matching for tensor_cast
/// ops such as `ForOpTensorCastFolder` in SCF.
///
/// Example:
///
/// ```mlir
///   %r = tensor.insert_slice %0 into %1[...] [64, 64] [1, 1]
///       : tensor<?x?xf32> into ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %tmp = tensor.cast %0 : tensor<?x?xf32> to tensor<64x64xf32>
///   %r = tensor.insert_slice %tmp into %1[...] [64, 64] [1, 1]
///       : tensor<64x64xf32> into ...
/// ```
///
/// This patterns works with both InsertSliceOp and ParallelInsertSliceOp.
template <typename InsertOpTy>
struct InsertSliceOpSourceCastInserter final
    : public OpRewritePattern<InsertOpTy> {
  using OpRewritePattern<InsertOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(InsertOpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    RankedTensorType srcType = insertSliceOp.getSourceType();
    if (srcType.getRank() != insertSliceOp.getDestType().getRank())
      return failure();
    SmallVector<int64_t> newSrcShape(srcType.getShape());
    for (int64_t i = 0; i < srcType.getRank(); ++i) {
      if (std::optional<int64_t> constInt =
              getConstantIntValue(insertSliceOp.getMixedSizes()[i])) {
        // Bail on invalid IR.
        if (*constInt < 0)
          return failure();
        newSrcShape[i] = *constInt;
      }
    }
    if (!hasValidSizesOffsets(newSrcShape))
      return failure();

    RankedTensorType newSrcType = RankedTensorType::get(
        newSrcShape, srcType.getElementType(), srcType.getEncoding());
    if (srcType == newSrcType ||
        !preservesStaticInformation(srcType, newSrcType) ||
        !tensor::CastOp::areCastCompatible(srcType, newSrcType))
      return failure();

    // newSrcType is:
    //   1) Different from srcType.
    //   2) "More static" than srcType.
    //   3) Cast-compatible with srcType.
    // Insert the cast.
    OpBuilder::InsertionGuard g(rewriter);
    // The only difference between InsertSliceOp and ParallelInsertSliceOp is
    // that the insertion point is just before the ParallelCombiningOp in the
    // parallel case.
    if (std::is_same<InsertOpTy, ParallelInsertSliceOp>::value)
      rewriter.setInsertionPoint(insertSliceOp->getParentOp());
    Value cast = tensor::CastOp::create(rewriter, insertSliceOp.getLoc(),
                                        newSrcType, insertSliceOp.getSource());
    rewriter.replaceOpWithNewOp<InsertOpTy>(
        insertSliceOp, cast, insertSliceOp.getDest(),
        insertSliceOp.getMixedOffsets(), insertSliceOp.getMixedSizes(),
        insertSliceOp.getMixedStrides());
    return success();
  }
};
} // namespace

llvm::SmallBitVector InsertSliceOp::getDroppedDims() {
  return ::getDroppedDims(getSourceType().getShape(), getMixedSizes());
}

void InsertSliceOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                                MLIRContext *context) {
  results.add<InsertSliceOpConstantArgumentFolder<InsertSliceOp>,
              InsertSliceOpCastFolder<InsertSliceOp>,
              InsertSliceOpSourceCastInserter<InsertSliceOp>>(context);
}

Value mlir::tensor::createCanonicalRankReducingInsertSliceOp(OpBuilder &b,
                                                             Location loc,
                                                             Value tensor,
                                                             Value dest) {
  auto rankedTensorType = llvm::cast<RankedTensorType>(dest.getType());
  unsigned rank = rankedTensorType.getRank();
  SmallVector<OpFoldResult> offsets(rank, b.getIndexAttr(0));
  SmallVector<OpFoldResult> sizes = getMixedSizes(b, loc, dest);
  SmallVector<OpFoldResult> strides(rank, b.getIndexAttr(1));
  return b.createOrFold<tensor::InsertSliceOp>(loc, tensor, dest, offsets,
                                               sizes, strides);
}

//===----------------------------------------------------------------------===//
// PadOp
//===----------------------------------------------------------------------===//

void PadOp::getAsmResultNames(function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "padded");
}

// TODO: Replace custom<InferType> directive with AllTypesMatch as soon as it
// supports optional types.
void printInferType(OpAsmPrinter &printer, Operation *op, Value optOperand,
                    Type typeToInfer, Type typeToInferFrom) {}

ParseResult
parseInferType(OpAsmParser &parser,
               std::optional<OpAsmParser::UnresolvedOperand> optOperand,
               Type &typeToInfer, Type typeToInferFrom) {
  if (optOperand)
    typeToInfer = typeToInferFrom;
  return success();
}

LogicalResult PadOp::verify() {
  auto sourceType = llvm::cast<RankedTensorType>(getSource().getType());
  auto resultType = llvm::cast<RankedTensorType>(getResult().getType());
  auto expectedType =
      PadOp::inferResultType(sourceType, getStaticLow(), getStaticHigh());
  if (!expectedType) {
    return emitError("failed to infer expectedType from sourceType ")
           << sourceType << ", specified resultType is " << resultType;
  }
  if (resultType.getRank() != expectedType.getRank()) {
    return emitError("specified type ")
           << resultType << " does not match the inferred type "
           << expectedType;
  }
  for (int i = 0, e = sourceType.getRank(); i < e; ++i) {
    if (resultType.getDimSize(i) == expectedType.getDimSize(i))
      continue;
    if (expectedType.isDynamicDim(i))
      continue;
    return emitError("specified type ")
           << resultType << " does not match the inferred type "
           << expectedType;
  }

  return success();
}

LogicalResult PadOp::verifyRegions() {
  auto &region = getRegion();
  unsigned rank = llvm::cast<RankedTensorType>(getResult().getType()).getRank();
  Block &block = region.front();
  if (block.getNumArguments() != rank)
    return emitError("expected the block to have ") << rank << " arguments";

  // Note: the number and type of yield values are checked in the YieldOp.
  for (const auto &en : llvm::enumerate(block.getArgumentTypes())) {
    if (!en.value().isIndex())
      return emitOpError("expected block argument ")
             << (en.index() + 1) << " to be an index";
  }

  // Ensure that the region yields an element of the right type.
  auto yieldOp = llvm::cast<YieldOp>(block.getTerminator());
  if (yieldOp.getValue().getType() !=
      llvm::cast<ShapedType>(getType()).getElementType())
    return emitOpError("expected yield type to match shape element type");

  return success();
}

RankedTensorType PadOp::inferResultType(RankedTensorType sourceType,
                                        ArrayRef<int64_t> staticLow,
                                        ArrayRef<int64_t> staticHigh,
                                        ArrayRef<int64_t> resultShape) {
  unsigned rank = sourceType.getRank();
  if (staticLow.size() != rank)
    return RankedTensorType();
  if (staticHigh.size() != rank)
    return RankedTensorType();
  if (!resultShape.empty() && resultShape.size() != rank)
    return RankedTensorType();

  SmallVector<int64_t, 4> inferredShape;
  for (auto i : llvm::seq<unsigned>(0, rank)) {
    if (sourceType.isDynamicDim(i) || staticLow[i] == ShapedType::kDynamic ||
        staticHigh[i] == ShapedType::kDynamic) {
      inferredShape.push_back(resultShape.empty() ? ShapedType::kDynamic
                                                  : resultShape[i]);
    } else {
      int64_t size = sourceType.getDimSize(i) + staticLow[i] + staticHigh[i];
      assert((resultShape.empty() || size == resultShape[i] ||
              resultShape[i] == ShapedType::kDynamic) &&
             "mismatch between inferred shape and result shape");
      inferredShape.push_back(size);
    }
  }

  return RankedTensorType::get(inferredShape, sourceType.getElementType());
}

void PadOp::build(OpBuilder &b, OperationState &result, Type resultType,
                  Value source, ArrayRef<int64_t> staticLow,
                  ArrayRef<int64_t> staticHigh, ValueRange low, ValueRange high,
                  bool nofold, ArrayRef<NamedAttribute> attrs) {
  auto sourceType = llvm::cast<RankedTensorType>(source.getType());
  if (!resultType)
    resultType = inferResultType(sourceType, staticLow, staticHigh);
  result.addAttributes(attrs);
  build(b, result, resultType, source, low, high,
        b.getDenseI64ArrayAttr(staticLow), b.getDenseI64ArrayAttr(staticHigh),
        nofold ? b.getUnitAttr() : UnitAttr());
}

void PadOp::build(OpBuilder &b, OperationState &result, Type resultType,
                  Value source, ValueRange low, ValueRange high, bool nofold,
                  ArrayRef<NamedAttribute> attrs) {
  auto sourceType = llvm::cast<RankedTensorType>(source.getType());
  unsigned rank = sourceType.getRank();
  SmallVector<int64_t, 4> staticVector(rank, ShapedType::kDynamic);
  build(b, result, resultType, source, staticVector, staticVector, low, high,
        nofold, attrs);
}

void PadOp::build(OpBuilder &b, OperationState &result, Type resultType,
                  Value source, ArrayRef<OpFoldResult> low,
                  ArrayRef<OpFoldResult> high, bool nofold,
                  ArrayRef<NamedAttribute> attrs) {
  auto sourceType = llvm::cast<RankedTensorType>(source.getType());
  SmallVector<Value, 4> dynamicLow, dynamicHigh;
  SmallVector<int64_t, 4> staticLow, staticHigh;
  // staticLow and staticHigh have full information of the padding config.
  // This will grow staticLow and staticHigh with 1 value. If the config is
  // dynamic (ie not a constant), dynamicLow and dynamicHigh will grow with 1
  // value as well.
  dispatchIndexOpFoldResults(low, dynamicLow, staticLow);
  dispatchIndexOpFoldResults(high, dynamicHigh, staticHigh);
  if (!resultType) {
    resultType = PadOp::inferResultType(sourceType, staticLow, staticHigh);
  }
  assert(llvm::isa<RankedTensorType>(resultType));
  result.addAttributes(attrs);
  build(b, result, resultType, source, dynamicLow, dynamicHigh,
        b.getDenseI64ArrayAttr(staticLow), b.getDenseI64ArrayAttr(staticHigh),
        nofold ? b.getUnitAttr() : UnitAttr());
}

void PadOp::build(OpBuilder &b, OperationState &result, Type resultType,
                  Value source, ArrayRef<OpFoldResult> low,
                  ArrayRef<OpFoldResult> high, Value constantPadValue,
                  bool nofold, ArrayRef<NamedAttribute> attrs) {
  build(b, result, resultType, source, low, high, nofold, attrs);

  // Add a region and a block to yield the pad value.
  Region *region = result.regions[0].get();
  int sourceRank = llvm::cast<RankedTensorType>(source.getType()).getRank();
  SmallVector<Type> blockArgTypes(sourceRank, b.getIndexType());
  SmallVector<Location> blockArgLocs(sourceRank, result.location);

  // `builder.createBlock` changes the insertion point within the block. Create
  // a guard to reset the insertion point of the builder after it is destroyed.
  OpBuilder::InsertionGuard guard(b);
  b.createBlock(region, region->end(), blockArgTypes, blockArgLocs);
  tensor::YieldOp::create(b, result.location, constantPadValue);
}

llvm::SmallBitVector PadOp::getPaddedDims() {
  llvm::SmallBitVector paddedDims(getSourceType().getRank());
  auto extractPaddedDims = [&](ArrayRef<OpFoldResult> paddingWidths) {
    for (const auto &en : enumerate(paddingWidths))
      if (getConstantIntValue(en.value()) != static_cast<int64_t>(0))
        paddedDims.set(en.index());
  };
  extractPaddedDims(getMixedLowPad());
  extractPaddedDims(getMixedHighPad());
  return paddedDims;
}

namespace {
// Folds tensor.pad when padding is static zeros and the attribute
// doesn't request otherwise.
struct FoldStaticZeroPadding : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    if (!padTensorOp.hasZeroLowPad() || !padTensorOp.hasZeroHighPad())
      return failure();
    if (padTensorOp.getNofold())
      return failure();
    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        padTensorOp, padTensorOp.getResult().getType(),
        padTensorOp.getSource());
    return success();
  }
};

// Fold CastOp into PadOp when adding static information.
struct FoldSourceTensorCast : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    auto castOp = padTensorOp.getSource().getDefiningOp<tensor::CastOp>();
    if (!tensor::canFoldIntoConsumerOp(castOp))
      return failure();

    auto newResultType = PadOp::inferResultType(
        llvm::cast<RankedTensorType>(castOp.getSource().getType()),
        padTensorOp.getStaticLow(), padTensorOp.getStaticHigh(),
        padTensorOp.getResultType().getShape());

    if (newResultType == padTensorOp.getResultType()) {
      rewriter.modifyOpInPlace(padTensorOp, [&]() {
        padTensorOp.getSourceMutable().assign(castOp.getSource());
      });
    } else {
      auto newOp = PadOp::create(
          rewriter, padTensorOp->getLoc(), newResultType,
          padTensorOp.getSource(), padTensorOp.getStaticLow(),
          padTensorOp.getStaticHigh(), padTensorOp.getLow(),
          padTensorOp.getHigh(), padTensorOp.getNofold(),
          getPrunedAttributeList(padTensorOp, PadOp::getAttributeNames()));
      IRMapping mapper;
      padTensorOp.getRegion().cloneInto(&newOp.getRegion(), mapper);

      rewriter.replaceOpWithNewOp<tensor::CastOp>(
          padTensorOp, padTensorOp.getResultType(), newOp);
    }
    return success();
  }
};

// Fold CastOp using the result of PadOp back into the latter if it adds
// static information.
struct FoldTargetTensorCast : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    if (!padTensorOp.getResult().hasOneUse())
      return failure();
    auto tensorCastOp =
        dyn_cast<tensor::CastOp>(*padTensorOp->getUsers().begin());
    if (!tensorCastOp)
      return failure();
    if (!tensor::preservesStaticInformation(padTensorOp.getResult().getType(),
                                            tensorCastOp.getDest().getType()))
      return failure();

    auto replacementOp = PadOp::create(
        rewriter, padTensorOp.getLoc(), tensorCastOp.getDest().getType(),
        padTensorOp.getSource(), padTensorOp.getStaticLow(),
        padTensorOp.getStaticHigh(), padTensorOp.getLow(),
        padTensorOp.getHigh(), padTensorOp.getNofold(),
        getPrunedAttributeList(padTensorOp, PadOp::getAttributeNames()));
    replacementOp.getRegion().takeBody(padTensorOp.getRegion());

    rewriter.replaceOp(padTensorOp, replacementOp.getResult());
    rewriter.replaceOp(tensorCastOp, replacementOp.getResult());
    return success();
  }
};

/// Fold chains of tensor::ExtractSliceOp, tensor::PadOp pairs that pad
/// different dimensions. The pattern applies if the following preconditions
/// hold:
///   1) the tensor::ExtractSliceOps are not rank-reducing,
///   2) the tensor::ExtractSliceOps have only unit-strides,
///   3) the tensor::PadOps perform only high-padding,
///   4) the tensor::PadOps have the same constant padding value,
///   5) the tensor::PadOps do not have common padding dimensions,
///   6) one tensor::ExtractSliceOp, tensor::PadOp pair has zero-padding and
///      zero-offset for every dimension.
///   7) the tensor::ExtractSliceOp sizes match the source tensor sizes for
///   the
///      padded source dimensions.
///
/// Example:
///
/// ```mlir
///   %0 = tensor.extract_slice %input[16, 0] [%sz0, 64] [1, 1]
///       : tensor<64x64xf32> to tensor<?x64xf32>
///   %1 = tensor.pad %0 low[0, 0] high[%pw0, 0] { ...
///     } : tensor<?x64xf32> to tensor<8x64xf32>
///   %2 = tensor.extract_slice %1[0, 4] [8, %sz1] [1, 1]
///        : tensor<8x64xf32> to tensor<8x?xf32>
///   %res = tensor.pad %2 nofold low[0, 0] high[0, %pw1] { ...
///     } : tensor<8x?xf32> to tensor<8x4xf32>
/// ```
///
/// folds into:
///
/// ```mlir
///   %0 = tensor.extract_slice %input[16, 4] [%sz0, %sz1] [1, 1]
///        : tensor<64x64xf32> to tensor<?x?xf32>
///   %res = tensor.pad %0 nofold low[0, 0] high[%pw0, %pw1] { ...
///     } : tensor<?x?xf32> to tensor<8x4xf32>
/// ```
struct FoldOrthogonalPaddings : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padOp,
                                PatternRewriter &rewriter) const override {
    auto innerSliceOp = padOp.getSource().getDefiningOp<ExtractSliceOp>();
    if (!innerSliceOp)
      return failure();
    auto outerPadOp = innerSliceOp.getSource().getDefiningOp<PadOp>();
    if (!outerPadOp || outerPadOp.getNofold())
      return failure();
    auto outerSliceOp = outerPadOp.getSource().getDefiningOp<ExtractSliceOp>();
    if (!outerSliceOp)
      return failure();

    // 1) Fail if the chain is rank-reducing.
    int64_t rank = padOp.getSourceType().getRank();
    if (outerSliceOp.getSourceType().getRank() != rank) {
      return rewriter.notifyMatchFailure(padOp,
                                         "cannot fold rank-reducing chain");
    }

    // 2) Fail if the tensor::ExtractSliceOps have non-unit strides.
    if (!innerSliceOp.hasUnitStride() || !outerSliceOp.hasUnitStride()) {
      return rewriter.notifyMatchFailure(
          padOp, "cannot fold non-unit stride ExtractSliceOps");
    }

    // 3) Fail if the tensor::PadOps have non-zero low padding.
    if (!padOp.hasZeroLowPad() || !outerPadOp.hasZeroLowPad()) {
      return rewriter.notifyMatchFailure(padOp,
                                         "cannot fold PadOps with low padding");
    }

    // 4) Fail if the tensor::PadOps padding values do not match.
    Attribute innerAttr, outerAttr;
    Value innerValue = padOp.getConstantPaddingValue();
    Value outerValue = outerPadOp.getConstantPaddingValue();
    if (!innerValue || !outerValue ||
        !matchPattern(innerValue, m_Constant(&innerAttr)) ||
        !matchPattern(outerValue, m_Constant(&outerAttr)) ||
        innerAttr != outerAttr) {
      return rewriter.notifyMatchFailure(
          padOp, "cannot fold PadOps with different padding values");
    }

    // 5) Fail if a dimension is padded by both tensor::PadOps.
    llvm::SmallBitVector innerDims = padOp.getPaddedDims();
    llvm::SmallBitVector outerDims = outerPadOp.getPaddedDims();
    if (innerDims.anyCommon(outerDims)) {
      return rewriter.notifyMatchFailure(
          padOp, "cannot fold PadOps with common padding dimensions");
    }

    // 6) Combine the offsets of the two tensor::ExtractSliceOps. Find the
    // zero-offset and zero-padding tensor::ExtractSliceOp, tensor::PadOp pair
    // for every dimension, and use the offset the other pair. Fail if no
    // zero-offset and zero-padding tensor::ExtractSliceOp, tensor::PadOp pair
    // exists.
    SmallVector<OpFoldResult> newOffsets(rank, rewriter.getIndexAttr(0));
    for (auto en : enumerate(newOffsets)) {
      OpFoldResult innerOffset = innerSliceOp.getMixedOffsets()[en.index()];
      OpFoldResult outerOffset = outerSliceOp.getMixedOffsets()[en.index()];
      if (!innerDims.test(en.index()) &&
          (getConstantIntValue(innerOffset) == static_cast<int64_t>(0))) {
        en.value() = outerOffset;
        continue;
      }
      if (!outerDims.test(en.index()) &&
          (getConstantIntValue(outerOffset) == static_cast<int64_t>(0))) {
        en.value() = innerOffset;
        continue;
      }
      return rewriter.notifyMatchFailure(
          padOp, "cannot find zero-offset and zero-padding pair");
    }

    // 7) Combine the sizes of the two tensor::ExtractSliceOps. Take the size
    // of the outer tensor::ExtractSliceOp for the dimensions padded by the
    // outer tensor::PadOp and fail if the size of the inner
    // tensor::ExtractSliceOp does not match the size of the padded dimension.
    // Otherwise, take the size of the inner tensor::ExtractSliceOp.
    SmallVector<OpFoldResult> newSizes = innerSliceOp.getMixedSizes();
    for (auto en : enumerate(newSizes)) {
      if (!outerDims.test(en.index()))
        continue;
      OpFoldResult sliceSize = innerSliceOp.getMixedSizes()[en.index()];
      int64_t sourceSize = innerSliceOp.getSourceType().getShape()[en.index()];
      assert(ShapedType::isStatic(sourceSize) &&
             "expected padded dimension to have a static size");
      if (getConstantIntValue(sliceSize) != sourceSize) {
        return rewriter.notifyMatchFailure(
            padOp, "cannot fold since the inner ExtractSliceOp size does not "
                   "match the size of the outer padding");
      }
      en.value() = outerSliceOp.getMixedSizes()[en.index()];
    }

    // Combine the high paddings of the two tensor::PadOps.
    SmallVector<OpFoldResult> newHighPad(rank, rewriter.getIndexAttr(0));
    for (auto en : enumerate(newHighPad)) {
      if (innerDims.test(en.index()))
        newHighPad[en.index()] = padOp.getMixedHighPad()[en.index()];
      if (outerDims.test(en.index()))
        newHighPad[en.index()] = outerPadOp.getMixedHighPad()[en.index()];
    }

    // Create a new tensor::ExtractSliceOp, tensor::PadOp pair that performs
    // the two paddings in one step.
    auto newSliceOp = ExtractSliceOp::create(
        rewriter, padOp.getLoc(), outerSliceOp.getSource(), newOffsets,
        newSizes, innerSliceOp.getMixedStrides());
    auto newPadOp = PadOp::create(
        rewriter, padOp.getLoc(), padOp.getResultType(), newSliceOp.getResult(),
        padOp.getMixedLowPad(), newHighPad, padOp.getNofold(),
        getPrunedAttributeList(padOp, PadOp::getAttributeNames()));
    rewriter.inlineRegionBefore(padOp.getRegion(), newPadOp.getRegion(),
                                newPadOp.getRegion().begin());
    rewriter.replaceOp(padOp, newPadOp.getResult());
    return success();
  }
};

struct FoldStaticPadding : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp padTensorOp,
                                PatternRewriter &rewriter) const override {
    Value input = padTensorOp.getSource();
    if (!llvm::isa<RankedTensorType>(input.getType()))
      return failure();
    auto inputDims = llvm::cast<RankedTensorType>(input.getType()).getShape();
    auto inputRank = inputDims.size();

    auto oldResultType =
        dyn_cast<RankedTensorType>(padTensorOp.getResult().getType());
    if (!oldResultType)
      return failure();

    auto outputDims = oldResultType.getShape();

    // Extract the static info from the high and low operands.
    SmallVector<int64_t> constOperandsLow;
    SmallVector<Value> newLows;
    for (auto operand : padTensorOp.getLow()) {
      APSInt intOp;
      if (!matchPattern(operand, m_ConstantInt(&intOp))) {
        constOperandsLow.push_back(ShapedType::kDynamic);
        newLows.push_back(operand);
        continue;
      }
      constOperandsLow.push_back(intOp.getExtValue());
    }
    SmallVector<int64_t> constOperandsHigh;
    SmallVector<Value> newHighs;
    for (auto operand : padTensorOp.getHigh()) {
      APSInt intOp;
      if (!matchPattern(operand, m_ConstantInt(&intOp))) {
        constOperandsHigh.push_back(ShapedType::kDynamic);
        newHighs.push_back(operand);
        continue;
      }
      constOperandsHigh.push_back(intOp.getExtValue());
    }

    SmallVector<int64_t> constLow(padTensorOp.getStaticLow());
    SmallVector<int64_t> constHigh(padTensorOp.getStaticHigh());

    // Verify the op is well-formed.
    if (inputDims.size() != outputDims.size() ||
        inputDims.size() != constLow.size() ||
        inputDims.size() != constHigh.size())
      return failure();

    auto lowCount = 0;
    auto highCount = 0;
    for (size_t i = 0; i < inputRank; i++) {
      if (constLow[i] == ShapedType::kDynamic)
        constLow[i] = constOperandsLow[lowCount++];
      if (constHigh[i] == ShapedType::kDynamic)
        constHigh[i] = constOperandsHigh[highCount++];
    }

    auto staticLow = ArrayRef<int64_t>(constLow);
    auto staticHigh = ArrayRef<int64_t>(constHigh);

    // Calculate the output sizes with the static information.
    SmallVector<int64_t> newOutDims;
    for (size_t i = 0; i < inputRank; i++) {
      if (outputDims[i] == ShapedType::kDynamic) {
        newOutDims.push_back(
            (staticLow[i] == ShapedType::kDynamic ||
                     staticHigh[i] == ShapedType::kDynamic ||
                     inputDims[i] == ShapedType::kDynamic
                 ? ShapedType::kDynamic
                 : inputDims[i] + staticLow[i] + staticHigh[i]));
      } else {
        newOutDims.push_back(outputDims[i]);
      }
    }

    if (SmallVector<int64_t>(outputDims) == newOutDims ||
        llvm::all_of(newOutDims,
                     [&](int64_t x) { return x == ShapedType::kDynamic; }))
      return failure();

    // Rewrite the op using the new static type.
    auto newResultType = RankedTensorType::get(
        newOutDims, padTensorOp.getType().getElementType());
    auto newOp = PadOp::create(
        rewriter, padTensorOp->getLoc(), newResultType, input, staticLow,
        staticHigh, newLows, newHighs, padTensorOp.getNofold(),
        getPrunedAttributeList(padTensorOp, PadOp::getAttributeNames()));

    IRMapping mapper;
    padTensorOp.getRegion().cloneInto(&newOp.getRegion(), mapper);
    rewriter.replaceOpWithNewOp<tensor::CastOp>(padTensorOp, oldResultType,
                                                newOp);

    return success();
  }
};

/// Folds a chain of `tensor.pad` ops with the same constant padding value.
///
/// Example:
///
/// ```mlir
///   %1 = tensor.pad %0 low[0, 1] high[0, 2] {
///       tensor.yield %val
///     } : tensor<1x2xf32> to tensor<2x5xf32>
///   %res = tensor.pad %1 low[0, 2] high[3, 0] {
///       tensor.yield %val
///     } : tensor<1x5xf32> to tensor<5x7xf32>
/// ```
///
/// folds into:
///
/// ```mlir
///   %res = tensor.pad %0 low[0, 3] high[3, 2] {
///       tensor.yield %val
///     } : tensor<1x2xf32> to tensor<5x7xf32>
/// ```
struct FoldConsecutiveConstantPadding : public OpRewritePattern<tensor::PadOp> {
  using OpRewritePattern<tensor::PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::PadOp padOp,
                                PatternRewriter &rewriter) const override {
    if (padOp.getNofold()) {
      return rewriter.notifyMatchFailure(padOp, "skipping unfoldable pad");
    }

    auto producerPad = padOp.getSource().getDefiningOp<tensor::PadOp>();
    if (!producerPad || producerPad.getNofold()) {
      return rewriter.notifyMatchFailure(
          padOp, "producer is not a foldable tensor.pad op");
    }

    // Fail if the tensor::PadOps padding values do not match.
    Value consumerPadValue = padOp.getConstantPaddingValue();
    Value producerPadValue = producerPad.getConstantPaddingValue();
    if (!consumerPadValue || !producerPadValue ||
        consumerPadValue != producerPadValue) {
      return rewriter.notifyMatchFailure(
          padOp,
          "cannot fold PadOps with different or non-constant padding values");
    }

    Location loc = padOp.getLoc();
    AffineExpr d0, d1;
    bindDims(rewriter.getContext(), d0, d1);

    // Combine the low/high paddings of the two tensor::PadOps.
    auto addPaddings = [&](ArrayRef<OpFoldResult> consumerPaddings,
                           ArrayRef<OpFoldResult> producerPaddings) {
      SmallVector<OpFoldResult> sumPaddings;
      for (auto [consumerIndex, producerIndex] :
           llvm::zip_equal(consumerPaddings, producerPaddings)) {
        sumPaddings.push_back(affine::makeComposedFoldedAffineApply(
            rewriter, loc, d0 + d1, {consumerIndex, producerIndex}));
      }
      return sumPaddings;
    };

    SmallVector<OpFoldResult> newHighPad =
        addPaddings(padOp.getMixedHighPad(), producerPad.getMixedHighPad());
    SmallVector<OpFoldResult> newLowPad =
        addPaddings(padOp.getMixedLowPad(), producerPad.getMixedLowPad());

    auto newPadOp = tensor::PadOp::create(
        rewriter, padOp.getLoc(), padOp.getResultType(),
        producerPad.getSource(), newLowPad, newHighPad, padOp.getNofold(),
        getPrunedAttributeList(padOp, tensor::PadOp::getAttributeNames()));
    rewriter.inlineRegionBefore(padOp.getRegion(), newPadOp.getRegion(),
                                newPadOp.getRegion().begin());
    rewriter.replaceOp(padOp, newPadOp.getResult());
    return success();
  }
};

} // namespace

LogicalResult
PadOp::reifyResultShapes(OpBuilder &b,
                         ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<OpFoldResult>(getType().getRank()));
  SmallVector<OpFoldResult> lp = getMixedLowPad();
  SmallVector<OpFoldResult> hp = getMixedHighPad();
  for (int64_t i = 0; i < getResultType().getRank(); ++i) {
    if (!getType().isDynamicDim(i)) {
      reifiedReturnShapes[0][i] = b.getIndexAttr(getType().getDimSize(i));
      continue;
    }
    Location loc = getLoc();
    Value dim = b.createOrFold<tensor::DimOp>(
        loc, getSource(), arith::ConstantIndexOp::create(b, loc, i));

    AffineExpr d0, d1, d2;
    bindDims(b.getContext(), d0, d1, d2);
    reifiedReturnShapes[0][i] = affine::makeComposedFoldedAffineApply(
        b, loc, {d0 + d1 + d2}, {dim, lp[i], hp[i]});
  }
  return success();
}

void PadOp::getCanonicalizationPatterns(RewritePatternSet &results,
                                        MLIRContext *context) {
  results.add<FoldStaticZeroPadding, FoldSourceTensorCast, FoldTargetTensorCast,
              FoldOrthogonalPaddings, FoldStaticPadding,
              FoldConsecutiveConstantPadding>(context);
}

/// Return the padding value of the PadOp if it constant. In this context,
/// "constant" means an actual constant or "defined outside of the block".
///
/// Values are considered constant in three cases:
///  - A ConstantLike value.
///  - A basic block argument from a different block.
///  - A value defined outside of the block.
///
/// If the padding value is not constant, an empty Value is returned.
Value PadOp::getConstantPaddingValue() {
  auto yieldOp = dyn_cast<YieldOp>(getRegion().front().getTerminator());
  if (!yieldOp)
    return {};
  Value padValue = yieldOp.getValue();
  // Check if yield value is a constant.
  if (matchPattern(padValue, m_Constant()))
    return padValue;
  // Check if yield value is defined inside the PadOp block.
  if (padValue.getParentBlock() == &getRegion().front())
    return {};
  // Else: Yield value defined outside of the PadOp block.
  return padValue;
}

OpFoldResult PadOp::fold(FoldAdaptor) {
  if (getResultType().hasStaticShape() && getResultType() == getSourceType() &&
      !getNofold())
    return getSource();
  return {};
}

//===----------------------------------------------------------------------===//
// ParallelInsertSliceOp
//===----------------------------------------------------------------------===//

OpResult ParallelInsertSliceOp::getTiedOpResult() {
  ParallelCombiningOpInterface parallelCombiningParent =
      getParallelCombiningParent();
  for (const auto &it :
       llvm::enumerate(parallelCombiningParent.getYieldingOps())) {
    Operation &nextOp = it.value();
    if (&nextOp == getOperation())
      return parallelCombiningParent.getParentResult(it.index());
  }
  llvm_unreachable("ParallelInsertSliceOp no tied OpResult found");
}

// Build a ParallelInsertSliceOp with mixed static and dynamic entries.
void ParallelInsertSliceOp::build(OpBuilder &b, OperationState &result,
                                  Value source, Value dest,
                                  ArrayRef<OpFoldResult> offsets,
                                  ArrayRef<OpFoldResult> sizes,
                                  ArrayRef<OpFoldResult> strides,
                                  ArrayRef<NamedAttribute> attrs) {
  SmallVector<int64_t> staticOffsets, staticSizes, staticStrides;
  SmallVector<Value> dynamicOffsets, dynamicSizes, dynamicStrides;
  dispatchIndexOpFoldResults(offsets, dynamicOffsets, staticOffsets);
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticSizes);
  dispatchIndexOpFoldResults(strides, dynamicStrides, staticStrides);
  result.addAttributes(attrs);
  build(b, result, {}, source, dest, dynamicOffsets, dynamicSizes,
        dynamicStrides, b.getDenseI64ArrayAttr(staticOffsets),
        b.getDenseI64ArrayAttr(staticSizes),
        b.getDenseI64ArrayAttr(staticStrides));
}

/// Build an ParallelInsertSliceOp with mixed static and dynamic entries
/// packed into a Range vector.
void ParallelInsertSliceOp::build(OpBuilder &b, OperationState &result,
                                  Value source, Value dest,
                                  ArrayRef<Range> ranges,
                                  ArrayRef<NamedAttribute> attrs) {
  auto [offsets, sizes, strides] = getOffsetsSizesAndStrides(ranges);
  build(b, result, source, dest, offsets, sizes, strides, attrs);
}

// Build a ParallelInsertSliceOp with dynamic entries.
void ParallelInsertSliceOp::build(OpBuilder &b, OperationState &result,
                                  Value source, Value dest, ValueRange offsets,
                                  ValueRange sizes, ValueRange strides,
                                  ArrayRef<NamedAttribute> attrs) {
  SmallVector<OpFoldResult> offsetValues = llvm::to_vector<4>(
      llvm::map_range(offsets, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> sizeValues = llvm::to_vector<4>(
      llvm::map_range(sizes, [](Value v) -> OpFoldResult { return v; }));
  SmallVector<OpFoldResult> strideValues = llvm::to_vector<4>(
      llvm::map_range(strides, [](Value v) -> OpFoldResult { return v; }));
  build(b, result, source, dest, offsetValues, sizeValues, strideValues);
}

LogicalResult ParallelInsertSliceOp::verify() {
  if (!isa<ParallelCombiningOpInterface>(getOperation()->getParentOp()))
    return this->emitError("expected ParallelCombiningOpInterface parent, got:")
           << *(getOperation()->getParentOp());

  // Verify result type against inferred type.
  RankedTensorType expectedType;
  SliceVerificationResult result =
      verifyInsertSliceOp(getSourceType(), getDestType(), getStaticOffsets(),
                          getStaticSizes(), getStaticStrides(), &expectedType);
  if (result != SliceVerificationResult::Success)
    return produceSliceErrorMsg(result, *this, expectedType);

  // Verify that offsets, sizes, strides do not run out-of-bounds with respect
  // to the destination tensor.
  SliceBoundsVerificationResult boundsResult = verifyInBoundsSlice(
      getDestType().getShape(), getStaticOffsets(), getStaticSizes(),
      getStaticStrides(), /*generateErrorMessage=*/true);
  if (!boundsResult.isValid)
    return getOperation()->emitError(boundsResult.errorMessage);

  return success();
}

void ParallelInsertSliceOp::getCanonicalizationPatterns(
    RewritePatternSet &results, MLIRContext *context) {
  results.add<InsertSliceOpConstantArgumentFolder<ParallelInsertSliceOp>,
              InsertSliceOpCastFolder<ParallelInsertSliceOp>,
              InsertSliceOpSourceCastInserter<ParallelInsertSliceOp>>(context);
}

llvm::SmallBitVector ParallelInsertSliceOp::getDroppedDims() {
  return ::getDroppedDims(getSourceType().getShape(), getMixedSizes());
}

//===----------------------------------------------------------------------===//
// ScatterOp
//===----------------------------------------------------------------------===//

void ScatterOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "scatter");
}

LogicalResult ScatterOp::verify() {
  int64_t destRank = getDestType().getRank();
  ArrayRef<int64_t> scatterDims = getScatterDims();
  if (failed(verifyGatherOrScatterDims(getOperation(), scatterDims,
                                       getIndicesType().getShape(), destRank,
                                       "scatter", "dest")))
    return failure();

  if (!getUnique())
    return emitOpError("requires 'unique' attribute to be set");
  // TODO: we could also check statically that there are fewer leading index
  // tensor dims than the dest dims. If this is not the case, the unique
  // attribute cannot be true.

  // Use the GatherOp::inferResultType on the `dest` type and verify the
  // expected type matches the source type.
  RankedTensorType expectedSourceType = GatherOp::inferResultType(
      getDestType(), getIndicesType(), scatterDims, /*rankReduced=*/false);
  RankedTensorType expectedRankReducedSourceType = GatherOp::inferResultType(
      getDestType(), getIndicesType(), scatterDims, /*rankReduced=*/true);
  if (getSourceType() != expectedSourceType &&
      getSourceType() != expectedRankReducedSourceType) {
    return emitOpError("source type "
                       "mismatch: "
                       "expected ")
           << expectedSourceType << " or its rank-reduced variant "
           << expectedRankReducedSourceType << " (got: " << getSourceType()
           << ")";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// SplatOp
//===----------------------------------------------------------------------===//

void SplatOp::build(OpBuilder &builder, OperationState &result, Value element,
                    Type aggregateType, ValueRange dynamicSizes) {
  build(builder, result, aggregateType, element, dynamicSizes);
}

void SplatOp::build(OpBuilder &builder, OperationState &result, Value element,
                    ArrayRef<int64_t> staticShape, ValueRange dynamicSizes) {
  auto aggregateType = RankedTensorType::get(staticShape, element.getType());
  build(builder, result, aggregateType, element, dynamicSizes);
}

void SplatOp::build(OpBuilder &builder, OperationState &result, Value element,
                    ArrayRef<OpFoldResult> sizes) {
  SmallVector<int64_t> staticShape;
  SmallVector<Value> dynamicSizes;
  dispatchIndexOpFoldResults(sizes, dynamicSizes, staticShape);
  build(builder, result, element, staticShape, dynamicSizes);
}

void SplatOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  setNameFn(getResult(), "splat");
}

LogicalResult SplatOp::verify() {
  if (getType().getNumDynamicDims() != getDynamicSizes().size())
    return emitOpError("incorrect number of dynamic sizes, has ")
           << getDynamicSizes().size() << ", expected "
           << getType().getNumDynamicDims();
  return success();
}

LogicalResult
SplatOp::reifyResultShapes(OpBuilder &builder,
                           ReifiedRankedShapedTypeDims &reifiedReturnShapes) {
  reifiedReturnShapes.resize(1, SmallVector<OpFoldResult>(getType().getRank()));
  unsigned ctr = 0;
  for (int64_t i = 0; i < getType().getRank(); ++i) {
    if (getType().isDynamicDim(i)) {
      reifiedReturnShapes[0][i] = getDynamicSizes()[ctr++];
    } else {
      reifiedReturnShapes[0][i] = builder.getIndexAttr(getType().getDimSize(i));
    }
  }
  return success();
}

OpFoldResult SplatOp::fold(FoldAdaptor adaptor) {
  auto constOperand = adaptor.getInput();
  if (!isa_and_nonnull<IntegerAttr, FloatAttr>(constOperand))
    return {};

  // Do not fold if the splat is not statically shaped
  if (!getType().hasStaticShape())
    return {};

  // SplatElementsAttr::get treats single value for second arg as being a
  // splat.
  return SplatElementsAttr::get(getType(), {constOperand});
}

//===----------------------------------------------------------------------===//
// Common Canonicalizers and Folders.
//===----------------------------------------------------------------------===//
bool foldTensorCastPrecondition(DestinationStyleOpInterface op) {
  // 1. InsertSliceOp has its own logic about folding tensor.cast ops.
  // 2. Exclude DPS ops that are also LoopLike from this interface as they
  // might need special handling of attached regions.
  if (isa<InsertSliceOp>(op.getOperation()) ||
      isa<LoopLikeOpInterface>(op.getOperation()))
    return false;

  return hasFoldableTensorCastOperand(op);
}

/// Folds a tensor.cast op into a consuming DestinationStyleOpInterface op if
/// the `tensor.cast` has source that is more static than the consuming op.
///
/// Example:
/// ```mlir
///   %1 = tensor.cast %0 : tensor<8x16xf32> to tensor<?x?xf32>
///   %2 = consumer %1 ... : tensor<?x?xf32> ...
/// ```
///
/// folds into:
///
/// ```mlir
///   %2 = consumer %0 ... : tensor<8x16xf32> ...
/// ```
/// TODO: Move the pattern to a proper place, so all other DestinationStyleOp
/// can add the pattern to their canonicalizers.
struct FoldTensorCastProducerOp
    : public OpInterfaceRewritePattern<DestinationStyleOpInterface> {
  using OpInterfaceRewritePattern<
      DestinationStyleOpInterface>::OpInterfaceRewritePattern;

  LogicalResult matchAndRewrite(DestinationStyleOpInterface op,
                                PatternRewriter &rewriter) const override {

    // Reject PackOp/UnpackOp (i.e. RelayoutOps) - there are dedicated patterns
    // for that instead.
    if (!foldTensorCastPrecondition(op) ||
        isa<linalg::RelayoutOpInterface>(*op))
      return failure();

    SmallVector<Type> newResultTypes(op->getResultTypes());
    SmallVector<Value> newOperands =
        getUpdatedOperandsAfterCastOpFolding(op, newResultTypes);

    // Clone op
    auto newOp = clone(rewriter, op, newResultTypes, newOperands);

    SmallVector<Value, 4> replacements;
    replacements.reserve(newOp->getNumResults());
    for (auto [oldResult, newResult] :
         llvm::zip(op->getResults(), newOp->getResults())) {
      if (newResult.getType() != oldResult.getType()) {
        replacements.push_back(tensor::CastOp::create(
            rewriter, op->getLoc(), oldResult.getType(), newResult));
      } else {
        replacements.push_back(newResult);
      }
    }
    rewriter.replaceOp(op, replacements);

    return success();
  }
};

//===----------------------------------------------------------------------===//
// TensorDialect
//===----------------------------------------------------------------------===//

void TensorDialect::getCanonicalizationPatterns(
    RewritePatternSet &results) const {
  results.add<FoldTensorCastProducerOp>(getContext());
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/Tensor/IR/TensorOps.cpp.inc"
