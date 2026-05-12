file(GLOB LC3_SRC_FILES
    "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
)

idf_component_register(
    SRCS ${LC3_SRC_FILES}
    INCLUDE_DIRS "include" "src"
)

# Enable LC3plus short frames (2.5 ms and 5 ms). Keep HR mode off to
# avoid the larger high-resolution tables on ESP32.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LC3_PLUS=1
    LC3_PLUS_HR=0
)
