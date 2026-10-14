# SPDX-License-Identifier: MIT
# xbox_nv2a_replay on a capture without and with --stream (wl_nv2a_replay_stream: the capture a piece
# at a time, the guest memory the caller's): the two outputs must be the same.
# cmake -DTOOL=<xbox_nv2a_replay> -DCAPTURE=<file> -DDIR=<folder for the PNGs> -P replay_stream.cmake
set(args ${CAPTURE} -v --png-gx ${DIR} --watch --textures-mb 1)
execute_process(COMMAND ${TOOL} ${args} OUTPUT_VARIABLE whole ERROR_VARIABLE whole_err RESULT_VARIABLE whole_rc)
execute_process(COMMAND ${TOOL} ${args} --stream OUTPUT_VARIABLE streamed ERROR_VARIABLE streamed_err
                RESULT_VARIABLE streamed_rc)
if(NOT whole_rc EQUAL 0 OR NOT streamed_rc EQUAL 0)
    message(FATAL_ERROR "the replay failed (${whole_rc}, ${streamed_rc}):\n${whole_err}\n${streamed_err}")
endif()
if(NOT whole STREQUAL streamed)
    message(FATAL_ERROR "--stream replayed differently:\n${whole}\n---- with --stream:\n${streamed}")
endif()
string(REGEX MATCH "recorder: digest [0-9a-f]+" digest "${streamed}")
message(STATUS "the same output with --stream (${digest})")
