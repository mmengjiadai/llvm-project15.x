// RUN: mlir-opt -pass-pipeline="builtin.module(func.func(convert-arith-to-llvm))" %s -split-input-file | FileCheck %s

// Same below, but using the `ConvertToLLVMPatternInterface` entry point
// and the generic `convert-to-llvm` pass.
// RUN: mlir-opt --convert-to-llvm --split-input-file %s | FileCheck %s

// CHECK-LABEL: @vector_ops
func.func @vector_ops(%arg0: vector<4xf32>, %arg1: vector<4xi1>, %arg2: vector<4xi64>, %arg3: vector<4xi64>) -> vector<4xf32> {
// CHECK-NEXT:  %0 = llvm.mlir.constant(dense<4.200000e+01> : vector<4xf32>) : vector<4xf32>
  %0 = arith.constant dense<42.> : vector<4xf32>
// CHECK-NEXT:  %1 = llvm.fadd %arg0, %0 : vector<4xf32>
  %1 = arith.addf %arg0, %0 : vector<4xf32>
// CHECK-NEXT:  %2 = llvm.sdiv %arg2, %arg2 : vector<4xi64>
  %3 = arith.divsi %arg2, %arg2 : vector<4xi64>
// CHECK-NEXT:  %3 = llvm.udiv %arg2, %arg2 : vector<4xi64>
  %4 = arith.divui %arg2, %arg2 : vector<4xi64>
// CHECK-NEXT:  %4 = llvm.srem %arg2, %arg2 : vector<4xi64>
  %5 = arith.remsi %arg2, %arg2 : vector<4xi64>
// CHECK-NEXT:  %5 = llvm.urem %arg2, %arg2 : vector<4xi64>
  %6 = arith.remui %arg2, %arg2 : vector<4xi64>
// CHECK-NEXT:  %6 = llvm.fdiv %arg0, %0 : vector<4xf32>
  %7 = arith.divf %arg0, %0 : vector<4xf32>
// CHECK-NEXT:  %7 = llvm.frem %arg0, %0 : vector<4xf32>
  %8 = arith.remf %arg0, %0 : vector<4xf32>
// CHECK-NEXT:  %8 = llvm.and %arg2, %arg3 : vector<4xi64>
  %9 = arith.andi %arg2, %arg3 : vector<4xi64>
// CHECK-NEXT:  %9 = llvm.or %arg2, %arg3 : vector<4xi64>
  %10 = arith.ori %arg2, %arg3 : vector<4xi64>
// CHECK-NEXT:  %10 = llvm.xor %arg2, %arg3 : vector<4xi64>
  %11 = arith.xori %arg2, %arg3 : vector<4xi64>
// CHECK-NEXT:  %11 = llvm.shl %arg2, %arg2 : vector<4xi64>
  %12 = arith.shli %arg2, %arg2 : vector<4xi64>
// CHECK-NEXT:  %12 = llvm.ashr %arg2, %arg2 : vector<4xi64>
  %13 = arith.shrsi %arg2, %arg2 : vector<4xi64>
// CHECK-NEXT:  %13 = llvm.lshr %arg2, %arg2 : vector<4xi64>
  %14 = arith.shrui %arg2, %arg2 : vector<4xi64>
  return %1 : vector<4xf32>
}

// CHECK-LABEL: @ops
func.func @ops(f32, f32, i32, i32, f64) -> (f32, i32) {
^bb0(%arg0: f32, %arg1: f32, %arg2: i32, %arg3: i32, %arg4: f64):
// CHECK:  = llvm.fsub %arg0, %arg1 : f32
  %0 = arith.subf %arg0, %arg1: f32
// CHECK: = llvm.sub %arg2, %arg3 : i32
  %1 = arith.subi %arg2, %arg3: i32
// CHECK: = llvm.icmp "slt" %arg2, %1 : i32
  %2 = arith.cmpi slt, %arg2, %1 : i32
// CHECK: = llvm.sdiv %arg2, %arg3 : i32
  %3 = arith.divsi %arg2, %arg3 : i32
// CHECK: = llvm.udiv %arg2, %arg3 : i32
  %4 = arith.divui %arg2, %arg3 : i32
// CHECK: = llvm.srem %arg2, %arg3 : i32
  %5 = arith.remsi %arg2, %arg3 : i32
// CHECK: = llvm.urem %arg2, %arg3 : i32
  %6 = arith.remui %arg2, %arg3 : i32
// CHECK: = llvm.fdiv %arg0, %arg1 : f32
  %8 = arith.divf %arg0, %arg1 : f32
// CHECK: = llvm.frem %arg0, %arg1 : f32
  %9 = arith.remf %arg0, %arg1 : f32
// CHECK: = llvm.and %arg2, %arg3 : i32
  %10 = arith.andi %arg2, %arg3 : i32
// CHECK: = llvm.or %arg2, %arg3 : i32
  %11 = arith.ori %arg2, %arg3 : i32
// CHECK: = llvm.xor %arg2, %arg3 : i32
  %12 = arith.xori %arg2, %arg3 : i32
// CHECK: = llvm.mlir.constant(7.900000e-01 : f64) : f64
  %15 = arith.constant 7.9e-01 : f64
// CHECK: = llvm.shl %arg2, %arg3 : i32
  %16 = arith.shli %arg2, %arg3 : i32
// CHECK: = llvm.ashr %arg2, %arg3 : i32
  %17 = arith.shrsi %arg2, %arg3 : i32
// CHECK: = llvm.lshr %arg2, %arg3 : i32
  %18 = arith.shrui %arg2, %arg3 : i32
  return %0, %4 : f32, i32
}

