file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/bt/host/bluedroid" BLUEDROID_ROOT)
file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/bt/common/include" BT_COMMON_INCLUDE)
file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/bt/common/osi/include" BT_OSI_INCLUDE)
set(SBC_ROOT "${BLUEDROID_ROOT}/external/sbc")

set(SBC_SRCS
    "${SBC_ROOT}/encoder/srce/sbc_analysis.c"
    "${SBC_ROOT}/encoder/srce/sbc_dct.c"
    "${SBC_ROOT}/encoder/srce/sbc_dct_coeffs.c"
    "${SBC_ROOT}/encoder/srce/sbc_enc_bit_alloc_mono.c"
    "${SBC_ROOT}/encoder/srce/sbc_enc_bit_alloc_ste.c"
    "${SBC_ROOT}/encoder/srce/sbc_enc_coeffs.c"
    "${SBC_ROOT}/encoder/srce/sbc_encoder.c"
    "${SBC_ROOT}/encoder/srce/sbc_packing.c"
    "${SBC_ROOT}/decoder/srce/bitalloc-sbc.c"
    "${SBC_ROOT}/decoder/srce/alloc.c"
    "${SBC_ROOT}/decoder/srce/bitalloc.c"
    "${SBC_ROOT}/decoder/srce/bitstream-decode.c"
    "${SBC_ROOT}/decoder/srce/decoder-oina.c"
    "${SBC_ROOT}/decoder/srce/decoder-private.c"
    "${SBC_ROOT}/decoder/srce/decoder-sbc.c"
    "${SBC_ROOT}/decoder/srce/dequant.c"
    "${SBC_ROOT}/decoder/srce/framing.c"
    "${SBC_ROOT}/decoder/srce/framing-sbc.c"
    "${SBC_ROOT}/decoder/srce/oi_codec_version.c"
    "${SBC_ROOT}/decoder/srce/synthesis-8-generated.c"
    "${SBC_ROOT}/decoder/srce/synthesis-dct8.c"
    "${SBC_ROOT}/decoder/srce/synthesis-sbc.c"
)

idf_component_register(
    SRCS ${SBC_SRCS}
    INCLUDE_DIRS
        "${SBC_ROOT}/encoder/include"
        "${SBC_ROOT}/decoder/include"
        "${BT_COMMON_INCLUDE}"
        "${BT_OSI_INCLUDE}"
        "${BLUEDROID_ROOT}/common/include"
        "${BLUEDROID_ROOT}/stack/include"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    SBC_ENC_INCLUDED=TRUE
    SBC_DEC_INCLUDED=TRUE
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-error
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
)
