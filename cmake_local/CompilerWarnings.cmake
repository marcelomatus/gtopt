# ---- Compiler warning configuration ----
# Provides a function to enable compiler warnings on a target.

function(target_set_warnings target)
  set(CLANG_GCC_WARNINGS
      -Wall
      -Wpedantic
      -Wextra
      -Werror
      -Wno-c2y-extensions
      -Wno-deprecated-declarations
  )

  set(GCC_WARNINGS
      -Wno-c2y-extensions
  )

  set(MSVC_WARNINGS /W4 /WX)

  target_compile_options(
    ${target}
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:${CLANG_GCC_WARNINGS}>"
           "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:${GCC_WARNINGS}>"
           "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:${MSVC_WARNINGS}>"
  )

  target_compile_definitions(
    ${target}
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:_GLIBCXX_CISO646>"
  )
endfunction()
