set(LLVM_TARGET_DEFINITIONS CGPasses.td)
mlir_tablegen(CGPasses.h.inc -gen-pass-decls -name OptCodeGen)
add_public_tablegen_target(FIROptCodeGenPassIncGen)

