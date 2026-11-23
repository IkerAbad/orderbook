if(NOT DEFINED ORDERBOOK OR NOT DEFINED OUT_DIR)
  message(FATAL_ERROR "ORDERBOOK and OUT_DIR are required")
endif()

set(first "${OUT_DIR}/generate_first.csv")
set(second "${OUT_DIR}/generate_second.csv")

execute_process(
  COMMAND "${ORDERBOOK}" generate --events 2000 --seed 7 --out "${first}"
  RESULT_VARIABLE first_result
  OUTPUT_QUIET
  ERROR_QUIET)

execute_process(
  COMMAND "${ORDERBOOK}" generate --events 2000 --seed 7 --out "${second}"
  RESULT_VARIABLE second_result
  OUTPUT_QUIET
  ERROR_QUIET)

if(NOT first_result EQUAL 0 OR NOT second_result EQUAL 0)
  message(FATAL_ERROR "the generate command failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${first}" "${second}"
  RESULT_VARIABLE compare_result)

if(NOT compare_result EQUAL 0)
  message(FATAL_ERROR "two runs with the same seed produced different traces")
endif()