// Checking conversion of index types to integers using i1, assuming no target
// system would have a 1-bit address space.  Otherwise, we would have had to
// make this test dependent on the pointer size on the target system.
// CHECK-LABEL: @index_cast
func.func @index_cast(%arg0: index, %arg1: i1) {
// CHECK: = llvm.trunc %0 : i{{.*}} to i1
  %0 = arith.index_cast %arg0: index to i1
// CHECK-NEXT: = llvm.sext %arg1 : i1 to i{{.*}}
  %1 = arith.index_cast %arg1: i1 to index
  return
}

// CHECK-LABEL: @vector_index_cast
func.func @vector_index_cast(%arg0: vector<2xindex>, %arg1: vector<2xi1>) {
// CHECK: = llvm.trunc %{{.*}} : vector<2xi{{.*}}> to vector<2xi1>
  %0 = arith.index_cast %arg0: vector<2xindex> to vector<2xi1>
// CHECK-NEXT: = llvm.sext %{{.*}} : vector<2xi1> to vector<2xi{{.*}}>
  %1 = arith.index_cast %arg1: vector<2xi1> to vector<2xindex>
  return
}

func.func @index_castui(%arg0: index, %arg1: i1) {
// CHECK: = llvm.trunc %0 : i{{.*}} to i1
  %0 = arith.index_castui %arg0: index to i1
// CHECK-NEXT: = llvm.zext %arg1 : i1 to i{{.*}}
  %1 = arith.index_castui %arg1: i1 to index
  return
}

// CHECK-LABEL: @vector_index_castui
func.func @vector_index_castui(%arg0: vector<2xindex>, %arg1: vector<2xi1>) {
// CHECK: = llvm.trunc %{{.*}} : vector<2xi{{.*}}> to vector<2xi1>
  %0 = arith.index_castui %arg0: vector<2xindex> to vector<2xi1>
// CHECK-NEXT: = llvm.zext %{{.*}} : vector<2xi1> to vector<2xi{{.*}}>
  %1 = arith.index_castui %arg1: vector<2xi1> to vector<2xindex>
  return
}

// Checking conversion of signed integer types to floating point.
// CHECK-LABEL: @sitofp
func.func @sitofp(%arg0 : i32, %arg1 : i64) {
// CHECK-NEXT: = llvm.sitofp {{.*}} : i32 to f32
  %0 = arith.sitofp %arg0: i32 to f32
// CHECK-NEXT: = llvm.sitofp {{.*}} : i32 to f64
  %1 = arith.sitofp %arg0: i32 to f64
// CHECK-NEXT: = llvm.sitofp {{.*}} : i64 to f32
  %2 = arith.sitofp %arg1: i64 to f32
// CHECK-NEXT: = llvm.sitofp {{.*}} : i64 to f64
  %3 = arith.sitofp %arg1: i64 to f64
  return
}

// Checking conversion of integer vectors to floating point vector types.
// CHECK-LABEL: @sitofp_vector
func.func @sitofp_vector(%arg0 : vector<2xi16>, %arg1 : vector<2xi32>, %arg2 : vector<2xi64>) {
// CHECK-NEXT: = llvm.sitofp {{.*}} : vector<2xi16> to vector<2xf32>
  %0 = arith.sitofp %arg0: vector<2xi16> to vector<2xf32>
// CHECK-NEXT: = llvm.sitofp {{.*}} : vector<2xi16> to vector<2xf64>
  %1 = arith.sitofp %arg0: vector<2xi16> to vector<2xf64>
// CHECK-NEXT: = llvm.sitofp {{.*}} : vector<2xi32> to vector<2xf32>
  %2 = arith.sitofp %arg1: vector<2xi32> to vector<2xf32>
// CHECK-NEXT: = llvm.sitofp {{.*}} : vector<2xi32> to vector<2xf64>
  %3 = arith.sitofp %arg1: vector<2xi32> to vector<2xf64>
// CHECK-NEXT: = llvm.sitofp {{.*}} : vector<2xi64> to vector<2xf32>
  %4 = arith.sitofp %arg2: vector<2xi64> to vector<2xf32>
// CHECK-NEXT: = llvm.sitofp {{.*}} : vector<2xi64> to vector<2xf64>
  %5 = arith.sitofp %arg2: vector<2xi64> to vector<2xf64>
  return
}

