# Assemble and zip a binary release: run by the `release` target with cmake -P.
#
# Only the programs and banks are taken from the card folder, never the game data that
# extract.py may have written beside them: that data is copyrighted and not ours to ship.
#
# Inputs: SOURCE_DIR, CARD_DIR, OUT_DIR, NAME.

set(stage ${OUT_DIR}/${NAME})
file(REMOVE_RECURSE ${stage} ${OUT_DIR}/${NAME}.zip)
file(MAKE_DIRECTORY ${stage}/SIMON65)

file(GLOB programs ${CARD_DIR}/SIMON65.PRG ${CARD_DIR}/SIMON65M.PRG ${CARD_DIR}/BANK*.BIN)
list(LENGTH programs count)
if(count LESS 3)
  message(FATAL_ERROR "no programs and banks in ${CARD_DIR}")
endif()
file(COPY ${programs} DESTINATION ${stage}/SIMON65)
file(COPY ${SOURCE_DIR}/extract.py ${SOURCE_DIR}/transfer.py ${SOURCE_DIR}/README.md
     ${SOURCE_DIR}/LICENSE DESTINATION ${stage})

execute_process(
  COMMAND ${CMAKE_COMMAND} -E tar cf ${NAME}.zip --format=zip ${NAME}
  WORKING_DIRECTORY ${OUT_DIR}
  RESULT_VARIABLE failed)
if(failed)
  message(FATAL_ERROR "could not zip ${stage}")
endif()
message(STATUS "${OUT_DIR}/${NAME}.zip: ${count} programs and banks")
