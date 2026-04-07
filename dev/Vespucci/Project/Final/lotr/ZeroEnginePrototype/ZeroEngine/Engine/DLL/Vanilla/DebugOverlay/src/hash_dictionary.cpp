// Hash Dictionary Implementation
// Pre-computed FNV-1a hashes from 243-task Wwise RE analysis

#include "hash_dictionary.h"
#include <cstdio>
//Eriumsss
// ============================================================================
// Hero Ability Names (from English.json)
// ============================================================================
static const char* g_HeroAbilities[] = {
    // Sauron abilities
    "Heart of Horror", "Mordor Pound", "Soul Collector",
    // Aragorn abilities  
    "Anduril's Spirit", "Warrior's Bane", "Blade Dance",
    // Gandalf abilities
    "Healing Wisdom", "You shall not PASS", "Cleansing Fire",
    // Legolas abilities
    "Multi-Arrow", "Poison Arrow", "Fire Arrow",
    // Gimli abilities
    "Dwarven Run", "Balin's Revenge", "Longbeard Pound",
    // Saruman abilities
    "Heal", "Isengard Blast", "Fire Ball",
    // Mouth of Sauron abilities
    "Barad-dur Fury", "Firewall",
    // Nazgul abilities
    "Flurry of Terror", "Black Breath", "Power of the Nine",
    // Eowyn abilities
    "Ederas Dash", "Shield Maiden's Wrath", "Rohan's Fury",
    // Frodo abilities
    "Cloak", "Sting's Fury", "Blasting Powder",
    // Wormtongue abilities
    "Deception Strike", "Soul Punch",
    // Scout/Archer/Warrior generic
    "Caltrops", "Shockwave", "Fire Wall",
    // Boromir abilities
    "Guardian Strike", "Defender's Slam", "Gondor Charge",
    // Faramir abilities
    "Ranger's Fire", "Gondorian Courage", "Steward of Gondor",
    // Arwen abilities
    "Spirit Strike", "Ice Vortex", "Flood of Bruinen",
    // Elrond abilities
    "Vilya's Light", "Elven Wisdom",
    // Isildur abilities
    "Blade of the Mark", "Narsil Spike", "Dunharrow Dash",
    // Balrog abilities
    "Trample Attack", "Breath of Morgoth", "Valaraukar Smash",
    nullptr
};

// Common Wwise event prefixes
static const char* g_EventPrefixes[] = {
    "Play_", "Stop_", "Pause_", "Resume_", "Set_",
    "Play_Music_", "Stop_Music_", "Play_VO_", "Play_SFX_",
    nullptr
};

// Hero names for event generation
static const char* g_HeroNames[] = {
    "Sauron", "Aragorn", "Gandalf", "Legolas", "Gimli", "Frodo",
    "Saruman", "Nazgul", "Eowyn", "Elrond", "Isildur", "Boromir",
    "Faramir", "Arwen", "Lurtz", "WitchKing", "Wormtongue", "MouthOfSauron",
    "Theoden", "Gothmog", "Treebeard", "Balrog",
    nullptr
};

// Combat sound types
static const char* g_CombatTypes[] = {
    "attack", "swing", "hit", "impact", "block", "parry",
    "ability", "special", "combo", "grunt", "pain", "death",
    "footstep", "jump", "land", "roll", "dodge",
    nullptr
};

// Weapon sound types
static const char* g_WeaponSounds[] = {
    "sword_swing", "sword_hit", "sword_block",
    "mace_swing", "mace_hit", "mace_impact",
    "axe_swing", "axe_hit", "axe_chop",
    "bow_draw", "bow_fire", "arrow_fly", "arrow_hit",
    "staff_swing", "staff_magic", "staff_lightning",
    "spear_thrust", "spear_throw",
    "hammer_swing", "hammer_smash",
    nullptr
};

// UI sound types
static const char* g_UISounds[] = {
    "ui_advance", "ui_back", "ui_scroll", "ui_select", "ui_confirm",
    "ui_cancel", "ui_hover", "ui_click", "ui_open", "ui_close",
    "ui_error", "ui_success", "ui_notification",
    nullptr
};