// Checking conversion of unsigned integer types to floating point.
// CHECK-LABEL: @uitofp
func.func @uitofp(%arg0 : i32, %arg1 : i64) {
// CHECK-NEXT: = llvm.uitofp {{.*}} : i32 to f32
  %0 = arith.uitofp %arg0: i32 to f32
// CHECK-NEXT: = llvm.uitofp {{.*}} : i32 to f64
  %1 = arith.uitofp %arg0: i32 to f64
// CHECK-NEXT: = llvm.uitofp {{.*}} : i64 to f32
  %2 = arith.uitofp %arg1: i64 to f32
// CHECK-NEXT: = llvm.uitofp {{.*}} : i64 to f64
  %3 = arith.uitofp %arg1: i64 to f64
  return
}

// Checking conversion of integer types to floating point.
// CHECK-LABEL: @fpext
func.func @fpext(%arg0 : f16, %arg1 : f32) {
// CHECK-NEXT: = llvm.fpext {{.*}} : f16 to f32
  %0 = arith.extf %arg0: f16 to f32
// CHECK-NEXT: = llvm.fpext {{.*}} : f16 to f64
  %1 = arith.extf %arg0: f16 to f64
// CHECK-NEXT: = llvm.fpext {{.*}} : f32 to f64
  %2 = arith.extf %arg1: f32 to f64
  return
}

// Checking conversion of integer types to floating point.
// CHECK-LABEL: @fpext
func.func @fpext_vector(%arg0 : vector<2xf16>, %arg1 : vector<2xf32>) {
// CHECK-NEXT: = llvm.fpext {{.*}} : vector<2xf16> to vector<2xf32>
  %0 = arith.extf %arg0: vector<2xf16> to vector<2xf32>
// CHECK-NEXT: = llvm.fpext {{.*}} : vector<2xf16> to vector<2xf64>
  %1 = arith.extf %arg0: vector<2xf16> to vector<2xf64>
// CHECK-NEXT: = llvm.fpext {{.*}} : vector<2xf32> to vector<2xf64>
  %2 = arith.extf %arg1: vector<2xf32> to vector<2xf64>
  return
}

// Checking conversion of floating point to integer types.
// CHECK-LABEL: @fptosi
func.func @fptosi(%arg0 : f32, %arg1 : f64) {
// CHECK-NEXT: = llvm.fptosi {{.*}} : f32 to i32
  %0 = arith.fptosi %arg0: f32 to i32
// CHECK-NEXT: = llvm.fptosi {{.*}} : f32 to i64
  %1 = arith.fptosi %arg0: f32 to i64
// CHECK-NEXT: = llvm.fptosi {{.*}} : f64 to i32
  %2 = arith.fptosi %arg1: f64 to i32
// CHECK-NEXT: = llvm.fptosi {{.*}} : f64 to i64
  %3 = arith.fptosi %arg1: f64 to i64
  return
}

// Checking conversion of floating point vectors to integer vector types.
// CHECK-LABEL: @fptosi_vector
func.func @fptosi_vector(%arg0 : vector<2xf16>, %arg1 : vector<2xf32>, %arg2 : vector<2xf64>) {
// CHECK-NEXT: = llvm.fptosi {{.*}} : vector<2xf16> to vector<2xi32>
  %0 = arith.fptosi %arg0: vector<2xf16> to vector<2xi32>
// CHECK-NEXT: = llvm.fptosi {{.*}} : vector<2xf16> to vector<2xi64>
  %1 = arith.fptosi %arg0: vector<2xf16> to vector<2xi64>
// CHECK-NEXT: = llvm.fptosi {{.*}} : vector<2xf32> to vector<2xi32>
  %2 = arith.fptosi %arg1: vector<2xf32> to vector<2xi32>
// CHECK-NEXT: = llvm.fptosi {{.*}} : vector<2xf32> to vector<2xi64>
  %3 = arith.fptosi %arg1: vector<2xf32> to vector<2xi64>
// CHECK-NEXT: = llvm.fptosi {{.*}} : vector<2xf64> to vector<2xi32>
  %4 = arith.fptosi %arg2: vector<2xf64> to vector<2xi32>
// CHECK-NEXT: = llvm.fptosi {{.*}} : vector<2xf64> to vector<2xi64>
  %5 = arith.fptosi %arg2: vector<2xf64> to vector<2xi64>
  return
}

