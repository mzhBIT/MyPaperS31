set(GMF_FFT_SRCS
    "src/common/fft_heap.c"
    "src/common/fft_power2_q15.c")

if(CONFIG_IDF_TARGET STREQUAL "esp32s3")
    set(GMF_FFT_TARGET_SRC "src/esp32s3/fft_pie_radix2_dit_s16.S")
elseif((CONFIG_IDF_TARGET STREQUAL "esp32s31") OR
       (CONFIG_IDF_TARGET STREQUAL "esp32p4"))
    # The RISC-V PIE assembly currently fails with newer riscv32-esp-elf
    # assemblers on ESP32-P4 because esp.lp.setup rejects local label
    # expressions such as (1f-4). Use the portable C fallback until the
    # upstream component ships a compatible assembly implementation.
    set(GMF_FFT_TARGET_SRC "src/esp_c/gmf_fft_radix2_dit_s16.c")
else()
    set(GMF_FFT_TARGET_SRC "src/esp_c/gmf_fft_radix2_dit_s16.c")
endif()

list(APPEND GMF_FFT_SRCS "${GMF_FFT_TARGET_SRC}")

idf_component_register(
    SRCS ${GMF_FFT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES heap
)
