add_contract(dexchange dexchange ${CMAKE_CURRENT_SOURCE_DIR}/src/dexchange.cpp)

target_include_directories(dexchange
   PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(dexchange
   PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/ricardian/dexchange.contracts.md.in ${CMAKE_CURRENT_BINARY_DIR}/ricardian/dexchange.contracts.md @ONLY )

target_compile_options( dexchange PUBLIC -R${CMAKE_CURRENT_SOURCE_DIR}/ricardian -R${CMAKE_CURRENT_BINARY_DIR}/ricardian )
