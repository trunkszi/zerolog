include_guard(GLOBAL)

function(zerolog_enable_sanitizers target)
  if(ZEROLOG_ENABLE_SANITIZERS)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
  endif()
endfunction()
