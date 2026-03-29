idf_component_register(
  SRCS
    "buildcheck_main.c"
  REQUIRES
    libbounce
)

target_compile_definitions(
  ${COMPONENT_LIB}
  PRIVATE
    BOUNCE_FREERTOS_ENABLE_FD_AWAIT=1
)
