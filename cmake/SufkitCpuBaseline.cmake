# Configure the private x86-64 instruction baseline used by sufkit.

include(CheckCXXCompilerFlag)

function(sufkit_enable_x86_64_sse42 target)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sufkit_processor)
    if(NOT _sufkit_processor MATCHES "^(amd64|x86_64|x64)$")
        return()
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()

    check_cxx_compiler_flag("-msse4.2" SUFKIT_COMPILER_HAS_MSSE42)
    if(NOT SUFKIT_COMPILER_HAS_MSSE42)
        message(FATAL_ERROR
            "sufkit requires compiler support for -msse4.2 on x86-64")
    endif()

    target_compile_options(${target} PRIVATE -msse4.2)
    target_compile_definitions(${target} PRIVATE
        SUFKIT_X86_64_SSE42_BASELINE=1)
endfunction()
