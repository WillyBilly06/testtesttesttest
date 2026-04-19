idf_component_register(
    SRCS "minimp3_impl.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
)

# ESP32 flash (IROM/DROM) only supports 32-bit aligned reads.
# minimp3 does byte-level access on static-const lookup tables → LoadStoreError.
# The linker fragment (linker.lf) places all of minimp3's code+rodata in IRAM/DRAM.