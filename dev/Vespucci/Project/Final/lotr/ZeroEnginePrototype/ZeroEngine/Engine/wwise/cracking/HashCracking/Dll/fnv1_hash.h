// fnv1_hash.h - Wwise 2008 (v2.1.2821) compatible hashing helpers
// Shared by native DLL and brute-force tools.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifdef __cplusplus
    #define FNV_EXPORT extern "C" __declspec(dllexport)
  #else
    #define FNV_EXPORT __declspec(dllexport)
  #endif
#else
  #ifdef __cplusplus
    #define FNV_EXPORT extern "C" __attribute__((visibility("default")))
  #else
    #define FNV_EXPORT __attribute__((visibility("default")))
  #endif
#endif

// Constants from AkFNVHash.h / AkAudioLib::GetIDFromString
enum {
    FNV_OFFSET_32 = 2166136261u,
    FNV_PRIME_32  = 16777619u,
    FNV_INVERSE_32 = 0x359C449B,      // Modular inverse of PRIME mod 2^32
    FNV_HASH30_MASK = 0x3FFFFFFFu,    // XOR-fold mask for Hash30 variant
    FNV_HASH_NONE = 748895195u        // HashName("none") sanity check
};

#define AK_HASH_STATE_NONE FNV_HASH_NONE   // Matches AkAudioLib.cpp constant
#define WWISE_SDK_VERSION  "v2008.2.1 (Build 2821)"

// Core APIs (ASCII only; caller responsible for encoding)
FNV_EXPORT uint32_t wwise_hash(const char* s);
FNV_EXPORT uint32_t wwise_hash_len(const char* s, int len);
FNV_EXPORT uint32_t wwise_hash_bytes(const uint8_t* bytes, size_t len, int normalize_ascii);
FNV_EXPORT uint32_t wwise_hash_continue(uint32_t prev_hash, const char* s);
FNV_EXPORT uint32_t wwise_hash_inverse(uint32_t target_hash, const char* suffix, int len);
FNV_EXPORT uint32_t wwise_hash_target_with_suffix(uint32_t target_hash, const char* suffix);

// Wide-character helper (mirrors AkAudioLib: wide -> ANSI -> lowercase -> hash)
FNV_EXPORT uint32_t wwise_hash_w(const wchar_t* ws);

// Hash30 helpers
FNV_EXPORT uint32_t wwise_hash30(const char* s);
FNV_EXPORT uint32_t wwise_hash32_to_30(uint32_t h32);

// Self-test: returns 1 on pass, 0 on fail
FNV_EXPORT int wwise_hash_selftest(void);
