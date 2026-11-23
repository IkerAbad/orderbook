if(NOT DEFINED ORDERBOOK OR NOT DEFINED OUT_DIR)
  message(FATAL_ERROR "ORDERBOOK and OUT_DIR are required")
endif()

set(map_csv "${OUT_DIR}/depth_map.csv")
set(pool_csv "${OUT_DIR}/depth_pool.csv")

execute_process(
  COMMAND "${ORDERBOOK}" depth --events 1000 --seed 1 --frames 5 --impl map
          --out "${map_csv}"
  RESULT_VARIABLE map_result
  OUTPUT_QUIET
  ERROR_QUIET)

execute_process(
  COMMAND "${ORDERBOOK}" depth --events 1000 --seed 1 --frames 5 --impl pool
          --out "${pool_csv}"
  RESULT_VARIABLE pool_result
  OUTPUT_QUIET
  ERROR_QUIET)

if(NOT map_result EQUAL 0 OR NOT pool_result EQUAL 0)
  message(FATAL_ERROR "the depth command failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${map_csv}" "${pool_csv}"
  RESULT_VARIABLE compare_result)

if(NOT compare_result EQUAL 0)
  message(FATAL_ERROR "map and pool depth outputs differ")
endif()
