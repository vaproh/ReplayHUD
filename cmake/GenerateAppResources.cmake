foreach(required OUTPUT X64_HOOK X86_HOOK X86_INJECTOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required resource-generation argument: ${required}")
    endif()
endforeach()

foreach(input X64_HOOK X86_HOOK X86_INJECTOR)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "Embedded resource does not exist: ${${input}}")
    endif()
    file(TO_CMAKE_PATH "${${input}}" ${input}_RC)
    string(REPLACE "\"" "\\\"" ${input}_RC "${${input}_RC}")
endforeach()

file(WRITE "${OUTPUT}"
    "#include \"resource.h\"\n"
    "IDR_REPLAYHUD_HOOK_X64 RCDATA \"${X64_HOOK_RC}\"\n"
    "IDR_REPLAYHUD_HOOK_X86 RCDATA \"${X86_HOOK_RC}\"\n"
    "IDR_REPLAYHUD_INJECTOR_X86 RCDATA \"${X86_INJECTOR_RC}\"\n"
)
