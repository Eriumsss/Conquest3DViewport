#include "bnkparser.h"
#include <cstring>
#include <cstdio>

// ============================================================================
// BNK Parser Implementation
// Parses Wwise v34 soundbank data from memory
// ============================================================================
//Eriumsss
namespace BnkParser {

// Section header structure (common to all chunks)
#pragma pack(push, 1)
struct SectionHeader {
    char magic[4];
    DWORD size;
};

// BKHD section content (after header)
struct BkhdContent {
    DWORD version;
    DWORD bankId;
    // More fields follow but we don't need them
};

// DIDX entry (12 bytes each)
struct DidxEntry {
    DWORD id;
    DWORD offset;
    DWORD size;
};
#pragma pack(pop)

// Helper to find section in bank data
static const BYTE* FindSection(const BYTE* data, DWORD dataSize, const char* magic, DWORD* outSectionSize) {
    const BYTE* ptr = data;
    const BYTE* end = data + dataSize;
    
    while (ptr + sizeof(SectionHeader) <= end) {
        const SectionHeader* hdr = reinterpret_cast<const SectionHeader*>(ptr);
        
        if (memcmp(hdr->magic, magic, 4) == 0) {
            if (outSectionSize) *outSectionSize = hdr->size;
            return ptr + sizeof(SectionHeader);  // Return pointer to section content
        }
        
        // Move to next section
        ptr += sizeof(SectionHeader) + hdr->size;
    }
    
    return nullptr;
}

DWORD GetBankIdFromMemory(const BYTE* data, DWORD size) {
    if (!data || size < 16) return 0;
    
    // Verify BKHD magic
    if (memcmp(data, "BKHD", 4) != 0) return 0;
    
    const BkhdContent* bkhd = reinterpret_cast<const BkhdContent*>(data + 8);
    return bkhd->bankId;
}

bool GetBankNameFromMemory(const BYTE* data, DWORD size, char* outName, int maxLen) {
    if (!data || !outName || maxLen <= 0) return false;
    outName[0] = '\0';
    
    DWORD stidSize = 0;
    const BYTE* stid = FindSection(data, size, "STID", &stidSize);
    if (!stid || stidSize < 8) return false;
    
    // STID format: u32 count, then for each: u32 id, u8 strlen, char[strlen] name
    DWORD count = *reinterpret_cast<const DWORD*>(stid + 4);  // Skip first u32 (unknown)
    if (count == 0) return false;
    
    const BYTE* ptr = stid + 8;
    const BYTE* end = stid + stidSize;
    
    // Read first entry
    if (ptr + 5 > end) return false;
    // DWORD id = *reinterpret_cast<const DWORD*>(ptr);  // Not needed
    ptr += 4;
    BYTE strLen = *ptr++;
    
    if (ptr + strLen > end) return false;
    int copyLen = (strLen < maxLen - 1) ? strLen : maxLen - 1;
    memcpy(outName, ptr, copyLen);
    outName[copyLen] = '\0';
    
    return true;
}

bool ParseBankFromMemory(const BYTE* data, DWORD size, BankInfo& outInfo) {
    outInfo = BankInfo{};
    outInfo.isValid = false;
    
    if (!data || size < 16) return false;
    
    // Verify and parse BKHD
    if (memcmp(data, "BKHD", 4) != 0) return false;
    
    const BkhdContent* bkhd = reinterpret_cast<const BkhdContent*>(data + 8);
    outInfo.version = bkhd->version;
    outInfo.bankId = bkhd->bankId;
    
    // Parse STID for bank name
    char nameBuffer[64] = {0};
    if (GetBankNameFromMemory(data, size, nameBuffer, 64)) {
        outInfo.bankName = nameBuffer;
    }
    
    // Parse DIDX for WEM list
    DWORD didxSize = 0;
    const BYTE* didx = FindSection(data, size, "DIDX", &didxSize);
    if (didx && didxSize >= 12) {
        int wemCount = didxSize / 12;
        const DidxEntry* entries = reinterpret_cast<const DidxEntry*>(didx);
        for (int i = 0; i < wemCount; i++) {
            WemInfo wem;
            wem.id = entries[i].id;
            wem.offset = entries[i].offset;
            wem.size = entries[i].size;
            outInfo.wems.push_back(wem);
        }
    }
    
    // Parse HIRC for events and sounds
    DWORD hircSize = 0;
    const BYTE* hirc = FindSection(data, size, "HIRC", &hircSize);
    if (hirc && hircSize >= 4) {
        outInfo.hircObjectCount = *reinterpret_cast<const DWORD*>(hirc);
        const BYTE* ptr = hirc + 4;
        const BYTE* end = hirc + hircSize;
        
        // Parse each HIRC object (just count events for now)
        for (DWORD i = 0; i < outInfo.hircObjectCount && ptr + 5 <= end; i++) {
            BYTE type = *ptr++;
            DWORD objSize = *reinterpret_cast<const DWORD*>(ptr);
            ptr += 4;
            
            if (ptr + objSize > end) break;
            
            if (type == HIRC_EVENT && objSize >= 5) {
                EventInfo evt;
                evt.id = *reinterpret_cast<const DWORD*>(ptr);
                evt.actionCount = ptr[4];
                // Could parse action IDs here if needed
                outInfo.events.push_back(evt);
            }
            else if (type == HIRC_SOUND && objSize >= 14) {
                SoundInfo snd;
                snd.id = *reinterpret_cast<const DWORD*>(ptr);
                // Sound structure varies by version, skip details for now
                outInfo.sounds.push_back(snd);
            }
            
            ptr += objSize;
        }
    }
    
    outInfo.isValid = true;
    return true;
}

} // namespace BnkParser

