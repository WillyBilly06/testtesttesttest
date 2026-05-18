file(GLOB LC3PLUS_FIXED_SRCS
    "src/*.c"
    "*.c"
    "basic_op/*.c"
)

list(FILTER LC3PLUS_FIXED_SRCS EXCLUDE REGEX ".*/codec_exe\\.c$")
list(FILTER LC3PLUS_FIXED_SRCS EXCLUDE REGEX ".*/ccConvert\\.c$")

idf_component_register(
    SRCS ${LC3PLUS_FIXED_SRCS}
    INCLUDE_DIRS "." "basic_op"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    DISABLE_HR_MODE
    SUBSET_ALL
    WMOPS=0
    DONT_COUNT_MEM
    NO_SCRATCH_STATS
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    rotl=lc3plus_fx_rotl
    rotr=lc3plus_fx_rotr
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -Wno-error
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-maybe-uninitialized
    -Wno-missing-field-initializers
)
