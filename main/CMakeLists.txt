# The receiver firmware: this speaker's audio path (audio/), the sender's
# clock (network/), the glue the RTSP server and the web server call
# (sound_engine.*, web_receiver.c) and start-up (main.c). The AirPlay 2
# protocol, Wi-Fi, web server, settings, LED, logs and OTA are components in
# ../components.
set(SRC_FILES
    "main.c"
    "airplay_identity.c"
    "amp_control.c"
    "firmware_id.c"
    "device_reset.c"
    "sound_engine.c"
    "web_receiver.c"
    "audio/audio_receiver.c"
    "audio/ap2_buffered_fifo.c"
    "audio/audio_eq.c"
    "audio/audio_loudness.c"
    "audio/aac_decoder.c"
    "audio/alac_decoder.c"
    "audio/realtime_receiver.c"
    "audio/pcm_fifo.c"
    "audio/pcm_rtp_ring.c"
    "audio/audio_playout.c"
    "audio/audio_crypto.c"
    "network/apple_clock.c"
    "network/apple_clock_core.c"
    "network/ptp_clock.c"
)

if(CONFIG_AIRPLAY_HARDWARE_BUTTONS)
    list(APPEND SRC_FILES "playback_buttons.c")
endif()

# Diagnostics are a true compile-time feature. When both diagnostic
# categories that use it (PLAYOUT, LIFECYCLE) are off, audio_diag.c is not compiled.
if(CONFIG_AIRPLAY_DIAG_ACTIVE)
    list(APPEND SRC_FILES "audio/audio_diag.c")
endif()

# Output latency: the pure analysis is always built (tiny), the ADC
# capture only with the wired measurement option.
list(APPEND SRC_FILES "audio/latency_cal_dsp.c")
if(CONFIG_AIRPLAY_LATENCY_CAL)
    list(APPEND SRC_FILES "audio/latency_cal.c")
endif()

idf_component_register(
    SRCS ${SRC_FILES}
    INCLUDE_DIRS "." "audio"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_server
        app_update
        esp_app_format
        mbedtls
        esp_adc
        fdk_aac
        ap2_core
        helpers
        led
        log_stream
        ota
        settings
        web_server
        wifi
)

# Embed the UI into the OTA application image. File changes automatically
# regenerate the embedded data; no separate filesystem upload is needed.
foreach(page index logs speedtest eq)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/www/${page}.html" BINARY
        RENAME_TO "web_${page}_html")
endforeach()

# The RTSP server (ap2_core) and the web server call the audio engine, the
# clock, the amplifier, the identity and the pages of this component: give
# them its headers and link them with it.
foreach(lib ap2_core web_server)
    idf_component_get_property(lib_target ${lib} COMPONENT_LIB)
    target_include_directories(${lib_target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio"
        "${CMAKE_CURRENT_SOURCE_DIR}/network")
    target_link_libraries(${lib_target} PRIVATE ${COMPONENT_LIB})
endforeach()
