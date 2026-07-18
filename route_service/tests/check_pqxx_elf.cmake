# 本脚本验证 libpqxx 兼容层只本地化目标模板变量，不误隐藏普通入口符号。
foreach(BINARY IN ITEMS "${PQXX_COMPAT_BINARY}" "${POSTGRES_STORE_BINARY}")
  execute_process(
    COMMAND "${READELF}" --wide --syms "${BINARY}"
    RESULT_VARIABLE READELF_RESULT
    OUTPUT_VARIABLE SYMBOLS
    ERROR_VARIABLE READELF_ERROR
  )
  if(NOT READELF_RESULT EQUAL 0)
    message(FATAL_ERROR "读取 ELF 符号失败：${BINARY}：${READELF_ERROR}")
  endif()

  string(REGEX MATCHALL "[^\n]*_ZN4pqxx9type_name[^\n]*" TYPE_NAME_LINES "${SYMBOLS}")
  string(REGEX MATCHALL "[^\n]*_ZGVN4pqxx9type_name[^\n]*" GUARD_LINES "${SYMBOLS}")
  if(NOT TYPE_NAME_LINES OR NOT GUARD_LINES)
    message(FATAL_ERROR "ELF 缺少 pqxx::type_name 或 guard 符号：${BINARY}")
  endif()
  foreach(LINE IN LISTS TYPE_NAME_LINES GUARD_LINES)
    if(NOT LINE MATCHES "[ \\t]LOCAL[ \\t]+DEFAULT[ \\t]")
      message(FATAL_ERROR "pqxx::type_name 符号未本地化：${BINARY}：${LINE}")
    endif()
  endforeach()

  string(REGEX MATCHALL "[^\n]*[ \\t]main[^\n]*" MAIN_LINES "${SYMBOLS}")
  if(NOT MAIN_LINES MATCHES "[ \\t]GLOBAL[ \\t]+DEFAULT[ \\t]")
    message(FATAL_ERROR "兼容层误隐藏关键入口符号 main：${BINARY}")
  endif()
endforeach()