// Checking conversion of floating point to integer types.
// CHECK-LABEL: @fptoui
func.func @fptoui(%arg0 : f32, %arg1 : f64) {
// CHECK-NEXT: = llvm.fptoui {{.*}} : f32 to i32
  %0 = arith.fptoui %arg0: f32 to i32
// CHECK-NEXT: = llvm.fptoui {{.*}} : f32 to i64
  %1 = arith.fptoui %arg0: f32 to i64
// CHECK-NEXT: = llvm.fptoui {{.*}} : f64 to i32
  %2 = arith.fptoui %arg1: f64 to i32
// CHECK-NEXT: = llvm.fptoui {{.*}} : f64 to i64
  %3 = arith.fptoui %arg1: f64 to i64
  return
}

// Checking conversion of floating point vectors to integer vector types.
// CHECK-LABEL: @fptoui_vector
func.func @fptoui_vector(%arg0 : vector<2xf16>, %arg1 : vector<2xf32>, %arg2 : vector<2xf64>) {
// CHECK-NEXT: = llvm.fptoui {{.*}} : vector<2xf16> to vector<2xi32>
  %0 = arith.fptoui %arg0: vector<2xf16> to vector<2xi32>
// CHECK-NEXT: = llvm.fptoui {{.*}} : vector<2xf16> to vector<2xi64>
  %1 = arith.fptoui %arg0: vector<2xf16> to vector<2xi64>
// CHECK-NEXT: = llvm.fptoui {{.*}} : vector<2xf32> to vector<2xi32>
  %2 = arith.fptoui %arg1: vector<2xf32> to vector<2xi32>
// CHECK-NEXT: = llvm.fptoui {{.*}} : vector<2xf32> to vector<2xi64>
  %3 = arith.fptoui %arg1: vector<2xf32> to vector<2xi64>
// CHECK-NEXT: = llvm.fptoui {{.*}} : vector<2xf64> to vector<2xi32>
  %4 = arith.fptoui %arg2: vector<2xf64> to vector<2xi32>
// CHECK-NEXT: = llvm.fptoui {{.*}} : vector<2xf64> to vector<2xi64>
  %5 = arith.fptoui %arg2: vector<2xf64> to vector<2xi64>
  return
}

// Checking conversion of integer vectors to floating point vector types.
// CHECK-LABEL: @uitofp_vector
func.func @uitofp_vector(%arg0 : vector<2xi16>, %arg1 : vector<2xi32>, %arg2 : vector<2xi64>) {
// CHECK-NEXT: = llvm.uitofp {{.*}} : vector<2xi16> to vector<2xf32>
  %0 = arith.uitofp %arg0: vector<2xi16> to vector<2xf32>
// CHECK-NEXT: = llvm.uitofp {{.*}} : vector<2xi16> to vector<2xf64>
  %1 = arith.uitofp %arg0: vector<2xi16> to vector<2xf64>
// CHECK-NEXT: = llvm.uitofp {{.*}} : vector<2xi32> to vector<2xf32>
  %2 = arith.uitofp %arg1: vector<2xi32> to vector<2xf32>
// CHECK-NEXT: = llvm.uitofp {{.*}} : vector<2xi32> to vector<2xf64>
  %3 = arith.uitofp %arg1: vector<2xi32> to vector<2xf64>
// CHECK-NEXT: = llvm.uitofp {{.*}} : vector<2xi64> to vector<2xf32>
  %4 = arith.uitofp %arg2: vector<2xi64> to vector<2xf32>
// CHECK-NEXT: = llvm.uitofp {{.*}} : vector<2xi64> to vector<2xf64>
  %5 = arith.uitofp %arg2: vector<2xi64> to vector<2xf64>
  return
}

// Checking conversion of integer types to floating point.
// CHECK-LABEL: @fptrunc
func.func @fptrunc(%arg0 : f32, %arg1 : f64) {
// CHECK-NEXT: = llvm.fptrunc {{.*}} : f32 to f16
  %0 = arith.truncf %arg0: f32 to f16
// CHECK-NEXT: = llvm.fptrunc {{.*}} : f64 to f16
  %1 = arith.truncf %arg1: f64 to f16
// CHECK-NEXT: = llvm.fptrunc {{.*}} : f64 to f32
  %2 = arith.truncf %arg1: f64 to f32
  return
}

// Checking conversion of integer types to floating point.
// CHECK-LABEL: @fptrunc
func.func @fptrunc_vector(%arg0 : vector<2xf32>, %arg1 : vector<2xf64>) {
// CHECK-NEXT: = llvm.fptrunc {{.*}} : vector<2xf32> to vector<2xf16>
  %0 = arith.truncf %arg0: vector<2xf32> to vector<2xf16>
// CHECK-NEXT: = llvm.fptrunc {{.*}} : vector<2xf64> to vector<2xf16>
  %1 = arith.truncf %arg1: vector<2xf64> to vector<2xf16>
// CHECK-NEXT: = llvm.fptrunc {{.*}} : vector<2xf64> to vector<2xf32>
  %2 = arith.truncf %arg1: vector<2xf64> to vector<2xf32>
  return
}

