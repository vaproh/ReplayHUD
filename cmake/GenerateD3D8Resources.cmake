foreach(required OUTPUT HOOK)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required resource-generation argument: ${required}")
    endif()
endforeach()

if(NOT EXISTS "${HOOK}")
    message(FATAL_ERROR "Embedded D3D8 hook does not exist: ${HOOK}")
endif()

file(TO_CMAKE_PATH "${HOOK}" HOOK_RC)
string(REPLACE "\"" "\\\"" HOOK_RC "${HOOK_RC}")

file(WRITE "${OUTPUT}"
    "#include \"resource.h\"\n"
    "IDR_D3D8_REPLAYHUD_HOOK RCDATA \"${HOOK_RC}\"\n"
)
