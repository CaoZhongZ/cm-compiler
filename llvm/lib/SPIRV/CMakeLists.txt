include_directories( ${CMAKE_CURRENT_SOURCE_DIR}/libSPIRV
  ${CMAKE_CURRENT_SOURCE_DIR}/Mangler
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

option(SPIRV_USE_LLVM_API "Enable usage of LLVM API for libSPIRV." ON)
if ( SPIRV_USE_LLVM_API )
  add_definitions(-D_SPIRV_LLVM_API)
endif()

add_llvm_library(LLVMSPIRVLib
  libSPIRV/SPIRVBasicBlock.cpp
  libSPIRV/SPIRVDebug.cpp
  libSPIRV/SPIRVDecorate.cpp
  libSPIRV/SPIRVEntry.cpp
  libSPIRV/SPIRVFunction.cpp
  libSPIRV/SPIRVInstruction.cpp
  libSPIRV/SPIRVModule.cpp
  libSPIRV/SPIRVStream.cpp
  libSPIRV/SPIRVType.cpp
  libSPIRV/SPIRVValue.cpp
  Mangler/FunctionDescriptor.cpp
  Mangler/Mangler.cpp
  Mangler/ManglingUtils.cpp
  Mangler/ParameterType.cpp
  OCL20To12.cpp
  OCL20ToSPIRV.cpp
  OCL21ToSPIRV.cpp
  OCLTypeToSPIRV.cpp
  OCLUtil.cpp
  SPIRVLowerBool.cpp
  SPIRVLowerConstExpr.cpp
  SPIRVLowerOCLBlocks.cpp
  SPIRVLowerMemmove.cpp
  SPIRVReader.cpp
  SPIRVRegularizeLLVM.cpp
  SPIRVToOCL20.cpp
  SPIRVUtil.cpp
  SPIRVWriter.cpp
  SPIRVWriterPass.cpp
  TransOCLMD.cpp

  DEPENDS
  intrinsics_gen
  )
#add_dependencies(LLVMSPIRVLib intrinsics_gen)