// Check sign and zero extension and truncation of integers.
// CHECK-LABEL: @integer_extension_and_truncation
func.func @integer_extension_and_truncation(%arg0 : i3) {
// CHECK-NEXT: = llvm.sext %arg0 : i3 to i6
  %0 = arith.extsi %arg0 : i3 to i6
// CHECK-NEXT: = llvm.zext %arg0 : i3 to i6
  %1 = arith.extui %arg0 : i3 to i6
// CHECK-NEXT: = llvm.trunc %arg0 : i3 to i2
   %2 = arith.trunci %arg0 : i3 to i2
  return
}

// CHECK-LABEL: @integer_cast_0d_vector
func.func @integer_cast_0d_vector(%arg0 : vector<i3>) {
// CHECK: %[[ARG0:.*]] = builtin.unrealized_conversion_cast
// CHECK-NEXT: = llvm.sext %[[ARG0]] : vector<1xi3> to vector<1xi6>
  %0 = arith.extsi %arg0 : vector<i3> to vector<i6>
// CHECK-NEXT: = llvm.zext %[[ARG0]] : vector<1xi3> to vector<1xi6>
  %1 = arith.extui %arg0 : vector<i3> to vector<i6>
// CHECK-NEXT: = llvm.trunc %[[ARG0]] : vector<1xi3> to vector<1xi2>
  %2 = arith.trunci %arg0 : vector<i3> to vector<i2>
  return
}

// CHECK-LABEL: func @fcmp(%arg0: f32, %arg1: f32) {
func.func @fcmp(f32, f32) -> () {
^bb0(%arg0: f32, %arg1: f32):
  // CHECK:      llvm.fcmp "oeq" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ogt" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "oge" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "olt" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ole" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "one" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ord" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ueq" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ugt" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "uge" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ult" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "ule" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "une" %arg0, %arg1 : f32
  // CHECK-NEXT: llvm.fcmp "uno" %arg0, %arg1 : f32
  // CHECK-NEXT: return
  %1 = arith.cmpf oeq, %arg0, %arg1 : f32
  %2 = arith.cmpf ogt, %arg0, %arg1 : f32
  %3 = arith.cmpf oge, %arg0, %arg1 : f32
  %4 = arith.cmpf olt, %arg0, %arg1 : f32
  %5 = arith.cmpf ole, %arg0, %arg1 : f32
  %6 = arith.cmpf one, %arg0, %arg1 : f32
  %7 = arith.cmpf ord, %arg0, %arg1 : f32
  %8 = arith.cmpf ueq, %arg0, %arg1 : f32
  %9 = arith.cmpf ugt, %arg0, %arg1 : f32
  %10 = arith.cmpf uge, %arg0, %arg1 : f32
  %11 = arith.cmpf ult, %arg0, %arg1 : f32
  %12 = arith.cmpf ule, %arg0, %arg1 : f32
  %13 = arith.cmpf une, %arg0, %arg1 : f32
  %14 = arith.cmpf uno, %arg0, %arg1 : f32

  return
}

// -----

// CHECK-LABEL: @index_vector
func.func @index_vector(%arg0: vector<4xindex>) {
  // CHECK: %[[CST:.*]] = llvm.mlir.constant(dense<[0, 1, 2, 3]> : vector<4xindex>) : vector<4xi64>
  %0 = arith.constant dense<[0, 1, 2, 3]> : vector<4xindex>
  // CHECK: %[[V:.*]] = llvm.add %{{.*}}, %[[CST]] : vector<4xi64>
  %1 = arith.addi %arg0, %0 : vector<4xindex>
  func.return
}

// -----

// CHECK-LABEL: @bitcast_1d
func.func @bitcast_1d(%arg0: vector<2xf32>) {
  // CHECK: llvm.bitcast %{{.*}} : vector<2xf32> to vector<2xi32>
  arith.bitcast %arg0 : vector<2xf32> to vector<2xi32>
  return
}

// -----

// CHECK-LABEL: @addui_extended_scalar
// CHECK-SAME:    ([[ARG0:%.+]]: i32, [[ARG1:%.+]]: i32) -> (i32, i1)
func.func @addui_extended_scalar(%arg0: i32, %arg1: i32) -> (i32, i1) {
  // CHECK-NEXT: [[RES:%.+]] = "llvm.intr.uadd.with.overflow"([[ARG0]], [[ARG1]]) : (i32, i32) -> !llvm.struct<(i32, i1)>
  // CHECK-NEXT: [[SUM:%.+]] = llvm.extractvalue [[RES]][0] : !llvm.struct<(i32, i1)>
  // CHECK-NEXT: [[CARRY:%.+]] = llvm.extractvalue [[RES]][1] : !llvm.struct<(i32, i1)>
  %sum, %carry = arith.addui_extended %arg0, %arg1 : i32, i1
  // CHECK-NEXT: return [[SUM]], [[CARRY]] : i32, i1
  return %sum, %carry : i32, i1
}

