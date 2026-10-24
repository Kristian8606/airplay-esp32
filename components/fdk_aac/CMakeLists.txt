# Fraunhofer FDK AAC decoder (fdk-aac v2.0.3, github.com/mstorsjo/fdk-aac),
# decoder libraries only; see fdk-aac/NOTICE for the license.
#
# EXPERIMENT: used for 5.1/7.1 AAC, which esp_aac_dec (PacketVideo, stereo
# only) cannot decode. Built only with CONFIG_AIRPLAY_MULTICHANNEL; otherwise
# this registers an empty component so normal builds stay unchanged.

if(NOT CONFIG_AIRPLAY_MULTICHANNEL)
    idf_component_register()
    return()
endif()

set(FDK "${CMAKE_CURRENT_SOURCE_DIR}/fdk-aac")
set(FDK_LIBS
    libAACdec libArithCoding libDRCdec libFDK libMpegTPDec
    libPCMutils libSACdec libSBRdec libSYS)

set(FDK_SRCS "")
set(FDK_INCLUDES "")
foreach(lib ${FDK_LIBS})
    file(GLOB lib_srcs "${FDK}/${lib}/src/*.cpp")
    list(APPEND FDK_SRCS ${lib_srcs})
    list(APPEND FDK_INCLUDES "${FDK}/${lib}/include")
endforeach()

idf_component_register(
    SRCS ${FDK_SRCS}
    INCLUDE_DIRS "${FDK}/libAACdec/include" "${FDK}/libSYS/include"
    PRIV_INCLUDE_DIRS ${FDK_INCLUDES}
)

# Third-party code: keep its own warnings out of the build, and optimise for
# speed (7.1 must decode in well under one 21.3 ms frame).
target_compile_options(${COMPONENT_LIB} PRIVATE
    -w -O2 -fno-exceptions -fno-rtti)