// ============================================================================
// Build the complete hash dictionary
// ============================================================================
void BuildHashDictionary(std::unordered_map<DWORD, std::string>& outDict) {
    outDict.clear();
    
    // 1. Add switch groups and values
    for (int i = 0; g_SwitchDictionary[i].name; i++) {
        outDict[g_SwitchDictionary[i].hash] = g_SwitchDictionary[i].name;
    }
    
    // 2. Add RTPC parameters
    for (int i = 0; g_RTPCDictionary[i].name; i++) {
        outDict[g_RTPCDictionary[i].hash] = g_RTPCDictionary[i].name;
    }
    
    // 3. Add hero abilities (compute hashes)
    for (int i = 0; g_HeroAbilities[i]; i++) {
        DWORD hash = ComputeFNV1a(g_HeroAbilities[i]);
        outDict[hash] = g_HeroAbilities[i];
    }
    
    // 4. Generate hero event permutations
    for (int h = 0; g_HeroNames[h]; h++) {
        for (int c = 0; g_CombatTypes[c]; c++) {
            char buf[128];
            // Play_HeroName_CombatType
            sprintf_s(buf, "Play_%s_%s", g_HeroNames[h], g_CombatTypes[c]);
            outDict[ComputeFNV1a(buf)] = buf;
            
            // heroname_combattype (lowercase)
            char lower[128];
            sprintf_s(lower, "%s_%s", g_HeroNames[h], g_CombatTypes[c]);
            for (char* p = lower; *p; p++) {
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            }
            outDict[ComputeFNV1a(lower)] = lower;
        }
    }
    
    // 5. Add UI sounds
    for (int i = 0; g_UISounds[i]; i++) {
        outDict[ComputeFNV1a(g_UISounds[i])] = g_UISounds[i];
    }

    // 6. Add weapon sounds
    for (int i = 0; g_WeaponSounds[i]; i++) {
        outDict[ComputeFNV1a(g_WeaponSounds[i])] = g_WeaponSounds[i];
    }

    // 7. Add AbilityType values (hero-specific from Phase 4 TXTP analysis)
    // These are the 1214237073 (AbilityType) switch values seen in TXTPs
    static const struct { DWORD hash; const char* name; } abilityHashes[] = {
        {1149275623u, "ability_sauron_1"},     // HeroSauron ability
        {1754364802u, "ability_sauron_2"},
        {1942738266u, "ability_sauron_3"},
        {2496563745u, "ability_gandalf_1"},    // Gandalf abilities
        {2556104227u, "ability_gandalf_2"},
        {2638661999u, "ability_gandalf_3"},
        {2843359763u, "ability_aragorn_1"},    // Aragorn abilities
        {3573377653u, "ability_aragorn_2"},
        {3716224463u, "ability_aragorn_3"},
        {4179045307u, "ability_legolas_1"},    // Legolas abilities
        {43077781u, "ability_legolas_2"},
        {0, nullptr}
    };
    for (int i = 0; abilityHashes[i].name; i++) {
        outDict[abilityHashes[i].hash] = abilityHashes[i].name;
    }

    // 8. Add BaseCombat generic combat events (corrected from previous SFXTroll misattribution)
    // These are universal combat events that fire for all units, not troll-specific
    static const struct { DWORD hash; const char* name; } baseCombatHashes[] = {
        // Attack/swing events
        {0x0A40E38Eu, "hero_combat_grunt"},     // Attack grunt - shared across heroes (was HeroLurtz-0026)
        {0x8E3F67ADu, "swing"},                 // BaseCombat-0707 - melee weapon swing
        {0x4588BDDCu, "ability"},               // BaseCombat-0709 - ability activation
        {0xA5D460EAu, "stop_ability"},          // Confirmed - ability deactivation
        {0x1EF65DBBu, "ability_loop"},          // Sustained ability sound (looping)
        // Impact events
        {0xC2299AA7u, "impact"},                // BaseCombat-0722 - weapon hit flesh/armor
        {0xEAE2EB5Fu, "impact_kill"},           // BaseCombat-0724 - killing blow impact
        {0xE5513895u, "heavy_impact"},          // BaseCombat-0757 - heavy weapon impact
        {0x0C293F6Fu, "critical_hit"},          // BaseCombat-0777 - critical hit
        {0xA17FF5B5u, "armor_hit"},             // BaseCombat-0754 - hit on armor
        // Defense events
        {0xEE8347B0u, "block_prep"},            // BaseCombat-0705 - block preparation
        {0xC5B7CABAu, "block"},                 // BaseCombat-0711 - block success
        {0xC411DD8Du, "parry"},                 // BaseCombat-0714 - parry/deflect
        {0xBCD61A87u, "dodge"},                 // BaseCombat-0736 - dodge/evade
        {0xAA17BDA2u, "shield_block"},          // BaseCombat-0744 - shield block
        {0xBBB14F0Au, "miss"},                  // BaseCombat-0769 - miss/whiff
        // Reaction events
        {0x4BF68CF3u, "hit_react"},             // Pain/flinch sound on taking damage
        {0xB5EE54A3u, "stagger"},               // BaseCombat-0787 - stagger reaction
        {0x4FFFB616u, "ram_thud"},              // BaseCombat-0748 - battering ram impact (NOT body fall)
        {0x120427B6u, "explosion"},             // BaseCombat-0781 - explosion effect
        // Vocal events
        {0x94720482u, "attack_vocal"},          // Combat vocal (sparse)
        {0x887825CAu, "special_vocal"},         // Special attack vocal (rare)
        {0, nullptr}
    };
    for (int i = 0; baseCombatHashes[i].name; i++) {
        outDict[baseCombatHashes[i].hash] = baseCombatHashes[i].name;
    }

    // 9. Add SFXBallista switch values (from Isengard session)
    static const struct { DWORD hash; const char* name; } sfxBallistaHashes[] = {
        {0xD6454E24u, "ballista_fire"},         // Siege weapon shot
        {0, nullptr}
    };
    for (int i = 0; sfxBallistaHashes[i].name; i++) {
        outDict[sfxBallistaHashes[i].hash] = sfxBallistaHashes[i].name;
    }

    // 9b. Add Effects bank events (from log analysis)
    static const struct { DWORD hash; const char* name; } effectsHashes[] = {
        {0x440565D2u, "fire_effect"},           // Effects-0738 - fire/burning loop
        {0x71CDE2C1u, "magic_effect"},          // Effects-0740 - magic cast/impact
        {0x1B333D05u, "effect_generic"},        // Effects-0767 - misc environment effect
        {0x11F6B73Du, "effect_ambient"},        // Effects-0807 - ambient effect
        {0, nullptr}
    };
    for (int i = 0; effectsHashes[i].name; i++) {
        outDict[effectsHashes[i].hash] = effectsHashes[i].name;
    }

    // 9c. Add Creatures bank events (corrected from SFXWarg - Creatures.bnk always loaded)
    static const struct { DWORD hash; const char* name; } creaturesHashes[] = {
        {0xA166D0B2u, "creature_attack"},       // Creatures-0442 - attack connect
        {0x9B2C24E3u, "creature_swing"},        // Creatures-0438 - attack swing
        {0xC6480CCDu, "creature_ready"},        // Creatures-0436 - ready/prepare
        {0xC2200DBCu, "creature_vocal"},        // Creatures-0453 - vocal grunt
        {0xF9D7E402u, "creature_idle"},         // Creatures-0451 - idle sound
        {0x08DCD80Cu, "creature_death"},        // Creatures-0449 - death sound
        {0x79FE2517u, "creature_impact"},       // Creatures-0444 - impact hit
        {0xB733C1E9u, "creature_ability"},      // Creatures-0471 - special ability
        {0xC4F463C1u, "creature_special"},      // Creatures-0473 - special action
        {0xD6AB3876u, "creature_charge"},       // Creatures-0477 - charge/rush
        {0xDD7978E6u, "creature_hit"},          // Creatures-0469 - hit reaction
        {0xF6D937C0u, "creature_spawn"},        // Creatures-0440 - spawn/appear
        {0, nullptr}
    };
    for (int i = 0; creaturesHashes[i].name; i++) {
        outDict[creaturesHashes[i].hash] = creaturesHashes[i].name;
    }

    // 9d. Add Music bank events (from Moria log analysis)
    static const struct { DWORD hash; const char* name; } musicHashes[] = {
        {0xA91F0B79u, "stop_music"},            // Music stop command
        {0x9047881Du, "stop_all_but_music"},    // Stop all except music
        {0x7613E3A5u, "music_battle"},          // Battle music cue
        {0xA5D62C24u, "music_intense_1"},       // Intensity layer 1
        {0xA5D62C25u, "music_intense_2"},       // Intensity layer 2
        {0xA5D62C23u, "music_intense_3"},       // Intensity layer 3
        {0xA5D62C21u, "music_calm"},            // Calm/ambient music
        {0xA5D62C2Eu, "music_victory"},         // Victory music
        {0xA6D62D99u, "music_defeat"},          // Defeat music
        {0, nullptr}
    };
    for (int i = 0; musicHashes[i].name; i++) {
        outDict[musicHashes[i].hash] = musicHashes[i].name;
    }

    // 9e. Add Ambience/Level events (from log analysis)
    static const struct { DWORD hash; const char* name; } ambienceHashes[] = {
        {0xD38A2654u, "ambient_loop"},          // Ambience - background loop (212x in Moria)
        {0xDB63CE9Eu, "ambient_stop"},          // Ambience - stop ambient
        {0x42B067D4u, "moria_ambient"},         // Level_Moria - cave ambient
        {0x535EE5FDu, "moria_cave"},            // Level_Moria - cave sounds
        {0x522E28B2u, "osgiliath_ambient"},     // Level_Osgiliath - ambient
        {0xA4A8E159u, "osgiliath_battle"},      // Level_Osgiliath - battle ambience
        {0x7F7D0FB6u, "troll_vocal"},           // SFXTroll - grunt/roar
        {0, nullptr}
    };
    for (int i = 0; ambienceHashes[i].name; i++) {
        outDict[ambienceHashes[i].hash] = ambienceHashes[i].name;
    }

    // 10. Add WWiseIDTable.audio.json entries (97 readable event names from game engine)
    // These are the AUTHORITATIVE mappings from the game's audio ID table
    static const struct { DWORD hash; const char* name; } wwiseIDTable[] = {
        // Combat events
        {1166589404u, "ability"},              // play_ability = same val
        {2386519981u, "swing"},
        {2782159082u, "stop_ability"},
        {3257506471u, "impact"},
        {3289505165u, "ranged_attack_release"},
        {3317156538u, "ranged_attack_charge"},
        {3703028245u, "firewall"},
        {3940739935u, "impact_kill"},
        {4001580976u, "Block"},
        {2234217108u, "VOKill"},
        {2046698775u, "taunt"},
        {2603361507u, "attack_vocal"},
        {3326610637u, "creature_death"},
        {3478489869u, "Human"},
        {4141430720u, "cheer"},
        {17721092u, "Grab"},
        // State switches
        {552215692u, "Set_State_character_select"},
        {587181338u, "cp_transition"},
        {694741453u, "impact_wood"},
        {714367781u, "cp_idle"},
        {1297289809u, "impact_stone"},
        {1866025847u, "footstep"},
        {2630068541u, "Set_State_normal"},
        {2655823908u, "Set_State_ride_horse"},
        {2888473175u, "cp_capture"},
        {3449843094u, "set_state_inside_mage_bubble"},
        // Music/System events
        {275249087u, "unpause_game"},
        {528278262u, "pause_game"},
        {779980380u, "stop_music_now"},
        {1001038644u, "shell_amb"},
        {1074380525u, "front_end"},
        {1989942821u, "mus_good_and_evil"},
        {2420607005u, "stop_all_but_music"},
        {2574558660u, "Training"},
        {2837384057u, "stop_music"},
        {2932040671u, "Music"},
        {3992826691u, "mp_good"},
        {4211265950u, "mp_evil"},
        // Creature events
        {1364620465u, "Grab_Ent"},
        // UI events (from WWiseIDTable)
        {21508801u, "ui_previous"},
        {216067002u, "ui_confirm"},
        {1208240901u, "ui_scroll"},
        {1369595247u, "ui_reject"},
        {3083016892u, "ui_cancel"},
        {3767976236u, "ui_prompt"},
        {4291146320u, "ui_advance"},
        // VO
        {4246190473u, "VO_CQ_conquest_v1"},
        // Character types
        {748895195u, "NONE"},
        {1160234136u, "Normal"},
        {1560169506u, "boss"},
        {2381453861u, "Scout"},
        {3196610217u, "Hero"},
        // Hero names (switch values)
        {229480994u, "gandalf"},
        {295736344u, "nazgul"},
        {577318455u, "Ballista"},
        {1113661300u, "wormtongue"},
        {1188319105u, "elrond"},
        {1361160663u, "frodo"},
        {1509888341u, "treebeard"},
        {1537245886u, "saruman"},
        {1638501227u, "gimli"},
        {1650771784u, "lurtz"},
        {1873893292u, "Warg"},
        {1990701021u, "Catapult"},
        {2621374719u, "sauron"},
        {3053278946u, "legolas"},
        {3092697867u, "isildur"},
        {3094611314u, "theoden"},
        {3380422531u, "Eagle"},
        {3474813125u, "Oliphaunt"},
        {3583352285u, "haldir"},
        {3647848943u, "faramir"},
        {3769929459u, "witchking"},
        {3840621202u, "Balrog"},
        {3854705345u, "eowyn"},
        {3854847584u, "Horse"},
        {3887404748u, "Human"},
        {4141081574u, "gothmog"},
        {4197352807u, "aragorn"},
        {3529103590u, "Troll"},
        // Surface types
        {2195636714u, "dirt"},
        {2654748154u, "Water"},
        // Weapon types
        {645565787u, "punch"},
        {2181839183u, "kick"},
        // Faction
        {668632890u, "good"},
        {670611050u, "Neutral"},
        {4254973567u, "evil"},
        // Movement
        {2108779966u, "walk"},
        {3221406614u, "trot"},
        // Misc
        {3368360469u, "impact_size"},
        {3059984139u, "Tunnel"},
        {228274673u, "UI"},
        {349205203u, "Hero"},
        {559420513u, "VO"},
        {836183331u, "Music"},
        {0, nullptr}
    };
    for (int i = 0; wwiseIDTable[i].name; i++) {
        outDict[wwiseIDTable[i].hash] = wwiseIDTable[i].name;
    }

    // 11. Add bank names (from Organized_Final_AllLanguages XML extraction - 85 banks)
    static const struct { DWORD hash; const char* name; } bankHashes[] = {
        // ===== Core Banks =====
        {0x05173259u, "Ambience"},        // 85412153
        {0xd0d5925au, "BaseCombat"},      // 3503657562
        {0xd49de19du, "Creatures"},       // 3567116701
        {0x73cb32c9u, "Effects"},         // 1942696649
        {0xedea3a66u, "Music"},           // 3991942870
        {0x5c770db7u, "UI"},              // 1551306167
        {0xf0e6cc1bu, "VoiceOver"},       // 4041657371

        // ===== Hero SFX Banks =====
        {0xae7fdabbu, "HeroSauron"},      // 2927614651
        {0xd210ff9bu, "HeroAragorn"},     // 3524329371
        {0xb9842d06u, "HeroGandalf"},     // 3112447238
        {0x8a265726u, "HeroLegolas"},     // 2317768486
        {0x58f75bafu, "HeroGimli"},       // 1492605871
        {0xf7d0958du, "HeroElrond"},      // 4157642125
        {0xb659c16fu, "HeroIsildur"},     // 3059335535
        {0x714c77c2u, "HeroSaruman"},     // 1900836802
        {0x5c3485ecu, "HeroLurtz"},       // 1546946028
        {0x440af85cu, "HeroNazgul"},      // 1141569628
        {0x1888157fu, "HeroWitchKing"},   // 411571583
        {0x338d4cf0u, "HeroWormtongue"},  // 864989040
        {0xdc91750eu, "HeroMouth"},       // 3700520206
        {0xa8c61853u, "HeroFrodo"},       // 2831547475

        // ===== Hero Chatter Banks (Voice) =====
        {0x024f9ca2u, "ChatterHeroSauron"},      // 38771874
        {0x33efebb8u, "ChatterHeroAragorn"},     // 871361464 (corrected)
        {0xe4aa8bf9u, "ChatterHeroGandalf"},     // 3836382201
        {0xfe28c825u, "ChatterHeroLegolas"},     // 4264085541
        {0xee150c68u, "ChatterHeroGimli"},       // 3994356840
        {0x43e4f31eu, "ChatterHeroEowyn"},       // 1139077918
        {0x9cb6d54cu, "ChatterHeroElrond"},      // 2629227852
        {0x305a8aadu, "ChatterHeroSaruman"},     // 811240109
        {0x85abd25bu, "ChatterHeroLurtz"},       // 2242630235
        {0xa7af2725u, "ChatterHeroNazgul"},      // 2813273893
        {0xa67ca2e4u, "ChatterHeroWitchKing"},   // 2793186020
        {0xe9b8f871u, "ChatterHeroWormtongue"},  // 3921213553
        {0x5786c025u, "ChatterHeroMouth"},       // 1468448805
        {0x9f0e53e8u, "ChatterHeroFrodo"},       // 2669609192
        {0x9dd29b56u, "ChatterHeroEvilFrodo"},   // 2647976406
        {0x5e2d4a7cu, "ChatterHeroFaramir"},     // 1580398204
        {0x7adb1f25u, "ChatterHeroGothmog"},     // 2061718309
        {0x8a1c758du, "ChatterHeroTheoden"},     // 2317841293
        {0x0165789fu, "ChatterHeroBalrog"},      // 23438015

        // ===== Faction Chatter Banks =====
        {0xfb2ba08bu, "ChatterElf"},             // 4213940363
        {0xda5a8e19u, "ChatterEvilHuman"},       // 3664300569
        {0x566b960fu, "ChatterGondor"},          // 1449891919
        {0x29e1426au, "ChatterRohan"},           // 702631914
        {0xdd436cfau, "ChatterOrc"},             // 3712186106
        {0xbee17e69u, "ChatterUruk"},            // 3202456073
        {0xbd078c7cu, "ChatterHobbit"},          // 3171699836

        // ===== Level Banks =====
        {0xc3c20417u, "Level_BlackGates"},  // 3284272151
        {0x9dd2f9ebu, "Level_Trng"},        // 2647849451
        {0x95b242e3u, "Level_Isengard"},    // 2511487715
        {0x6dbaae75u, "Level_HelmsDeep"},   // 1840950901
        {0xb36d8f45u, "Level_MinasMorg"},   // 3010301765
        {0x19fe16f1u, "Level_MinasTir"},    // 436016369
        {0xa5656ae4u, "Level_Moria"},       // 2775270116
        {0x4d2b5754u, "Level_MountDoom"},   // 1294560980
        {0xeb65cea4u, "Level_Osgiliath"},   // 3948208292
        {0x3a28186fu, "Level_Pelennor"},    // 975722735
        {0x19e6ce43u, "Level_Rivendell"},   // 434439107
        {0x447fb7b1u, "Level_Shire"},       // 1148930993
        {0xaf60c6e5u, "Level_Weathertop"},  // 2943686885

        // ===== Voice-Over Banks =====
        {0xc463b59cu, "VO_BlackGates"},     // 3294868892
        {0x1629aef4u, "VO_Trng"},           // 371830516
        {0xd07a401cu, "VO_MinasMorg"},      // 3497672732
        {0x6f52a71au, "VO_MinasTir"},       // 1868043562
        {0xaa1f2d89u, "VO_Moria"},          // 2854391945
        {0x8a08d489u, "VO_MountDoom"},      // 2316267673
        {0x0283621du, "VO_Osgiliath"},      // 42175133
        {0x3c0ff274u, "VO_Pelennor"},       // 1007412724
        {0x028a0702u, "VO_Rivendell"},      // 42613634
        {0xafbb7b4cu, "VO_Shire"},          // 2947837516
        {0xa95e19aau, "VO_Weathertop"},     // 2840916394
        {0xc551c6c0u, "VO_HelmsDeep"},      // 3309576832
        {0xe3738990u, "VO_Isengard"},       // 3816387472

        // ===== Creature/Vehicle SFX Banks =====
        {0xa6c9c687u, "SFXTroll"},          // 2798241415
        {0xcdcb4fcdu, "SFXEnt"},            // 3452653517
        {0x4cfff4aau, "SFXEagle"},          // 1291842730
        {0x632d860au, "SFXFellBeast"},      // 1663927818
        {0xa1b662e0u, "SFXBallista"},       // 2713084640
        {0xdd9cf028u, "SFXBatteringRam"},   // 3717702568
        {0x213276eau, "SFXCatapult"},       // 556881898
        {0x81c4f06du, "SFXHorse"},          // 2177926797
        {0xc5478c9bu, "SFXOliphant"},       // 3309206939
        {0xfba5e84eu, "SFXSiegeTower"},     // 4222676942
        {0x09c86077u, "SFXWarg"},           // 164095351
        {0x970db169u, "SFXBalrog"},         // 2532441961

        // ===== Volume Controls (RTPC) =====
        {0xdc4c65b0u, "volume_master"},
        {0xe7f119bbu, "volume_music"},
        {0xdafafc77u, "volume_sfx"},
        {0xf80e9e43u, "volume_vo"},

        // ===== Game State Switches =====
        {0x0f940ccdu, "battle_size"},
        {0, nullptr}
    };
    for (int i = 0; bankHashes[i].name; i++) {
        outDict[bankHashes[i].hash] = bankHashes[i].name;
    }
}