// CHECK-LABEL: @addui_extended_vector1d
// CHECK-SAME:    ([[ARG0:%.+]]: vector<3xi16>, [[ARG1:%.+]]: vector<3xi16>) -> (vector<3xi16>, vector<3xi1>)
func.func @addui_extended_vector1d(%arg0: vector<3xi16>, %arg1: vector<3xi16>) -> (vector<3xi16>, vector<3xi1>) {
  // CHECK-NEXT: [[RES:%.+]] = "llvm.intr.uadd.with.overflow"([[ARG0]], [[ARG1]]) : (vector<3xi16>, vector<3xi16>) -> !llvm.struct<(vector<3xi16>, vector<3xi1>)>
  // CHECK-NEXT: [[SUM:%.+]] = llvm.extractvalue [[RES]][0] : !llvm.struct<(vector<3xi16>, vector<3xi1>)>
  // CHECK-NEXT: [[CARRY:%.+]] = llvm.extractvalue [[RES]][1] : !llvm.struct<(vector<3xi16>, vector<3xi1>)>
  %sum, %carry = arith.addui_extended %arg0, %arg1 : vector<3xi16>, vector<3xi1>
  // CHECK-NEXT: return [[SUM]], [[CARRY]] : vector<3xi16>, vector<3xi1>
  return %sum, %carry : vector<3xi16>, vector<3xi1>
}

// -----

// CHECK-LABEL: @mulsi_extended_scalar
// CHECK-SAME:    ([[ARG0:%.+]]: i32, [[ARG1:%.+]]: i32) -> (i32, i32)
func.func @mulsi_extended_scalar(%arg0: i32, %arg1: i32) -> (i32, i32) {
  // CHECK-NEXT: [[LHS:%.+]]  = llvm.sext [[ARG0]] : i32 to i64
  // CHECK-NEXT: [[RHS:%.+]]  = llvm.sext [[ARG1]] : i32 to i64
  // CHECK-NEXT: [[MUL:%.+]]  = llvm.mul [[LHS]], [[RHS]] : i64
  // CHECK-NEXT: [[LOW:%.+]]  = llvm.trunc [[MUL]] : i64 to i32
  // CHECK-NEXT: [[C32:%.+]]  = llvm.mlir.constant(32 : i64) : i64
  // CHECK-NEXT: [[SHL:%.+]]  = llvm.lshr [[MUL]], [[C32]] : i64
  // CHECK-NEXT: [[HIGH:%.+]] = llvm.trunc [[SHL]] : i64 to i32
  %low, %high = arith.mulsi_extended %arg0, %arg1 : i32
  // CHECK-NEXT: return [[LOW]], [[HIGH]] : i32, i32
  return %low, %high : i32, i32
}

// CHECK-LABEL: @mulsi_extended_vector1d
// CHECK-SAME:    ([[ARG0:%.+]]: vector<3xi64>, [[ARG1:%.+]]: vector<3xi64>) -> (vector<3xi64>, vector<3xi64>)
func.func @mulsi_extended_vector1d(%arg0: vector<3xi64>, %arg1: vector<3xi64>) -> (vector<3xi64>, vector<3xi64>) {
  // CHECK-NEXT: [[LHS:%.+]]  = llvm.sext [[ARG0]] : vector<3xi64> to vector<3xi128>
  // CHECK-NEXT: [[RHS:%.+]]  = llvm.sext [[ARG1]] : vector<3xi64> to vector<3xi128>
  // CHECK-NEXT: [[MUL:%.+]]  = llvm.mul [[LHS]], [[RHS]] : vector<3xi128>
  // CHECK-NEXT: [[LOW:%.+]]  = llvm.trunc [[MUL]] : vector<3xi128> to vector<3xi64>
  // CHECK-NEXT: [[C64:%.+]]  = llvm.mlir.constant(dense<64> : vector<3xi128>) : vector<3xi128>
  // CHECK-NEXT: [[SHL:%.+]]  = llvm.lshr [[MUL]], [[C64]] : vector<3xi128>
  // CHECK-NEXT: [[HIGH:%.+]] = llvm.trunc [[SHL]] : vector<3xi128> to vector<3xi64>
  %low, %high = arith.mulsi_extended %arg0, %arg1 : vector<3xi64>
  // CHECK-NEXT: return [[LOW]], [[HIGH]] : vector<3xi64>, vector<3xi64>
  return %low, %high : vector<3xi64>, vector<3xi64>
}

// -----

