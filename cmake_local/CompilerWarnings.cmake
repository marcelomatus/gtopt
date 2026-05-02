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
    # Catch copies in range-based for loops (e.g. `for (const auto [a,b] : ...)`
    # where a reference would avoid a copy).  Not enabled by -Wall/-Wextra on
    # GCC; unknown availability on Clang, so keep it GCC-only for now.
    -Wrange-loop-construct

    # `-Wmaybe-uninitialized` is *included* in `-Wall` and is unreliable
    # at `-O2`/`-O3` on libstdc++ aggregate paths: GCC's flow analysis
    # cannot see through `std::string`'s `_M_dataplus._M_p` /
    # `_M_allocated_capacity` union arms or `std::optional::_M_engaged`
    # discriminator on aggregate-init paths, so it spams false-positive
    # `may be used uninitialized` errors under `-Werror` for any test
    # that does `Planning p { ... };` without explicitly setting every
    # std::string / std::optional member.  Disabled GCC-wide — the
    # warning is genuinely buggy at high optimisation levels and
    # libstdc++'s container default ctors are noexcept-correct so the
    # underlying memory is always initialised.  Clang's flow analysis
    # does not have this problem and keeps the warning enabled by
    # default.
    -Wno-maybe-uninitialized
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
