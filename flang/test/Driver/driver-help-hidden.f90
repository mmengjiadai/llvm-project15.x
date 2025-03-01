
!--------------------------
! FLANG DRIVER (flang-new)
!--------------------------
! RUN: %flang --help-hidden 2>&1 | FileCheck %s
! RUN: not %flang  -help-hidden 2>&1 | FileCheck %s --check-prefix=ERROR-FLANG

!----------------------------------------
! FLANG FRONTEND DRIVER (flang-new -fc1)
!----------------------------------------
! RUN: not %flang_fc1 --help-hidden 2>&1 | FileCheck %s --check-prefix=ERROR-FLANG-FC1
! RUN: not %flang_fc1  -help-hidden 2>&1 | FileCheck %s --check-prefix=ERROR-FLANG-FC1

! CHECK:USAGE: flang-new
! CHECK-EMPTY:
! CHECK-NEXT:OPTIONS:
! CHECK-NEXT: -###      Print (but do not run) the commands to run for this compilation
! CHECK-NEXT: -cpp      Enable predefined and command line preprocessor macros
! CHECK-NEXT: -c        Only run preprocess, compile, and assemble steps
! CHECK-NEXT: -D <macro>=<value>     Define <macro> to <value> (or 1 if <value> omitted)
! CHECK-NEXT: -emit-llvm             Use the LLVM representation for assembler and object files
! CHECK-NEXT: -E        Only run the preprocessor
! CHECK-NEXT: -falternative-parameter-statement
! CHECK-NEXT: Enable the old style PARAMETER statement
! CHECK-NEXT: -fapprox-func          Allow certain math function calls to be replaced with an approximately equivalent calculation
! CHECK-NEXT: -fbackslash            Specify that backslash in string introduces an escape character
! CHECK-NEXT: -fcolor-diagnostics    Enable colors in diagnostics
! CHECK-NEXT: -fconvert=<value>      Set endian conversion of data for unformatted files
! CHECK-NEXT: -fdefault-double-8     Set the default double precision kind to an 8 byte wide type
! CHECK-NEXT: -fdefault-integer-8    Set the default integer and logical kind to an 8 byte wide type
! CHECK-NEXT: -fdefault-real-8       Set the default real kind to an 8 byte wide type
! CHECK-NEXT: -ffast-math            Allow aggressive, lossy floating-point optimizations
! CHECK-NEXT: -ffixed-form           Process source files in fixed form
! CHECK-NEXT: -ffixed-line-length=<value>
! CHECK-NEXT: Use <value> as character line width in fixed mode
! CHECK-NEXT: -ffp-contract=<value> Form fused FP ops (e.g. FMAs)
! CHECK-NEXT: -ffree-form            Process source files in free form
! CHECK-NEXT: -fimplicit-none        No implicit typing allowed unless overridden by IMPLICIT statements
! CHECK-NEXT: -finput-charset=<value> Specify the default character set for source files
! CHECK-NEXT: -fintrinsic-modules-path <dir>
! CHECK-NEXT:                        Specify where to find the compiled intrinsic modules
! CHECK-NEXT: -flang-experimental-hlfir
! CHECK-NEXT:                        Use HLFIR lowering (experimental)
! CHECK-NEXT: -flang-experimental-polymorphism
! CHECK-NEXT:                        Enable Fortran 2003 polymorphism (experimental)
! CHECK-NEXT: -flarge-sizes          Use INTEGER(KIND=8) for the result type in size-related intrinsics
! CHECK-NEXT: -flogical-abbreviations Enable logical abbreviations
! CHECK-NEXT: -flto=<value> Set LTO mode
! CHECK-NEXT: -flto Enable LTO in 'full' mode
! CHECK-NEXT: -fno-automatic         Implies the SAVE attribute for non-automatic local objects in subprograms unless RECURSIVE
! CHECK-NEXT: -fno-color-diagnostics  Disable colors in diagnostics
! CHECK-NEXT: -fno-integrated-as     Disable the integrated assembler
! CHECK-NEXT: -fno-ppc-native-vector-element-order
! CHECK-NEXT:                        Specifies PowerPC non-native vector element order
! CHECK-NEXT: -fno-signed-zeros      Allow optimizations that ignore the sign of floating point zeros
! CHECK-NEXT: -fno-stack-arrays      Allocate array temporaries on the heap (default)
! CHECK-NEXT: -fno-version-loops-for-stride
! CHECK-NEXT:                        Do not create unit-strided loops (default)
! CHECK-NEXT: -fopenacc              Enable OpenACC
! CHECK-NEXT: -fopenmp-version=<value>
! CHECK-NEXT:                        Set OpenMP version (e.g. 45 for OpenMP 4.5, 50 for OpenMP 5.0). Default value is 50 for Clang and 11 for Flang
! CHECK-NEXT: -fopenmp               Parse OpenMP pragmas and generate parallel code.
! CHECK-NEXT: -foptimization-record-file=<file>
! CHECK-NEXT:                        Specify the output name of the file containing the optimization remarks. Implies -fsave-optimization-record. On Darwin platforms, this cannot be used with multiple -arch <arch> options.
! CHECK-NEXT: -foptimization-record-passes=<regex>
! CHECK-NEXT:                        Only include passes which match a specified regular expression in the generated optimization record (by default, include all passes)
! CHECK-NEXT: -fpass-plugin=<dsopath> Load pass plugin from a dynamic shared object file (only with new pass manager).
! CHECK-NEXT: -fppc-native-vector-element-order
! CHECK-NEXT:                        Specifies PowerPC native vector element order
! CHECK-NEXT: -freciprocal-math      Allow division operations to be reassociated
! CHECK-NEXT: -fsave-optimization-record=<format>
! CHECK-NEXT:                        Generate an optimization record file in a specific format
! CHECK-NEXT: -fsave-optimization-record
! CHECK-NEXT:                        Generate a YAML optimization record file
! CHECK-NEXT: -fstack-arrays         Attempt to allocate array temporaries on the stack, no matter their size
! CHECK-NEXT: -fsyntax-only          Run the preprocessor, parser and semantic analysis stages
! CHECK-NEXT: -funderscoring         Appends one trailing underscore to external names
! CHECK-NEXT: -fversion-loops-for-stride
! CHECK-NEXT:                        Create unit-strided versions of loops
! CHECK-NEXT: -fxor-operator         Enable .XOR. as a synonym of .NEQV.
! CHECK-NEXT: -gline-tables-only     Emit debug line number tables only
! CHECK-NEXT: -g                     Generate source-level debug information
! CHECK-NEXT: -help     Display available options
! CHECK-NEXT: -I <dir>               Add directory to the end of the list of include search paths
! CHECK-NEXT: -mllvm=<arg>           Alias for -mllvm
! CHECK-NEXT: -mllvm <value>         Additional arguments to forward to LLVM's option processing
! CHECK-NEXT: -mmlir <value>         Additional arguments to forward to MLIR's option processing
! CHECK-NEXT: -module-dir <dir>      Put MODULE files in <dir>
! CHECK-NEXT: -nocpp                 Disable predefined and command line preprocessor macros
! CHECK-NEXT: --offload-device-only   Only compile for the offloading device.
! CHECK-NEXT: --offload-host-device   Compile for both the offloading host and device (default).
! CHECK-NEXT: --offload-host-only     Only compile for the offloading host.
! CHECK-NEXT: -o <file> Write output to <file>
! CHECK-NEXT: -pedantic              Warn on language extensions
! CHECK-NEXT: -print-effective-triple Print the effective target triple
! CHECK-NEXT: -print-target-triple    Print the normalized target triple
! CHECK-NEXT: -P                     Disable linemarker output in -E mode
! CHECK-NEXT: -Rpass-analysis=<value> Report transformation analysis from optimization passes whose name matches the given POSIX regular expression
! CHECK-NEXT: -Rpass-missed=<value>   Report missed transformations by optimization passes whose name matches the given POSIX regular expression
! CHECK-NEXT: -Rpass=<value>          Report transformations performed by optimization passes whose name matches the given POSIX regular expression
! CHECK-NEXT: -R<remark>              Enable the specified remark
! CHECK-NEXT: -save-temps=<value>    Save intermediate compilation results.
! CHECK-NEXT: -save-temps            Save intermediate compilation results
! CHECK-NEXT: -std=<value>           Language standard to compile for
! CHECK-NEXT: -S                     Only run preprocess and compilation steps
! CHECK-NEXT: --target=<value>        Generate code for the given target
! CHECK-NEXT: -U <macro>             Undefine macro <macro>
! CHECK-NEXT: --version Print version information
! CHECK-NEXT: -W<warning>            Enable the specified warning
! CHECK-NEXT: -Xflang <arg>          Pass <arg> to the flang compiler

! ERROR-FLANG: error: unknown argument '-help-hidden'; did you mean '--help-hidden'?

! Frontend driver -help-hidden is not supported
! ERROR-FLANG-FC1: error: unknown argument: '{{.*}}'
