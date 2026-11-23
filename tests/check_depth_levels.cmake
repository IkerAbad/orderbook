if(NOT DEFINED ORDERBOOK OR NOT DEFINED OUT_DIR)
  message(FATAL_ERROR "ORDERBOOK and OUT_DIR are required")
endif()

set(csv "${OUT_DIR}/depth_levels.csv")

execute_process(
  COMMAND "${ORDERBOOK}" depth --events 1000 --seed 1 --frames 3 --levels 3
          --out "${csv}"
  RESULT_VARIABLE run_result
  OUTPUT_QUIET
  ERROR_QUIET)

if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "the depth command failed")
endif()

file(STRINGS "${csv}" lines)
set(rows 0)

foreach(line IN LISTS lines)
  if(line MATCHES "^[0-9]+,([BS]),([0-9]+),")
    math(EXPR rows "${rows} + 1")
    if(CMAKE_MATCH_2 GREATER_EQUAL 3)
      message(FATAL_ERROR "rank ${CMAKE_MATCH_2} is outside the requested levels")
    endif()
  endif()
endforeach()

if(rows EQUAL 0)
  message(FATAL_ERROR "the depth file has no data rows")
endif()
