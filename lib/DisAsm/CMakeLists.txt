project(disasm)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

if (WIN32)
  # Create .def file containing a list of exports preceeded by
  # 'EXPORTS'.  The file "disasm.exports" already contains the list, so we
  # massage it into the correct format here to create "disasm.exports.def".
  set(DISASM_EXPORTS_DEF ${CMAKE_CURRENT_BINARY_DIR}/disasm.exports.def)
  set(DISASM_EXPORTS_DEF_TEMP ${DISASM_EXPORTS_DEF}.txt)
  file(READ "disasm.exports" exports_list)
  file(WRITE ${DISASM_EXPORTS_DEF_TEMP} "LIBRARY DISASM\n")
  file(APPEND ${DISASM_EXPORTS_DEF_TEMP} "EXPORTS\n")
  file(APPEND ${DISASM_EXPORTS_DEF_TEMP} ${exports_list})
  # Copy the file only if it has changed.
  execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${DISASM_EXPORTS_DEF_TEMP} ${DISASM_EXPORTS_DEF})
endif()

# Now build our tools
add_library(disasm
   SHARED
   disasm.cpp
   ${DISASM_EXPORTS_DEF}
)

# Find the libraries that correspond to the LLVM components
# that we wish to use

llvm_map_components_to_libnames(llvm_libs
  ${LLVM_TARGETS_TO_BUILD}
)

# Link against LLVM libraries
target_link_libraries(disasm
${llvm_libs})

