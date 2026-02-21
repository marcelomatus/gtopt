# ---- Compiler warning configuration ----
# Provides a function to enable compiler warnings on a target.

function(target_set_warnings target)
  set(CLANG_GCC_WARNINGS
    -Wall
    -Wpedantic
    -Wextra
    -Werror
    -Wno-deprecated-declarations
  )

  # Keep GCC-specific warnings here (do not add Clang-only flags to GCC).
  set(GCC_WARNINGS
  )

  set(MSVC_WARNINGS /W4 /WX)

  target_compile_options(
    ${target}
    PUBLIC
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:${CLANG_GCC_WARNINGS}>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:${GCC_WARNINGS}>"
    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:${MSVC_WARNINGS}>"
    # Silence C2y extension warnings only on modern Clang (>= 23).
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,23>>:-Wno-c2y-extensions>"
    # Clang < 23 raises -Winvalid-constexpr for C++26 constexpr patterns that are
    # valid in standard C++26 but not fully supported in earlier Clang versions.
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,23>>:-Wno-invalid-constexpr>"
  )

  target_compile_definitions(
    ${target}
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:_GLIBCXX_CISO646>"
    # Clang reports __cpp_concepts=201907L which is below the 202002L threshold
    # required by libstdc++14 to enable std::expected.  Force the feature-test
    # macro so that <expected> is available when compiling with Clang.
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>>:__cpp_lib_expected=202211L>"
  )
endfunction()