// CHECK-LABEL: @mului_extended_scalar
// CHECK-SAME:    ([[ARG0:%.+]]: i32, [[ARG1:%.+]]: i32) -> (i32, i32)
func.func @mului_extended_scalar(%arg0: i32, %arg1: i32) -> (i32, i32) {
  // CHECK-NEXT: [[LHS:%.+]]  = llvm.zext [[ARG0]] : i32 to i64
  // CHECK-NEXT: [[RHS:%.+]]  = llvm.zext [[ARG1]] : i32 to i64
  // CHECK-NEXT: [[MUL:%.+]]  = llvm.mul [[LHS]], [[RHS]] : i64
  // CHECK-NEXT: [[LOW:%.+]]  = llvm.trunc [[MUL]] : i64 to i32
  // CHECK-NEXT: [[C32:%.+]]  = llvm.mlir.constant(32 : i64) : i64
  // CHECK-NEXT: [[SHL:%.+]]  = llvm.lshr [[MUL]], [[C32]] : i64
  // CHECK-NEXT: [[HIGH:%.+]] = llvm.trunc [[SHL]] : i64 to i32
  %low, %high = arith.mului_extended %arg0, %arg1 : i32
  // CHECK-NEXT: return [[LOW]], [[HIGH]] : i32, i32
  return %low, %high : i32, i32
}

// CHECK-LABEL: @mului_extended_vector1d
// CHECK-SAME:    ([[ARG0:%.+]]: vector<3xi64>, [[ARG1:%.+]]: vector<3xi64>) -> (vector<3xi64>, vector<3xi64>)
func.func @mului_extended_vector1d(%arg0: vector<3xi64>, %arg1: vector<3xi64>) -> (vector<3xi64>, vector<3xi64>) {
  // CHECK-NEXT: [[LHS:%.+]]  = llvm.zext [[ARG0]] : vector<3xi64> to vector<3xi128>
  // CHECK-NEXT: [[RHS:%.+]]  = llvm.zext [[ARG1]] : vector<3xi64> to vector<3xi128>
  // CHECK-NEXT: [[MUL:%.+]]  = llvm.mul [[LHS]], [[RHS]] : vector<3xi128>
  // CHECK-NEXT: [[LOW:%.+]]  = llvm.trunc [[MUL]] : vector<3xi128> to vector<3xi64>
  // CHECK-NEXT: [[C64:%.+]]  = llvm.mlir.constant(dense<64> : vector<3xi128>) : vector<3xi128>
  // CHECK-NEXT: [[SHL:%.+]]  = llvm.lshr [[MUL]], [[C64]] : vector<3xi128>
  // CHECK-NEXT: [[HIGH:%.+]] = llvm.trunc [[SHL]] : vector<3xi128> to vector<3xi64>
  %low, %high = arith.mului_extended %arg0, %arg1 : vector<3xi64>
  // CHECK-NEXT: return [[LOW]], [[HIGH]] : vector<3xi64>, vector<3xi64>
  return %low, %high : vector<3xi64>, vector<3xi64>
}

// -----

// CHECK-LABEL: func @cmpf_2dvector(
func.func @cmpf_2dvector(%arg0 : vector<4x3xf32>, %arg1 : vector<4x3xf32>) {
  // CHECK: %[[ARG0:.*]] = builtin.unrealized_conversion_cast
  // CHECK: %[[ARG1:.*]] = builtin.unrealized_conversion_cast
  // CHECK: %[[EXTRACT1:.*]] = llvm.extractvalue %[[ARG0]][0] : !llvm.array<4 x vector<3xf32>>
  // CHECK: %[[EXTRACT2:.*]] = llvm.extractvalue %[[ARG1]][0] : !llvm.array<4 x vector<3xf32>>
  // CHECK: %[[CMP:.*]] = llvm.fcmp "olt" %[[EXTRACT1]], %[[EXTRACT2]] : vector<3xf32>
  // CHECK: %[[INSERT:.*]] = llvm.insertvalue %[[CMP]], %2[0] : !llvm.array<4 x vector<3xi1>>
  %0 = arith.cmpf olt, %arg0, %arg1 : vector<4x3xf32>
  func.return
}

// -----

// CHECK-LABEL: func @cmpi_0dvector(
func.func @cmpi_0dvector(%arg0 : vector<i32>, %arg1 : vector<i32>) {
  // CHECK: %[[ARG0:.*]] = builtin.unrealized_conversion_cast
  // CHECK: %[[ARG1:.*]] = builtin.unrealized_conversion_cast
  // CHECK: %[[CMP:.*]] = llvm.icmp "ult" %[[ARG0]], %[[ARG1]] : vector<1xi32>
  %0 = arith.cmpi ult, %arg0, %arg1 : vector<i32>
  func.return
}

// -----

