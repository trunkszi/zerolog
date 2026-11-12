foreach(name IN ITEMS deleted_push deleted_resize invalid_flag invalid_field)
  zerolog_add_negative_test(zerolog_abi_${name} "abi/${name}.cc")
endforeach()

foreach(name IN ITEMS
    deleted_thread_buffer_copy
    deleted_lease_copy
    deleted_span_guard_move
    deleted_registry_copy)
  zerolog_add_negative_test(zerolog_core_${name} "core/${name}.cc")
endforeach()

foreach(name IN ITEMS raw_pointer reference union_type bit_field private_member)
  zerolog_add_negative_test(zerolog_encode_${name} "encode/${name}.cc")
endforeach()

foreach(name IN ITEMS arity bad_spec type_mismatch stray_brace unclosed)
  zerolog_add_negative_test(zerolog_format_${name} "format/${name}.cc")
endforeach()
