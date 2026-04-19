file(GLOB LC3_SRC_FILES
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
)

idf_component_register(
    SRCS ${LC3_SRC_FILES}
    INCLUDE_DIRS "include" "src"
)

# Keep only Bluetooth LC3 feature set (no LC3plus short-frame/HR paths).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LC3_PLUS=0
    LC3_PLUS_HR=0
)
