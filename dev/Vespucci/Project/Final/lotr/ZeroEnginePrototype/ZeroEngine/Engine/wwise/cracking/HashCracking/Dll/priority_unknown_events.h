// Priority Unknown Events for In-Game Identification
// Generated from gameplay log analysis (278,660 event plays)
// These are the most frequently played events that lack cracked names

#pragma once
#include <Windows.h>

// Priority Tier 1: High-frequency TXTP-named events (need identification)
// These have TXTP files so we know which bank they're from, but no semantic name
struct PriorityUnknownEvent {
    DWORD eventId;
    const char* txtpName;      // e.g., "Creatures-0442"
    int playCount;             // From gameplay logs
    const char* context;       // When does this play? (guessed from bank)
};

static const PriorityUnknownEvent g_PriorityUnknownEvents[] = {
    // From gameplay log analysis - sorted by play frequency
    {0, "Creatures-0442", 4455, "Creature vocalization?"},
    {0, "Effects-0740", 3073, "Combat effect?"},
    {0, "Effects-0738", 2890, "Combat effect?"},
    {0, "HeroLurtz-0026", 2220, "Lurtz combat sound"},
    {0, "BaseCombat-0748", 2122, "Combat sound"},
    {0, "SFXFellBeast-0075", 1661, "Fell beast sound"},
    {0, "Ambience-0114", 1475, "Ambient sound"},
    {0, "Level_Shire-0086", 753, "Shire level sound"},
    {0, "SFXBallista-0035", 615, "Ballista sound"},
    {0, "Creatures-0451", 584, "Creature vocalization"},
    {0, "SFXCatapult-0037", 573, "Catapult sound"},
    {0, "Level_HelmsDeep-0206", 549, "Helm's Deep level sound"},
    {0, "Creatures-0453", 530, "Creature vocalization"},
    {0, "Ambience-0118", 505, "Ambient sound"},
    {0, "Creatures-0449", 411, "Creature vocalization"},
    {0, "SFXCatapult-0052", 399, "Catapult sound"},
    {0, "Ambience-0102", 353, "Ambient sound"},
    {0, "BaseCombat-0750", 314, "Combat sound"},
    {0, "SFXCatapult-0050", 304, "Catapult sound"},
    {0, "BaseCombat-0767", 270, "Combat sound"},
    {0, "SFXTroll-0224", 246, "Troll sound"},
    {0, "Effects-0767", 242, "Effect"},
    {0, "Level_HelmsDeep-0208", 240, "Helm's Deep level sound"},
    {0, "Effects-0732", 238, "Effect"},
    {0, "SFXWarg-0145", 235, "Warg sound"},
    {0, "BaseCombat-0757", 175, "Combat sound"},
    {0, "Level_Isengard-0207", 166, "Isengard level sound"},
    {0, "Level_Isengard-0195", 165, "Isengard level sound"},
    {0, "Level_HelmsDeep-0226", 165, "Helm's Deep level sound"},
    {0, "SFXHorse-0139", 164, "Horse sound"},
    {0, "BaseCombat-0779", 148, "Combat sound"},
    {0, "BaseCombat-0742", 146, "Combat sound"},
    {0, "BaseCombat-0746", 139, "Combat sound"},
    {0, "UI-0066", 126, "UI sound"},
    {0, "Level_Isengard-0229", 117, "Isengard level sound"},
    {0, "HeroSauron-0095", 106, "Sauron combat sound"},
    {0, "SFXTroll-0238", 102, "Troll sound"},
    {0, "Effects-0803", 101, "Effect"},
    {0, "BaseCombat-0752", 99, "Combat sound"},
    {0, "BaseCombat-0771", 94, "Combat sound"},
};

static const int g_PriorityUnknownEventCount = sizeof(g_PriorityUnknownEvents) / sizeof(g_PriorityUnknownEvents[0]);

// 9 Stubborn Single-Event Banks (banks with only 1 uncracked event)
// These are priority targets for pattern-based attacks
struct StubbornBankEvent {
    DWORD eventId;
    const char* bankName;
    const char* notes;
};

static const StubbornBankEvent g_StubbornBankEvents[] = {
    {0xE234322F, "Ambience", "1 of 16 uncracked"},
    {0xDD7978E6, "Creatures", "1 of 15 uncracked"},
    {0xDCD9D5DD, "SFXSiegeTower", "1 of 5 uncracked"},
    {0xD1E41CDA, "SFXBalrog", "1 of 20 uncracked"},
    {0xDF91450F, "SFXOliphant", "1 of 8 uncracked"},
    {0xA6D835D7, "HeroSaruman", "1 of 3 uncracked"},
    {0xFF74FDE5, "HeroGimli", "1 of 4 uncracked"},
    {0xEF688F80, "HeroMouth", "1 of 2 uncracked"},
    {0x94BDA720, "Level_Isengard", "1 of 18 uncracked"},
};

static const int g_StubbornBankEventCount = sizeof(g_StubbornBankEvents) / sizeof(g_StubbornBankEvents[0]);

// Cracking Statistics
// Total events: 2,817
// Cracked: 1,325 (47.0%)
// Remaining: 1,492 (53.0%)
//
// Priority banks with 1-3 remaining:
// - 9 banks with 1 uncracked (listed above)
// - 9 banks with 2-3 uncracked (HeroLegolas, HeroIsildur, Music, etc.)
//
// Chatter/VO banks account for ~1,200 of remaining (voice line naming patterns)

