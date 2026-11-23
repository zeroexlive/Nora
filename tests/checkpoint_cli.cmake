# Nora - checkpoint/resume across process runs with nora-cli.
#   cmake -DCLI=nora-cli -DCMP=imgcmp -DDIR=build/dir -P checkpoint_cli.cmake
# 1. render N mutations/px and save a checkpoint
# 2. resume it in a new process (different thread count) up to N+M
# 3. render N+M directly; 2 and 3 must agree (tolerance, float summation order)
# 4. resuming with another scene is refused; periodic checkpoints work; PT too
function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}\n${err}")
    endif()
    message(STATUS "${out}")
endfunction()

set(common --width 64 --height 64 --bootstrap 4000 --chains 16 -q)
file(REMOVE ${DIR}/ck_a.nrck ${DIR}/ck_int.nrck ${DIR}/ck_pt.nrck)
run(${CLI} cornell --spp 4 --threads 2 ${common} --checkpoint ${DIR}/ck_a.nrck -o ${DIR}/ck_n.pfm)
if(NOT EXISTS ${DIR}/ck_a.nrck)
    message(FATAL_ERROR "no checkpoint written")
endif()
run(${CLI} cornell --resume ${DIR}/ck_a.nrck --spp 10 --threads 3 -q -o ${DIR}/ck_resumed.pfm)
run(${CLI} cornell --spp 10 --threads 2 ${common} -o ${DIR}/ck_direct.pfm)
run(${CMP} ${DIR}/ck_resumed.pfm ${DIR}/ck_direct.pfm 0.001)

execute_process(COMMAND ${CLI} cornell-glass --resume ${DIR}/ck_a.nrck -q -o ${DIR}/ck_bad.pfm RESULT_VARIABLE rc ERROR_VARIABLE err)
if(rc EQUAL 0)
    message(FATAL_ERROR "resuming a checkpoint of another scene was not refused")
endif()
message(STATUS "refused as expected: ${err}")

run(${CLI} cornell --spp 24 --threads 2 ${common} --checkpoint ${DIR}/ck_int.nrck --checkpoint-interval 0.02 -o ${DIR}/ck_int.pfm)
run(${CLI} cornell --resume ${DIR}/ck_int.nrck -q -o ${DIR}/ck_int2.pfm)
run(${CMP} ${DIR}/ck_int2.pfm ${DIR}/ck_int.pfm 0.001)

run(${CLI} cornell --integrator pt --spp 2 --threads 2 ${common} --checkpoint ${DIR}/ck_pt.nrck -o ${DIR}/ck_pt_n.pfm)
run(${CLI} cornell --resume ${DIR}/ck_pt.nrck --spp 6 --threads 1 -q -o ${DIR}/ck_pt_resumed.pfm)
run(${CLI} cornell --integrator pt --spp 6 --threads 2 ${common} -o ${DIR}/ck_pt_direct.pfm)
run(${CMP} ${DIR}/ck_pt_resumed.pfm ${DIR}/ck_pt_direct.pfm 0.0001)
