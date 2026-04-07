"""
Canonical Wwise 2008 (SDK v2.1.2821) hashing helpers.

Matches AkAudioLib::GetIDFromString / HashName in
SDK/source/SoundEngine/AkAudioLib/Common/AkAudioLib.cpp:
1) Wide string -> ANSI
2) ASCII‑only lowercase (A-Z -> a-z)
3) FNV-1 32‑bit (multiply then XOR), offset 2166136261, prime 16777619
4) Optional XOR-fold for smaller bit widths is unused in Conquest.
"""

from __future__ import annotations

import os
import sys
import ctypes
from pathlib import Path
from typing import Iterable

# Constants straight from AkFNVHash.h
FNV_PRIME: int = 16777619
FNV_OFFSET: int = 2166136261

# Modular inverse of FNV_PRIME mod 2^32 (used for suffix/backward search)
FNV_INVERSE: int = 0x359C449B  # 899433627

# Optional native DLL/SO (fnv1_hash) for maximum speed; falls back to Python.
_DLL_PATH = (
    Path(__file__).resolve().parents[1]
    / "Dll"
    / ("fnv1_hash.dll" if os.name == "nt" else "fnv1_hash.so")
)
_use_native = False
_native_hash = None
_native_hash_continue = None
_native_hash_inverse = None
_native_hash_len = None

if _DLL_PATH.exists():
    try:
        _dll = ctypes.CDLL(str(_DLL_PATH))
        _dll.wwise_hash.restype = ctypes.c_uint32
        _dll.wwise_hash.argtypes = [ctypes.c_char_p]

        _dll.wwise_hash_continue.restype = ctypes.c_uint32
        _dll.wwise_hash_continue.argtypes = [ctypes.c_uint32, ctypes.c_char_p]

        _dll.wwise_hash_inverse.restype = ctypes.c_uint32
        _dll.wwise_hash_inverse.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_int]

        _dll.wwise_hash_len.restype = ctypes.c_uint32
        _dll.wwise_hash_len.argtypes = [ctypes.c_char_p, ctypes.c_int]

        _native_hash = _dll.wwise_hash
        _native_hash_continue = _dll.wwise_hash_continue
        _native_hash_inverse = _dll.wwise_hash_inverse
        _native_hash_len = _dll.wwise_hash_len
        _use_native = True
    except Exception:
        _use_native = False


def _lower_ascii_bytes(data: bytes) -> bytes:
    """Lowercase ASCII A-Z only; leave everything else intact (mirrors _MakeLower)."""
    return bytes((b + 32) if 65 <= b <= 90 else b for b in data)


def fnv1_hash_bytes(data: bytes) -> int:
    """FNV-1 on already-lowercased bytes."""
    if _use_native and _native_hash_len:
        return int(_native_hash_len(data, len(data)))
    h = FNV_OFFSET
    for b in data:
        h = (h * FNV_PRIME) & 0xFFFFFFFF
        h ^= b
    return h


def fnv1_hash(text: str | bytes, normalize_ascii: bool = True) -> int:
    """
    Hash a string exactly like Wwise:
    - Wide -> ANSI assumed already
    - Lowercase ASCII only when normalize_ascii is True
    """
    if isinstance(text, str):
        raw = text.encode("latin-1", errors="ignore")
    else:
        raw = text
    if normalize_ascii:
        raw = _lower_ascii_bytes(raw)
    if _use_native and _native_hash:
        return int(_native_hash(raw))
    return fnv1_hash_bytes(raw)


def fnv1_continue(prev_hash: int, text: str | bytes, normalize_ascii: bool = True) -> int:
    """Continue hashing from an existing state (prefix caching)."""
    if isinstance(text, str):
        raw = text.encode("latin-1", errors="ignore")
    else:
        raw = text
    if normalize_ascii:
        raw = _lower_ascii_bytes(raw)
    if _use_native and _native_hash_continue:
        return int(_native_hash_continue(prev_hash & 0xFFFFFFFF, raw))
    h = prev_hash & 0xFFFFFFFF
    for b in raw:
        h = (h * FNV_PRIME) & 0xFFFFFFFF
        h ^= b
    return h


def fnv1_inverse_step(current_hash: int, last_byte: int) -> int:
    """
    Reverse one FNV-1 step (used for suffix search):
    h_{n-1} = ((h_n ^ byte) * FNV_INVERSE) mod 2^32
    """
    return ((current_hash ^ last_byte) * FNV_INVERSE) & 0xFFFFFFFF


def hash_none_test() -> bool:
    """Sanity check against SDK constant: HashName(\"none\") == 748895195."""
    return fnv1_hash("none") == 748895195


__all__ = [
    "FNV_PRIME",
    "FNV_OFFSET",
    "FNV_INVERSE",
    "fnv1_hash",
    "fnv1_hash_bytes",
    "fnv1_continue",
    "fnv1_inverse_step",
    "hash_none_test",
]
