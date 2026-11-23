# Nora - .nora files with nora-cli: save the Cornell box as a scene, render it,
# export OBJ and render that; check that a moved object changes the image.
function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}\n${err}")
    endif()
endfunction()
set(common --integrator pt --spp 4 --width 48 --height 48 --threads 2 -q)
run(${CLI} cornell --width 48 --height 48 --save-scene ${DIR}/sc_cornell.nora --no-render)
run(${CLI} ${DIR}/sc_cornell.nora ${common} -o ${DIR}/sc_from_nora.pfm)
run(${CLI} cornell ${common} -o ${DIR}/sc_builtin.pfm)
run(${CMP} ${DIR}/sc_from_nora.pfm ${DIR}/sc_builtin.pfm 0.0001)
# move the tall block in the scene file
file(READ ${DIR}/sc_cornell.nora text)
string(REPLACE "\"name\": \"Tall block\", \"source\": 0, \"mesh\": 6,\n      \"translation\": [0, 0, 0], \"rotation\": [0, 0, 0]"
               "\"name\": \"Tall block\", \"source\": 0, \"mesh\": 6,\n      \"translation\": [-40, 0, -60], \"rotation\": [0, 30, 0]" moved "${text}")
if(moved STREQUAL text)
    message(FATAL_ERROR "scene file layout changed: could not move the tall block")
endif()
file(WRITE ${DIR}/sc_moved.nora "${moved}")
run(${CLI} ${DIR}/sc_moved.nora ${common} -o ${DIR}/sc_moved.pfm)
execute_process(COMMAND ${CMP} ${DIR}/sc_moved.pfm ${DIR}/sc_builtin.pfm 0.02 RESULT_VARIABLE rc)
if(rc EQUAL 0)
    message(FATAL_ERROR "moving an object in the .nora file did not change the render")
endif()
# baked OBJ export renders like the scene it came from
run(${CLI} ${DIR}/sc_moved.nora --export-obj ${DIR}/sc_moved.obj --no-render -q)
run(${CLI} ${DIR}/sc_moved.obj ${common} --env 0 --fov 39.3077 -o ${DIR}/sc_obj.pfm)