// CHECK-LABEL: func @cmpi_2dvector(
func.func @cmpi_2dvector(%arg0 : vector<4x3xi32>, %arg1 : vector<4x3xi32>) {
  // CHECK: %[[ARG0:.*]] = builtin.unrealized_conversion_cast
  // CHECK: %[[ARG1:.*]] = builtin.unrealized_conversion_cast
  // CHECK: %[[EXTRACT1:.*]] = llvm.extractvalue %[[ARG0]][0] : !llvm.array<4 x vector<3xi32>>
  // CHECK: %[[EXTRACT2:.*]] = llvm.extractvalue %[[ARG1]][0] : !llvm.array<4 x vector<3xi32>>
  // CHECK: %[[CMP:.*]] = llvm.icmp "ult" %[[EXTRACT1]], %[[EXTRACT2]] : vector<3xi32>
  // CHECK: %[[INSERT:.*]] = llvm.insertvalue %[[CMP]], %2[0] : !llvm.array<4 x vector<3xi1>>
  %0 = arith.cmpi ult, %arg0, %arg1 : vector<4x3xi32>
  func.return
}

// -----

// CHECK-LABEL: @select
func.func @select(%arg0 : i1, %arg1 : i32, %arg2 : i32) -> i32 {
  // CHECK: = llvm.select %arg0, %arg1, %arg2 : i1, i32
  %0 = arith.select %arg0, %arg1, %arg2 : i32
  return %0 : i32
}

// -----

// CHECK-LABEL: @minmaxi
func.func @minmaxi(%arg0 : i32, %arg1 : i32) -> i32 {
  // CHECK: = llvm.intr.smin(%arg0, %arg1) : (i32, i32) -> i32
  %0 = arith.minsi %arg0, %arg1 : i32
  // CHECK: = llvm.intr.smax(%arg0, %arg1) : (i32, i32) -> i32
  %1 = arith.maxsi %arg0, %arg1 : i32
  // CHECK: = llvm.intr.umin(%arg0, %arg1) : (i32, i32) -> i32
  %2 = arith.minui %arg0, %arg1 : i32
  // CHECK: = llvm.intr.umax(%arg0, %arg1) : (i32, i32) -> i32
  %3 = arith.maxui %arg0, %arg1 : i32
  return %0 : i32
}

// CHECK-LABEL: @minmaxf
func.func @minmaxf(%arg0 : f32, %arg1 : f32) -> f32 {
  // CHECK: = llvm.intr.minnum(%arg0, %arg1) : (f32, f32) -> f32
  %0 = arith.minf %arg0, %arg1 : f32
  // CHECK: = llvm.intr.maxnum(%arg0, %arg1) : (f32, f32) -> f32
  %1 = arith.maxf %arg0, %arg1 : f32
  return %0 : f32
}

// -----

// CHECK-LABEL: @fastmath
func.func @fastmath(%arg0: f32, %arg1: f32, %arg2: i32) {
// CHECK: llvm.fadd %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
// CHECK: llvm.fmul %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
// CHECK: llvm.fneg %arg0  {fastmathFlags = #llvm.fastmath<fast>} : f32
// CHECK: llvm.fadd %arg0, %arg1  : f32
// CHECK: llvm.fadd %arg0, %arg1  {fastmathFlags = #llvm.fastmath<nnan, ninf>} : f32
  %0 = arith.addf %arg0, %arg1 fastmath<fast> : f32
  %1 = arith.mulf %arg0, %arg1 fastmath<fast> : f32
  %2 = arith.negf %arg0 fastmath<fast> : f32
  %3 = arith.addf %arg0, %arg1 fastmath<none> : f32
  %4 = arith.addf %arg0, %arg1 fastmath<nnan,ninf> : f32
  return
}

// -----

// CHECK-LABEL: @ops_supporting_fastmath
func.func @ops_supporting_fastmath(%arg0: f32, %arg1: f32, %arg2: i32) {
// CHECK: llvm.fadd %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
  %0 = arith.addf %arg0, %arg1 fastmath<fast> : f32
// CHECK: llvm.fdiv %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
  %1 = arith.divf %arg0, %arg1 fastmath<fast> : f32
// CHECK: llvm.intr.maxnum(%arg0, %arg1) {fastmathFlags = #llvm.fastmath<fast>} : (f32, f32) -> f32
  %2 = arith.maxf %arg0, %arg1 fastmath<fast> : f32
// CHECK: llvm.intr.minnum(%arg0, %arg1) {fastmathFlags = #llvm.fastmath<fast>} : (f32, f32) -> f32
  %3 = arith.minf %arg0, %arg1 fastmath<fast> : f32
// CHECK: llvm.fmul %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
  %4 = arith.mulf %arg0, %arg1 fastmath<fast> : f32
// CHECK: llvm.fneg %arg0  {fastmathFlags = #llvm.fastmath<fast>} : f32
  %5 = arith.negf %arg0 fastmath<fast> : f32
// CHECK: llvm.frem %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
  %6 = arith.remf %arg0, %arg1 fastmath<fast> : f32
// CHECK: llvm.fsub %arg0, %arg1  {fastmathFlags = #llvm.fastmath<fast>} : f32
  %7 = arith.subf %arg0, %arg1 fastmath<fast> : f32
  return
}
