set(SRC_FILES
    "main.c"
    "airplay_identity.c"
    "amp_control.c"
    "settings.c"
    "led.c"
    "audio/audio_receiver.c"
    "audio/ap2_buffered_fifo.c"
    "audio/audio_eq.c"
    "audio/aac_decoder.c"
    "audio/alac_decoder.c"
    "audio/realtime_receiver.c"
    "audio/pcm_rtp_ring.c"
    "audio/audio_playout.c"
    "audio/audio_crypto.c"
    "rtsp/rtsp_server.c"
    "rtsp/rtsp_conn.c"
    "rtsp/rtsp_message.c"
    "rtsp/rtsp_handlers.c"
    "rtsp/rtsp_fairplay.c"
    "rtsp/rtsp_crypto.c"
    "rtsp/rtsp_events.c"
    "rtsp/rtsp_remote.c"
    "rtsp/rtsp_rc.c"
    "plist/plist_xml.c"
    "plist/bplist_parser.c"
    "plist/bplist_builder.c"
    "plist/bplist_writer.c"
    "plist/base64.c"
    "hap/hap.c"
    "hap/hap_pair_verify.c"
    "hap/hap_pair_setup.c"
    "hap/hap_crypto.c"
    "hap/srp.c"
    "hap/tlv8.c"
    "network/wifi.c"
    "network/mdns_airplay.c"
    "network/ptp_clock.c"
    "network/ptp_clock_engine.c"
    "network/socket_utils.c"
    "network/web_server.c"
    "network/ota.c"
    "network/dns_server.c"
    "network/log_stream.c"
)


if(CONFIG_AIRPLAY_HARDWARE_BUTTONS)
    list(APPEND SRC_FILES "playback_buttons.c")
endif()

# Temporary diagnostics are a true compile-time feature. When the master
# switch is off, or all categories are off, audio_diag.c is not compiled.
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
    INCLUDE_DIRS "." "audio" "rtsp" "plist" "hap" "network"
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
)

# Embed the UI into the OTA application image. File changes automatically
# regenerate the embedded data; no separate filesystem upload is needed.
foreach(page index logs speedtest eq)
    target_add_binary_data(${COMPONENT_LIB}
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/www/${page}.html" BINARY
        RENAME_TO "web_${page}_html")
endforeach()
