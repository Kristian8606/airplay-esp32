#pragma once

// AirPlay feature flags = base | bits selected in menuconfig ("AirPlay
// feature bits (experimental)"). Base 0x14340405C4A00: Shairport Sync's
// AirPlay 2 bits without bit 47, plus bit 46 (HomePod presentation). Key bits:
//   Bit 38: SupportsUnifiedMediaControl
//   Bit 40: SupportsBufferedAudio, Bit 41: SupportsPTP
//   Bit 46: SupportsHKPairingAndAccessControl
//   Bit 48: SupportsCoreUtilsPairingAndEncryption
// Bits 64+ are published only in the "fex" TXT key.
#include "sdkconfig.h"
#define AIRPLAY_FEATURES_BASE 0x00014340405C4A00ULL
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_15
#define AP_FB15 (1ULL << 15)
#else
#define AP_FB15 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_16
#define AP_FB16 (1ULL << 16)
#else
#define AP_FB16 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_17
#define AP_FB17 (1ULL << 17)
#else
#define AP_FB17 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_21
#define AP_FB21 (1ULL << 21)
#else
#define AP_FB21 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_25
#define AP_FB25 (1ULL << 25)
#else
#define AP_FB25 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_27
#define AP_FB27 (1ULL << 27)
#else
#define AP_FB27 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_36
#define AP_FB36 (1ULL << 36)
#else
#define AP_FB36 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_39
#define AP_FB39 (1ULL << 39)
#else
#define AP_FB39 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_43
#define AP_FB43 (1ULL << 43)
#else
#define AP_FB43 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_45
#define AP_FB45 (1ULL << 45)
#else
#define AP_FB45 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_47
#define AP_FB47 (1ULL << 47)
#else
#define AP_FB47 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_50
#define AP_FB50 (1ULL << 50)
#else
#define AP_FB50 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_52
#define AP_FB52 (1ULL << 52)
#else
#define AP_FB52 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_53
#define AP_FB53 (1ULL << 53)
#else
#define AP_FB53 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_58
#define AP_FB58 (1ULL << 58)
#else
#define AP_FB58 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_59
#define AP_FB59 (1ULL << 59)
#else
#define AP_FB59 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_60
#define AP_FB60 (1ULL << 60)
#else
#define AP_FB60 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_61
#define AP_FB61 (1ULL << 61)
#else
#define AP_FB61 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_68
#define AP_FB68 (1ULL << 4)
#else
#define AP_FB68 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_72
#define AP_FB72 (1ULL << 8)
#else
#define AP_FB72 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_77
#define AP_FB77 (1ULL << 13)
#else
#define AP_FB77 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_79
#define AP_FB79 (1ULL << 15)
#else
#define AP_FB79 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_81
#define AP_FB81 (1ULL << 17)
#else
#define AP_FB81 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_82
#define AP_FB82 (1ULL << 18)
#else
#define AP_FB82 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_83
#define AP_FB83 (1ULL << 19)
#else
#define AP_FB83 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_85
#define AP_FB85 (1ULL << 21)
#else
#define AP_FB85 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_87
#define AP_FB87 (1ULL << 23)
#else
#define AP_FB87 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_91
#define AP_FB91 (1ULL << 27)
#else
#define AP_FB91 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_92
#define AP_FB92 (1ULL << 28)
#else
#define AP_FB92 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_94
#define AP_FB94 (1ULL << 30)
#else
#define AP_FB94 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_95
#define AP_FB95 (1ULL << 31)
#else
#define AP_FB95 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_96
#define AP_FB96 (1ULL << 32)
#else
#define AP_FB96 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_98
#define AP_FB98 (1ULL << 34)
#else
#define AP_FB98 0ULL
#endif
#ifdef CONFIG_AIRPLAY_FEATURE_BIT_99
#define AP_FB99 (1ULL << 35)
#else
#define AP_FB99 0ULL
#endif
#if defined(CONFIG_AIRPLAY_FEATURES_HOMEPOD_MINI)
/* Exactly what a HomePod mini on software 27.2 advertises:
 * features=0x4A7FCA00,0x3C356BD0, fex=AMp/StBrNTwQoa7YDQ. */
#define AIRPLAY_FEATURES 0x3C356BD04A7FCA00ULL
#define AIRPLAY_FEATURES_EX 0x0000000DD8AEA110ULL
#else
#define AIRPLAY_FEATURES \
  (AIRPLAY_FEATURES_BASE | \
    AP_FB15 | AP_FB16 | AP_FB17 | AP_FB21 | AP_FB25 | AP_FB27 | AP_FB36 | \
    AP_FB39 | AP_FB43 | AP_FB45 | AP_FB47 | AP_FB50 | AP_FB52 | AP_FB53 | \
    AP_FB58 | AP_FB59 | AP_FB60 | AP_FB61)
#define AIRPLAY_FEATURES_EX \
  (0ULL | \
    AP_FB68 | AP_FB72 | AP_FB77 | AP_FB79 | AP_FB81 | AP_FB82 | AP_FB83 | \
    AP_FB85 | AP_FB87 | AP_FB91 | AP_FB92 | AP_FB94 | AP_FB95 | AP_FB96 | \
    AP_FB98 | AP_FB99)
#endif
#define AIRPLAY_FEATURES_HI ((unsigned)(AIRPLAY_FEATURES >> 32))
#define AIRPLAY_FEATURES_LO ((unsigned)(AIRPLAY_FEATURES & 0xFFFFFFFFULL))
