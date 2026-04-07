#!/usr/bin/env python3
"""
ADVANCED Wwise Event Name Brute-Forcer v3.0

INTEGRATED TECHNIQUES FROM RESEARCH:
1. Inverse FNV optimization - suffix searches as fast as prefix
2. N-gram filtering - skip impossible 3-char sequences (~90% reduction)
3. Fuzzy hash early-exit - check upper 24 bits first
4. Meet-in-the-middle attack - O(2^(n/2)) for medium names
5. Pattern-based generation - play_%s, stop_%s patterns
6. Dictionary combination - multi-word stem joining
7. Prefix hash caching - avoid recomputation
8. Native C acceleration - 3-10x faster than pure Python
9. Wwise charset rules - first char [a-z], rest [a-z0-9_] (from FnvBrute)
10. Roman numeral patterns - _I, _II, _III, _IV, _V (from WwiseNameCracker)
11. Pattern recognition - Word, Number, WordNumber types (from WwiseNameCracker)
12. Bidirectional search - O(37^n + 37^m) vs O(37^(n+m)) (from wwiser-utils #7)

Based on research from:
- bnnm/wwiser-utils (fnv.c, words.py, NAMES.md, issue #7)
- davispuh/WwiseNameCracker (D language pattern cracker)
- xyx0826/FnvBrute (C# multi-threaded brute force)
- Audiokinetic Wwise SDK (AkFNVHash.h official implementation)
- Mandiant SUNBURST countermeasures
- Official FNV spec: http://www.isthe.com/chongo/tech/comp/fnv/

FNV ALGORITHM DETAILS (from Landon Curt Noll's official spec):
- FNV-1:  hash = (hash * prime) ^ byte   (multiply-then-XOR) <- WWISE USES THIS
- FNV-1a: hash = (hash ^ byte) * prime   (XOR-then-multiply)
- 32-bit prime: 16777619 = 2^24 + 2^8 + 0x93
- 32-bit offset: 2166136261 = FNV-0 hash of "chongo <Landon Curt Noll> /\\..\\"
- XOR-fold for smaller hashes: (hash >> bits) ^ (hash & mask)
- Shift-add optimization: h*prime = h + (h<<1) + (h<<4) + (h<<7) + (h<<8) + (h<<24)

ZERO-HASH CHALLENGE RESULTS (useful for collision analysis):
- Shortest 32-bit FNV-1 binary collision: 5 bytes (254 solutions exist)
- Shortest 32-bit FNV-1a binary collision: 4 bytes (2 solutions exist)
- Shortest 32-bit alphanumeric collision: 6 chars

Author: LOTR Conquest RE Project
"""

import os
import sys
import json
import time
import ctypes
import struct
import argparse
import itertools
import multiprocessing as mp
from pathlib import Path
from datetime import datetime, timedelta
from typing import Set, Dict, List, Tuple, Optional
from dataclasses import dataclass
from collections import defaultdict

# Ensure shared hash helpers (built from official Wwise SDK) are importable
SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import (  # type: ignore  # runtime path injection
    FNV_OFFSET,
    FNV_PRIME,
    FNV_INVERSE,
    fnv1_hash as _fnv1_hash,
    fnv1_continue as _fnv1_continue,
    fnv1_inverse_step,
)

# ============================================================================
# MATRIX-STYLE VERBOSE LOGGING
# ============================================================================
VERBOSE = True  # Global verbose flag

def log(msg: str, level: int = 0):
    """Matrix-style logging - dumps everything to console."""
    if not VERBOSE:
        return
    indent = "  " * level
    timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"[{timestamp}] {indent}{msg}", flush=True)

def log_progress(current: int, total: int, prefix: str = "", every: int = 1000):
    """Log progress every N iterations."""
    if current % every == 0 or current == total:
        pct = (current / total * 100) if total > 0 else 0
        log(f"{prefix} {current:,}/{total:,} ({pct:.1f}%)")

def log_match(name: str, hash_val: int, bank: str = ""):
    """Log a match in green (simulated with emphasis)."""
    bank_str = f" [{bank}]" if bank else ""
    print(f"[MATCH] >>> 0x{hash_val:08X} = '{name}'{bank_str} <<<", flush=True)

# ============================================================================
# CONFIGURATION (from official Audiokinetic Wwise SDK AkFNVHash.h)
# Values imported from shared wwise_hash helpers.
# ============================================================================
HASH30_MASK = 0x3FFFFFFF     # For Hash30 XOR-fold variant

# Wwise charset rules (from FnvBrute/Audiokinetic SDK):
# - First character MUST be lowercase letter [a-z]
# - Remaining characters can be [a-z, 0-9, _]
CHARSET_FIRST = 'abcdefghijklmnopqrstuvwxyz'  # First char: letters only
CHARSET_REST = 'abcdefghijklmnopqrstuvwxyz_0123456789'  # Rest: letters, digits, underscore
CHARSET = CHARSET_REST  # Legacy compatibility
CHARSET_EXTENDED = CHARSET + '.-/'

# Roman numerals for pattern generation (from WwiseNameCracker)
ROMAN_NUMERALS = ['i', 'ii', 'iii', 'iv', 'v', 'vi', 'vii', 'viii', 'ix', 'x',
                  'xi', 'xii', 'xiii', 'xiv', 'xv', 'xvi', 'xvii', 'xviii', 'xix', 'xx']

# Common Wwise naming patterns from wwiser-utils
WWISE_PREFIXES = [
    'play_', 'stop_', 'pause_', 'resume_', 'set_',
    'music_', 'sfx_', 'vo_', 'amb_', 'ui_',
    'hero_', 'npc_', 'weapon_', 'vehicle_', 'creature_',
]

WWISE_SUFFIXES = [
    '_lp', '_loop', '_01', '_02', '_03', '_04', '_05', '_06', '_07', '_08', '_09', '_10',
    '_start', '_stop', '_end', '_hit', '_miss', '_death', '_spawn', '_idle',
    '_a', '_b', '_c', '_d', '_attack', '_swing', '_block', '_parry',
    '_footstep', '_run', '_walk', '_jump', '_land', '_fall',
    # Roman numerals (from WwiseNameCracker)
    '_i', '_ii', '_iii', '_iv', '_v', '_vi', '_vii', '_viii', '_ix', '_x',
    # Extended suffixes
    '_pain', '_grunt', '_vocal', '_voice', '_vo', '_sfx', '_amb', '_mus',
    '_fire', '_impact', '_explode', '_charge', '_release', '_throw', '_catch',
    '_win', '_lose', '_good', '_evil', '_hero', '_enemy', '_ally', '_neutral',
    '_melee', '_ranged', '_magic', '_ability', '_special', '_ultimate',
    # Combat/Action suffixes
    '_taunt', '_cheer', '_roar', '_scream', '_yell', '_cry', '_grunt',
    '_slice', '_slash', '_stab', '_crush', '_smash', '_bash', '_pound',
    '_kill', '_die', '_hurt', '_heal', '_revive', '_resurrect',
    # Level/Location suffixes
    '_amb', '_atmo', '_win', '_lose', '_intro', '_outro', '_loop',
    # Hero ability suffixes
    '_activate', '_deactivate', '_cast', '_channel', '_combo',
    '_primary', '_secondary', '_alt', '_left', '_right',
    '_light', '_heavy', '_quick', '_slow', '_chain',
]

# LOTR Conquest specific terms - creatures, heroes, units, locations
LOTR_TERMS = [
    # Heroes - Good
    'aragorn', 'gandalf', 'legolas', 'gimli', 'frodo', 'sam', 'merry', 'pippin',
    'isildur', 'elendil', 'theoden', 'faramir', 'boromir', 'eowyn', 'haldir', 'elrond',
    'treebeard', 'galadriel', 'arwen',
    # Heroes - Evil
    'sauron', 'saruman', 'lurtz', 'gothmog', 'wormtongue', 'witchking', 'mouth',
    'nazgul', 'ringwraith', 'shagrat', 'gorbag',
    # Hero prefixed names (for ability patterns)
    'hero_aragorn', 'hero_legolas', 'hero_gimli', 'hero_gandalf', 'hero_frodo',
    'hero_sauron', 'hero_saruman', 'hero_lurtz', 'hero_gothmog', 'hero_witchking',
    'hero_elrond', 'hero_faramir', 'hero_theoden', 'hero_eowyn', 'hero_isildur',
    'hero_mouth', 'hero_wormtongue', 'hero_nazgul', 'hero_haldir', 'hero_balrog',
    # Aragorn specific
    'aragorn_sword', 'aragorn_kick', 'aragorn_oathbreaker', 'aragorn_ranger',
    # Legolas specific
    'legolas_arrow', 'legolas_bow', 'legolas_knife', 'legolas_dual',
    # Gimli specific
    'gimli_axe', 'gimli_leap', 'gimli_throw', 'gimli_slam',
    # Gandalf specific
    'gandalf_staff', 'gandalf_lightning', 'gandalf_shield', 'gandalf_istari',
    # Sauron specific
    'sauron_mace', 'sauron_ring', 'sauron_dominate', 'sauron_terror',
    # Saruman specific
    'saruman_staff', 'saruman_voice', 'saruman_blast', 'saruman_fireball',
    # Witch King specific
    'witchking_sword', 'witchking_scream', 'witchking_morgul', 'witchking_fellbeast',
    # Lurtz specific
    'lurtz_bow', 'lurtz_crossbow', 'lurtz_shield', 'lurtz_throw',
    # Gothmog specific
    'gothmog_mace', 'gothmog_sword', 'gothmog_hammer', 'gothmog_charge',
    # Mouth specific
    'mouth_sword', 'mouth_mace', 'mouth_diplomacy', 'mouth_taunt',
    # Faramir specific
    'faramir_bow', 'faramir_sword', 'faramir_ranger', 'faramir_stealth',
    # Theoden specific
    'theoden_sword', 'theoden_charge', 'theoden_rally', 'theoden_horse',
    # Eowyn specific
    'eowyn_sword', 'eowyn_shield', 'eowyn_charge', 'eowyn_shieldmaiden',
    # Isildur specific
    'isildur_sword', 'isildur_narsil', 'isildur_ring', 'isildur_charge',
    # Frodo specific
    'frodo_sting', 'frodo_ring', 'frodo_cloak', 'frodo_invisible',
    # Wormtongue specific
    'wormtongue_dagger', 'wormtongue_poison', 'wormtongue_backstab',
    # Hero action variations
    'hero_attack', 'hero_ability', 'hero_special', 'hero_ultimate',
    'hero_death', 'hero_spawn', 'hero_block', 'hero_dodge', 'hero_charge',
    # Specific ability names from game
    # Aragorn: Oathbreaker Army, Blade Storm, Army of the Dead
    'oathbreaker', 'bladestorm', 'blade_storm', 'army_dead', 'army_of_the_dead',
    'aragorn_oathbreaker', 'aragorn_blade', 'aragorn_army',
    'aragorn_attack', 'aragorn_swing', 'aragorn_combo', 'aragorn_kick',
    'aragorn_special', 'aragorn_ability', 'aragorn_death', 'aragorn_spawn',
    # Gandalf: Wizard Blast, Lightning Sword, Istari Light
    'wizard_blast', 'lightning', 'istari', 'istari_light', 'you_shall_not_pass',
    'gandalf_blast', 'gandalf_light', 'gandalf_lightning', 'gandalf_shield',
    # Gimli: Leap Attack, Axe Throw, Berserker
    'leap_attack', 'axe_throw', 'berserker', 'gimli_leap', 'gimli_throw', 'gimli_berserker',
    # Legolas: Arrow Storm, Knife Throw, Triple Shot
    'arrow_storm', 'knife_throw', 'triple_shot', 'legolas_storm', 'legolas_knife', 'legolas_triple',
    # Sauron: Dominate, Ring Power, Eye of Sauron
    'dominate', 'ring_power', 'eye_sauron', 'soul_collect', 'heart_horror', 'mordor_pound',
    'sauron_dominate', 'sauron_eye', 'sauron_soul', 'sauron_heart', 'sauron_pound',
    # Saruman: Voice Command, Fireball, Staff Slam
    'voice_command', 'fireball', 'staff_slam', 'saruman_voice', 'saruman_fireball', 'saruman_slam',
    # Witch King: Morgul Blade, Fear, Fell Beast
    'morgul_blade', 'fear', 'fell_beast', 'witchking_blade', 'witchking_fear', 'witchking_beast',
    # More hero action verbs
    'cast', 'summon', 'call', 'invoke', 'activate', 'trigger', 'execute', 'perform',
    'strike', 'slash', 'thrust', 'parry', 'counter', 'dodge', 'roll', 'evade',
    # Gandalf specific extended
    'gandalf_you_shall', 'gandalf_flame', 'gandalf_white', 'gandalf_grey',
    'gandalf_cast', 'gandalf_summon', 'gandalf_magic', 'gandalf_wizard',
    # Legolas specific extended
    'legolas_shot', 'legolas_aim', 'legolas_fire', 'legolas_draw', 'legolas_nock',
    'legolas_rapid', 'legolas_precision', 'legolas_volley',
    # Sauron specific extended
    'sauron_crush', 'sauron_slam', 'sauron_ground', 'sauron_wave', 'sauron_aura',
    'sauron_power', 'sauron_dark', 'sauron_evil', 'sauron_corruption',
    # Play/stop hero patterns
    'play_aragorn', 'stop_aragorn', 'play_gandalf', 'stop_gandalf',
    'play_legolas', 'stop_legolas', 'play_gimli', 'stop_gimli',
    'play_sauron', 'stop_sauron', 'play_saruman', 'stop_saruman',
    # Set hero patterns
    'set_aragorn', 'set_gandalf', 'set_legolas', 'set_gimli', 'set_sauron',
    'set_hero_aragorn', 'set_hero_gandalf', 'set_hero_legolas',
    # Voice line patterns (VO_)
    'vo_cks', 'vo_shire', 'vo_moria', 'vo_helms', 'vo_pelennor', 'vo_osg',
    'vo_training', 'vo_rivendell', 'vo_morgul', 'vo_blackgates', 'vo_weathertop',
    'vo_isengard', 'vo_mountdoom', 'vo_intro', 'vo_outro', 'vo_victory', 'vo_defeat',
    # Chatter patterns
    'chatter_aragorn', 'chatter_gandalf', 'chatter_legolas', 'chatter_gimli',
    'chatter_frodo', 'chatter_sauron', 'chatter_saruman', 'chatter_witchking',
    'chatter_orc', 'chatter_uruk', 'chatter_gondor', 'chatter_rohan', 'chatter_elf',
    # Combat event prefixes
    'cbt', 'cmbt', 'combat', 'atk', 'attack', 'def', 'defend',
    'cbt_aragorn', 'cbt_gandalf', 'cbt_legolas', 'cbt_gimli', 'cbt_sauron',
    'combat_aragorn', 'combat_gandalf', 'combat_legolas', 'combat_gimli',
    # Hero abbreviations
    'gnd', 'ara', 'leg', 'gim', 'frd', 'sau', 'sar', 'wk', 'isl',
    'gndlf', 'arag', 'legls', 'saur', 'srumn',
    # Alternate hero names
    'strider', 'mithrandir', 'grey', 'white', 'dwarf', 'hobbit', 'ringbearer',
    'darklord', 'lordofrings', 'nazgul_lord', 'ringwraith',
    # Simple numbered patterns
    'hero_01', 'hero_02', 'hero_03', 'hero_04', 'hero_05',
    'ability_01', 'ability_02', 'ability_03', 'ability_04',
    'combat_01', 'combat_02', 'combat_03', 'combat_04',
    # More level patterns
    'pelennor_intro', 'pelennor_outro', 'play_pelennor_good', 'play_pelennor_evil',
    'stop_pelennor_good', 'stop_pelennor_evil', 'pelennor_music',
    'weathertop_intro', 'weathertop_outro', 'play_weathertop', 'stop_weathertop',
    'isengard_intro', 'isengard_outro', 'play_isengard_good', 'play_isengard_evil',
    'stop_isengard_good', 'stop_isengard_evil',
    'mount_doom_intro', 'mount_doom_outro', 'mountdoom_intro', 'mountdoom_outro',
    'minas_tirith_intro', 'minas_intro', 'tirith_intro', 'minas_outro',
    # Siege weapons
    'trebuchet_fire', 'trebuchet_impact', 'trebuchet_load', 'trebuchet_release',
    'siege_tower', 'battering_ram', 'ram_impact', 'ladder_place',
    # Environment
    'fire_loop', 'fire_start', 'fire_end', 'explosion', 'explosion_large',
    'rock_impact', 'rock_fall', 'collapse', 'crumble', 'debris',
    # More music
    'play_battle', 'stop_battle', 'play_victory', 'stop_victory',
    'play_defeat', 'stop_defeat', 'play_tension', 'stop_tension',
    # More siege/combat effects
    'battering_ram', 'battering_ram_hit', 'battering_ram_swing', 'battering_ram_load',
    'siege_tower_move', 'siege_ladder', 'ladder_break', 'wall_breach',
    'gate_destroy', 'gate_open', 'gate_close', 'gate_impact',
    'arrow_hit', 'arrow_fly', 'arrow_land', 'arrow_wall', 'arrow_shield',
    'crossbow_fire', 'crossbow_load', 'bow_draw', 'bow_release',
    # Soldier/unit sounds
    'soldier_death', 'soldier_attack', 'soldier_hit', 'soldier_block',
    'uruk_death', 'uruk_attack', 'uruk_hit', 'uruk_taunt',
    'gondor_death', 'gondor_attack', 'gondor_charge', 'gondor_rally',
    'rohan_death', 'rohan_attack', 'rohan_charge', 'rohan_horn',
    'elf_death', 'elf_attack', 'elf_arrow', 'elf_bow',
    # Mount sounds
    'horse_gallop', 'horse_run', 'horse_walk', 'horse_stop', 'horse_neigh',
    'warg_mount', 'warg_dismount', 'warg_run', 'warg_bite',
    # UI/System sounds
    'menu_select', 'menu_back', 'menu_confirm', 'menu_cancel',
    'ui_click', 'ui_hover', 'ui_select', 'ui_back', 'ui_confirm',
    'spawn_in', 'spawn_out', 'respawn', 'death_cam',
    # More UI patterns (sfx_ui_ prefix found)
    'sfx_ui_click', 'sfx_ui_select', 'sfx_ui_back', 'sfx_ui_confirm',
    'sfx_ui_hover', 'sfx_ui_cancel', 'sfx_ui_start', 'sfx_ui_end',
    'sfx_ui_open', 'sfx_ui_close', 'sfx_ui_scroll', 'sfx_ui_tab',
    # More siege patterns (siege_tower_move_ prefix found)
    'siege_tower_move', 'siege_tower_hit', 'siege_tower_destroy',
    'siege_tower_deploy', 'siege_tower_burn', 'siege_tower_collapse',
    'battering_ram_hit', 'battering_ram_swing', 'battering_ram_impact',
    'battering_ram_destroy', 'battering_ram_move', 'battering_ram_stop',
    # Ladder patterns
    'siege_ladder_place', 'siege_ladder_climb', 'siege_ladder_fall',
    'ladder_place', 'ladder_climb', 'ladder_fall', 'ladder_break',
    # Gate patterns
    'gate_hit', 'gate_break', 'gate_destroy', 'gate_open', 'gate_close',
    'gate_impact', 'gate_creak', 'gate_slam',
    # More _lp (loop) patterns discovered
    'siege_tower_move_lp', 'catapult_lp', 'ballista_lp', 'fire_lp', 'wind_lp',
    'water_lp', 'ambient_lp', 'battle_lp', 'crowd_lp', 'army_lp',
    # More _start/_stop patterns
    'catapult_start', 'catapult_stop', 'ballista_start', 'ballista_stop',
    'fire_start', 'fire_stop', 'battle_start', 'battle_stop',
    # Trebuchet patterns
    'trebuchet_load', 'trebuchet_release', 'trebuchet_impact', 'trebuchet_lp',
    'trebuchet_start', 'trebuchet_stop', 'trebuchet_swing',
    # Catapult patterns
    'catapult_load', 'catapult_release', 'catapult_impact', 'catapult_swing',
    # Ballista patterns
    'ballista_load', 'ballista_fire', 'ballista_impact', 'ballista_swing',
    # Cloak/stealth patterns (cloak found)
    'cloak_start', 'cloak_end', 'cloak_loop', 'cloak_lp', 'play_cloak',
    'stealth', 'stealth_start', 'stealth_end', 'stealth_loop',
    'invisible', 'invisible_start', 'invisible_end',
    # Weapon tag patterns (weapon_tag_vocal found)
    'weapon_tag', 'weapon_tag_hit', 'weapon_tag_swing', 'weapon_tag_impact',
    'weapon_swing', 'weapon_hit', 'weapon_impact', 'weapon_draw', 'weapon_sheath',
    # More vocal patterns
    'death_vocal', 'spawn_vocal', 'taunt_vocal', 'charge_vocal', 'pain_vocal',
    'grunt_vocal', 'effort_vocal', 'yell_vocal', 'scream_vocal',
    # Class-specific patterns
    'warrior_attack', 'warrior_death', 'warrior_hit', 'warrior_spawn',
    'archer_attack', 'archer_death', 'archer_hit', 'archer_spawn',
    'mage_attack', 'mage_death', 'mage_hit', 'mage_spawn', 'mage_cast',
    'scout_attack', 'scout_death', 'scout_hit', 'scout_spawn',
    # Hero ability names from game (exact names)
    'heart_of_horror', 'mordor_pound', 'soul_collector',  # Sauron
    'wizard_blast', 'lightning_sword', 'istari_light',  # Gandalf
    'army_of_dead', 'oathbreaker', 'ranger_strike',  # Aragorn
    'rain_of_arrows', 'arrow_storm', 'elven_cloak',  # Legolas
    'berserker_rage', 'axe_throw', 'dwarf_toss',  # Gimli
    'ring_bearer', 'sting', 'mithril_coat',  # Frodo
    'wormtongue_poison', 'grima_dagger',  # Wormtongue
    'mouth_of_sauron', 'dark_speech',  # Mouth of Sauron
    'witch_king_scream', 'morgul_blade', 'fell_beast',  # Witch King
    'lurtz_bow', 'uruk_strength', 'berserker',  # Lurtz
    'saruman_blast', 'orthanc_fire', 'white_hand',  # Saruman
    'elrond_heal', 'vilya', 'rivendell_charge',  # Elrond
    'isildur_blade', 'narsil', 'bane_of_sauron',  # Isildur
    # Short ability prefixes
    'ab_', 'abl_', 'ability_', 'skill_', 'power_', 'special_',
    'ab_aragorn', 'ab_gandalf', 'ab_legolas', 'ab_gimli', 'ab_sauron',
    'abl_aragorn', 'abl_gandalf', 'abl_legolas', 'abl_gimli', 'abl_sauron',
    # More hero patterns with numbered suffixes
    'gandalf_01', 'gandalf_02', 'gandalf_03', 'gandalf_04', 'gandalf_05',
    'legolas_01', 'legolas_02', 'legolas_03', 'legolas_04', 'legolas_05',
    'aragorn_01', 'aragorn_02', 'aragorn_03', 'aragorn_04', 'aragorn_05',
    'gimli_01', 'gimli_02', 'gimli_03', 'gimli_04', 'gimli_05',
    'frodo_01', 'frodo_02', 'frodo_03', 'frodo_04', 'frodo_05',
    'elrond_01', 'elrond_02', 'elrond_03', 'elrond_04', 'elrond_05',
    'isildur_01', 'isildur_02', 'isildur_03', 'isildur_04', 'isildur_05',
    'witchking_01', 'witchking_02', 'witchking_03', 'witchking_04',
    'wormtongue_01', 'wormtongue_02', 'wormtongue_03', 'wormtongue_04',
    'mouth_01', 'mouth_02', 'mouth_03', 'mouth_04',
    'nazgul_01', 'nazgul_02', 'nazgul_03', 'nazgul_04',
    # Hero grunt/attack patterns
    'gandalf_grunt', 'gandalf_attack', 'gandalf_ability', 'gandalf_swing',
    'legolas_grunt', 'legolas_attack', 'legolas_ability', 'legolas_arrow',
    'aragorn_grunt', 'aragorn_attack', 'aragorn_ability', 'aragorn_swing',
    'gimli_grunt', 'gimli_attack', 'gimli_ability', 'gimli_axe',
    'frodo_grunt', 'frodo_attack', 'frodo_ability', 'frodo_ring',
    'elrond_grunt', 'elrond_attack', 'elrond_ability', 'elrond_heal',
    'isildur_grunt', 'isildur_attack', 'isildur_ability', 'isildur_sword',
    'witchking_grunt', 'witchking_attack', 'witchking_ability',
    'wormtongue_grunt', 'wormtongue_attack', 'wormtongue_ability',
    'mouth_grunt', 'mouth_attack', 'mouth_ability',
    'nazgul_grunt', 'nazgul_attack', 'nazgul_ability',
    # Creatures/units
    'balrog', 'troll', 'oliphaunt', 'mumakil', 'warg', 'fellbeast', 'shelob', 'spider',
    'orc', 'uruk', 'goblin', 'ent', 'elf', 'dwarf', 'hobbit', 'horse', 'eagle', 'cave',
    'soldier', 'archer', 'warrior', 'mage', 'scout', 'captain', 'grunt', 'minion',
    'human', 'undead', 'ghost', 'wraith', 'creature', 'beast', 'monster',
    # Locations
    'mordor', 'gondor', 'rohan', 'shire', 'moria', 'minas', 'tirith', 'helms', 'deep',
    'isengard', 'barad', 'dur', 'osgiliath', 'rivendell', 'lorien', 'pelennor',
    'weathertop', 'blackgate', 'mount', 'doom', 'morgul', 'orthanc', 'tower',
    'training', 'trng', 'level',
    # Level abbreviations (as found in game files)
    'osg', 'pel', 'md', 'wt', 'riv', 'iso', 'bg', 'mt', 'mm',
    'hdp', 'bgates', 'trn', 'lvl', 'map', 'blackgates', 'mountdoom',
    # Full level prefixes for music patterns
    'shire', 'moria', 'helms', 'rivendell', 'morgul', 'osgiliath',
    'pelennor', 'weathertop', 'isengard', 'training',
    # Level music play/stop patterns
    'play_shire', 'stop_shire', 'play_helms', 'stop_helms',
    'play_rivendell', 'stop_rivendell', 'play_pel', 'stop_pel',
    'play_bgates', 'stop_bgates', 'play_wt', 'stop_wt',
    'play_mm', 'stop_mm', 'play_md', 'stop_md', 'play_iso', 'stop_iso',
    # Weapons/items
    'sword', 'bow', 'axe', 'mace', 'staff', 'shield', 'ring', 'bomb', 'arrow', 'spear',
    'catapult', 'ballista', 'siege', 'ram', 'battering', 'projectile', 'missile',
    # Actions/sounds
    'combat', 'attack', 'defend', 'special', 'ability', 'ultimate', 'swing', 'block',
    'victory', 'defeat', 'spawn', 'death', 'respawn', 'capture', 'grab', 'throw',
    'taunt', 'cheer', 'roar', 'scream', 'growl', 'bite', 'stomp', 'charge',
    'pain', 'grunt', 'yell', 'cry', 'laugh', 'cough', 'breath', 'gasp',
    'footstep', 'impact', 'hit', 'slash', 'stab', 'crush', 'smash', 'kill',
    # Audio prefixes
    'play', 'stop', 'set', 'amb', 'sfx', 'mus', 'vo', 'ui', 'fx', 'hero',
    # Foley/sound design
    'foley', 'cloth', 'metal', 'leather', 'armor', 'water', 'splash', 'fire',
    'wind', 'rain', 'thunder', 'ambient', 'atmo', 'room', 'cave',
    # Class types
    'warrior', 'archer', 'mage', 'scout', 'tank', 'healer', 'support',
    # Factions
    'good', 'evil', 'neutral', 'enemy', 'ally', 'friend', 'foe',
    # Level states
    'intro', 'outro', 'loop', 'win', 'lose', 'start', 'end', 'ambient',
    # Game modes
    'ctf', 'tdm', 'conquest', 'dm', 'coop', 'campaign', 'versus',
]

# ============================================================================
# TARGET & DICTIONARY LOADERS
# ============================================================================

def load_wwise_id_table(json_path: Path) -> Tuple[Set[int], Dict[int, int], Set[str]]:
    """
    Parse WWiseIDTable.audio.json to extract target hashes.

    JSON structure:
    - obj1s: [{key, val}, ...] - direct entries
    - obj2s, obj3s: [[header, [{key, val}, ...]], ...] - nested entries
    - obj5s, obj6s, obj7s: [{key, val}, ...] - direct entries
    - extra: ["0x...", ...] - hex strings only

    Returns:
        - target_hashes: Set of hex keys (as integers) that are uncracked FNV hashes
        - hash_to_val: Dict mapping hash -> internal val
        - already_named: Set of keys that already have string names
    """
    target_hashes = set()
    hash_to_val = {}
    already_named = set()

    if not json_path.exists():
        print(f"[-] WWiseIDTable not found: {json_path}")
        return target_hashes, hash_to_val, already_named

    with open(json_path, 'r') as f:
        data = json.load(f)

    def process_entry(entry):
        """Process a single {key, val} entry."""
        if not isinstance(entry, dict):
            return
        key = entry.get('key', '')
        val = entry.get('val', 0)

        if key.startswith('0x') or key.startswith('0X'):
            # Hex hash - uncracked target
            try:
                h = int(key, 16)
                target_hashes.add(h)
                hash_to_val[h] = val
            except ValueError:
                pass
        elif key and key not in ('NONE',):
            # Already has string name
            already_named.add(key.lower())

    for list_name, obj_list in data.items():
        if not isinstance(obj_list, list):
            continue

        if list_name == 'extra':
            # extra is just a list of hex strings
            for hex_str in obj_list:
                if isinstance(hex_str, str) and hex_str.startswith('0x'):
                    try:
                        h = int(hex_str, 16)
                        target_hashes.add(h)
                    except ValueError:
                        pass
        elif list_name in ('obj1s', 'obj5s', 'obj6s', 'obj7s'):
            # Direct list of {key, val} entries
            for entry in obj_list:
                process_entry(entry)
        elif list_name in ('obj2s', 'obj3s'):
            # Nested: [[header, [{key, val}, ...]], ...]
            for nested in obj_list:
                if isinstance(nested, list) and len(nested) >= 2:
                    entries = nested[1]
                    if isinstance(entries, list):
                        for entry in entries:
                            process_entry(entry)

    return target_hashes, hash_to_val, already_named


def load_lotr_dictionary() -> Set[str]:
    """
    Load pre-extracted dictionary from lotr_dictionary.txt.
    Run extract_dictionary.py first to generate the file.
    """
    script_dir = Path(__file__).parent
    dict_file = script_dir / 'lotr_dictionary.txt'

    if not dict_file.exists():
        log(f"[-] Dictionary file not found: {dict_file}")
        log(f"    Run: python extract_dictionary.py")
        return set(LOTR_TERMS)

    terms = set()
    with open(dict_file, 'r', encoding='utf-8') as f:
        for line in f:
            term = line.strip()
            if term:
                terms.add(term)

    log(f"Loaded {len(terms):,} terms from {dict_file.name}")
    return terms


# ============================================================================
# FNV HASH IMPLEMENTATIONS
# ============================================================================

def fnv1_hash(s: str) -> int:
    """Standard FNV-1 hash (Wwise uses this with lowercase A-Z)."""
    return _fnv1_hash(s)

def fnv1_hash_continue(prev_hash: int, s: str) -> int:
    """Continue hash from existing state (for prefix caching)."""
    return _fnv1_continue(prev_hash, s)

def fnv1_inverse(target_hash: int, suffix: str) -> int:
    """
    Compute inverse FNV hash - undo hash operations from the end.
    Given target hash and suffix, returns what prefix hash should be.
    """
    h = target_hash
    for c in reversed(suffix):
        h = fnv1_inverse_step(h, ord(c.lower()))
    return h

def fnv1_fuzzy_mask(h: int) -> int:
    """Return upper 24 bits for fast early rejection."""
    return ((h * FNV_PRIME) & 0xFFFFFFFF) & 0xFFFFFF00

def fnv1_hash30(s: str) -> int:
    """
    Hash30 variant from Wwise SDK - XOR-folds 32-bit to 30-bit.
    Formula: (hash >> 30) ^ (hash & 0x3FFFFFFF)
    Some Wwise implementations use 30-bit IDs instead of 32-bit.
    """
    h32 = fnv1_hash(s)
    return (h32 >> 30) ^ (h32 & HASH30_MASK)

def fnv1_hash32_to_30(h32: int) -> int:
    """Convert an existing 32-bit hash to 30-bit XOR-folded variant."""
    return (h32 >> 30) ^ (h32 & HASH30_MASK)

# ============================================================================
# N-GRAM FILTERING (from wwiser-utils fnv.lst and fnv3.lst)
# Dramatically reduces search space by filtering impossible letter combinations
# ============================================================================

class NgramFilter:
    """
    Advanced 3-gram filter based on wwiser-utils fnv.lst/fnv3.lst format.

    Supports two modes:
    1. Banlist mode (fnv.lst): Start with all allowed, ban specific combos
    2. Oklist mode (fnv3.lst): Start with all banned, allow combos above threshold

    Format:
    - ^abc: ban/allow at word START
    - abc: ban/allow in MIDDLE of word
    - a[bcd]: shorthand for ab, ac, ad
    - abc: 123 (fnv3.lst format with count)
    """

    # Position constants (like fnv.c)
    POS_START = 0
    POS_MIDDLE = 1

    def __init__(self, banlist_file: Optional[Path] = None,
                 oklist_file: Optional[Path] = None,
                 threshold: int = 0):
        """
        Initialize filter.

        Args:
            banlist_file: Path to fnv.lst format (ban specific combos)
            oklist_file: Path to fnv3.lst format (allow combos above threshold)
            threshold: Minimum frequency count for oklist mode
        """
        # 3D lookup tables: [position][char1][char2][char3] -> allowed (True/False)
        # Using dict for sparse storage instead of full 37^3 array
        self.trigram_table = [{}, {}]  # [start_pos, middle_pos]
        self.threshold = threshold
        self.use_oklist = False

        if oklist_file and oklist_file.exists():
            self._load_oklist(oklist_file)
            self.use_oklist = True
        elif banlist_file and banlist_file.exists():
            self._load_banlist(banlist_file)
        else:
            # Default: minimal banlist
            self._init_default_banlist()

    def _init_default_banlist(self):
        """Initialize with minimal default banned trigrams."""
        banned = {
            'qxz', 'qzx', 'xqz', 'xzq', 'zqx', 'zxq',
            'jjj', 'kkk', 'qqq', 'vvv', 'www', 'xxx', 'zzz',
        }
        for trigram in banned:
            self.trigram_table[self.POS_MIDDLE][trigram] = False

    def _load_banlist(self, path: Path):
        """
        Load fnv.lst format banlist.
        Format: ^ab (start), ab (middle), a[bcd] (expansion)
        """
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue

                pos = self.POS_MIDDLE
                if line.startswith('^'):
                    pos = self.POS_START
                    line = line[1:]

                # Handle bracket expansion: a[bcd] -> ab, ac, ad
                if '[' in line and ']' in line:
                    base = line[0]
                    bracket_start = line.index('[')
                    bracket_end = line.index(']')
                    chars = line[bracket_start+1:bracket_end]
                    for c in chars:
                        # This bans all trigrams starting with base+c
                        bigram = base + c
                        for third in CHARSET_REST:
                            trigram = bigram + third
                            self.trigram_table[pos][trigram.lower()] = False
                else:
                    # Direct bigram ban - bans all trigrams starting with it
                    bigram = line[:2].lower()
                    for third in CHARSET_REST:
                        trigram = bigram + third
                        self.trigram_table[pos][trigram] = False

    def _load_oklist(self, path: Path):
        """
        Load fnv3.lst format oklist (allow list with frequency counts).
        Format: ^abc: 123 (start position), abc: 123 (middle position)
        """
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue

                pos = self.POS_MIDDLE
                if line.startswith('^'):
                    pos = self.POS_START
                    line = line[1:]

                # Parse "abc: 123" or "abc:123"
                if ':' not in line:
                    continue

                parts = line.split(':')
                trigram = parts[0].strip().lower()
                try:
                    count = int(parts[1].strip())
                except (ValueError, IndexError):
                    continue

                if len(trigram) >= 3:
                    trigram = trigram[:3]
                    # Allow if count >= threshold
                    self.trigram_table[pos][trigram] = (count >= self.threshold)

    def is_valid(self, s: str) -> bool:
        """Check if string contains no banned trigrams."""
        s = s.lower()
        if len(s) < 3:
            return True

        # Check start trigram
        start_tri = s[:3]
        if start_tri in self.trigram_table[self.POS_START]:
            if not self.trigram_table[self.POS_START][start_tri]:
                return False
        elif self.use_oklist:
            # In oklist mode, unlisted = banned
            return False

        # Check middle trigrams
        for i in range(1, len(s) - 2):
            tri = s[i:i+3]
            if tri in self.trigram_table[self.POS_MIDDLE]:
                if not self.trigram_table[self.POS_MIDDLE][tri]:
                    return False
            elif self.use_oklist:
                return False

        return True

    def is_valid_extension(self, prefix: str, char: str) -> bool:
        """Quick check if adding char creates banned trigram."""
        if len(prefix) < 2:
            return True

        trigram = (prefix[-2:] + char).lower()
        pos = self.POS_START if len(prefix) == 2 else self.POS_MIDDLE

        if trigram in self.trigram_table[pos]:
            return self.trigram_table[pos][trigram]

        # In oklist mode, unlisted = banned
        if self.use_oklist:
            return False

        # In banlist mode, unlisted = allowed
        return True

    def get_stats(self) -> Dict[str, int]:
        """Return statistics about loaded filters."""
        start_count = len(self.trigram_table[self.POS_START])
        middle_count = len(self.trigram_table[self.POS_MIDDLE])
        return {
            'start_trigrams': start_count,
            'middle_trigrams': middle_count,
            'mode': 'oklist' if self.use_oklist else 'banlist',
            'threshold': self.threshold,
        }

# ============================================================================
# PATTERN RECOGNITION (from WwiseNameCracker)
# ============================================================================

class NameType:
    """Name component types from WwiseNameCracker."""
    WORD = 'word'           # Alphabetic word: play, stop, hero
    NUMBER = 'number'       # Numeric: 01, 02, 123
    ROMAN = 'roman'         # Roman numeral: i, ii, iii, iv, v
    WORD_NUMBER = 'wordnum' # Word + number: attack1, hit02
    SEPARATOR = 'sep'       # Underscore separator

def to_roman(n: int) -> str:
    """Convert integer to lowercase Roman numeral (1-20)."""
    if n < 1 or n > 20:
        return str(n)
    numerals = ['', 'i', 'ii', 'iii', 'iv', 'v', 'vi', 'vii', 'viii', 'ix', 'x',
                'xi', 'xii', 'xiii', 'xiv', 'xv', 'xvi', 'xvii', 'xviii', 'xix', 'xx']
    return numerals[n]

def from_roman(s: str) -> int:
    """Convert lowercase Roman numeral to integer."""
    roman_map = {'i': 1, 'v': 5, 'x': 10}
    result = 0
    prev = 0
    for c in reversed(s.lower()):
        val = roman_map.get(c, 0)
        if val < prev:
            result -= val
        else:
            result += val
        prev = val
    return result

def is_roman(s: str) -> bool:
    """Check if string is a valid Roman numeral."""
    return s.lower() in ROMAN_NUMERALS

# ============================================================================
# PATTERN-BASED GENERATOR (from wwiser-utils words.py + WwiseNameCracker)
# ============================================================================

class PatternGenerator:
    """Generate candidate names using Wwise naming patterns."""

    def __init__(self, base_words: List[str]):
        self.base_words = [w.lower() for w in base_words]
        self.prefixes = WWISE_PREFIXES
        self.suffixes = WWISE_SUFFIXES

    def generate_with_prefix(self, word: str) -> List[str]:
        """Generate all prefix combinations: play_word, stop_word, etc."""
        return [f"{p}{word}" for p in self.prefixes]

    def generate_with_suffix(self, word: str) -> List[str]:
        """Generate all suffix combinations: word_01, word_loop, etc."""
        return [f"{word}{s}" for s in self.suffixes]

    def generate_with_numbers(self, word: str, max_n: int = 20) -> List[str]:
        """Generate numbered variants: word_01, word_02, ..., word_20."""
        results = []
        for i in range(max_n + 1):
            results.extend([
                f"{word}_{i}",
                f"{word}_{i:02d}",
                f"{word}{i}",
            ])
        return results

    def generate_with_roman(self, word: str, max_n: int = 10) -> List[str]:
        """Generate Roman numeral variants: word_i, word_ii, word_iii, etc."""
        results = []
        for i in range(1, max_n + 1):
            roman = to_roman(i)
            results.extend([
                f"{word}_{roman}",      # word_i
                f"{word}{roman}",       # wordi (less common)
            ])
        return results

    def generate_combinations(self, depth: int = 2, max_words: int = 500) -> List[str]:
        """Generate multi-word combinations: word1_word2, word1_word2_word3.

        Limited to first max_words to avoid memory explosion.
        With 500 words and depth=2: 500*499 = ~250k combinations (manageable)
        With 500 words and depth=3: would be ~20M (still too many, so cap at depth=2)
        """
        results = []
        # Only use most important words (shorter ones tend to be more meaningful)
        limited_words = sorted(self.base_words, key=len)[:max_words]
        # Cap depth at 2 to avoid explosion
        actual_depth = min(depth, 2)

        for r in range(2, actual_depth + 1):
            for combo in itertools.combinations(limited_words, r):
                # Join with underscore
                results.append('_'.join(combo))
                # Join reversed
                results.append('_'.join(reversed(combo)))
        return results

    def generate_all(self) -> List[str]:
        """Generate all pattern variations for all base words."""
        candidates = set()

        # Limit to shorter, more likely terms (38k -> ~2k most useful)
        priority_words = sorted([w for w in self.base_words if 3 <= len(w) <= 15], key=len)[:2000]
        print(f"    Processing {len(priority_words)} priority words (of {len(self.base_words)} total)...", flush=True)

        for i, word in enumerate(priority_words):
            candidates.add(word)
            candidates.update(self.generate_with_prefix(word))
            candidates.update(self.generate_with_suffix(word))
            candidates.update(self.generate_with_numbers(word, max_n=5))  # Reduced from 20
            candidates.update(self.generate_with_roman(word, max_n=5))  # Reduced from 10
            if (i + 1) % 500 == 0:
                print(f"      {i+1}/{len(priority_words)} words...", flush=True)

        # Multi-word combinations - use only core LOTR terms
        print(f"    Generating multi-word combinations...", flush=True)
        candidates.update(self.generate_combinations(depth=2, max_words=200))

        # Prefix + word + suffix combinations - LIMITED
        print(f"    Generating prefix+word+suffix...", flush=True)
        for word in priority_words[:200]:
            for prefix in self.prefixes:
                for suffix in self.suffixes:
                    candidates.add(f"{prefix}{word}{suffix}")

        print(f"    Total candidates: {len(candidates):,}", flush=True)
        return list(candidates)


# ============================================================================
# MEET-IN-THE-MIDDLE ATTACK
# Split search: compute prefix hashes forward, suffix hashes backward
# Find collision = found preimage
# ============================================================================

class MeetInTheMiddle:
    """
    Meet-in-the-middle attack for FNV hash.
    For a target hash T and string length L:
    1. Precompute all prefix hashes for strings of length L/2
    2. For each suffix of length L/2, compute inverse hash
    3. If inverse(T, suffix) matches a prefix hash, we found it!

    Time: O(charset^(L/2)) instead of O(charset^L)
    """

    def __init__(self, targets: Set[int], charset: str = CHARSET):
        self.targets = targets
        self.charset = charset
        self.prefix_table: Dict[int, str] = {}  # hash -> prefix string

    def build_prefix_table(self, prefix_len: int) -> int:
        """Build table of all prefix hashes up to prefix_len."""
        count = 0
        log(f"Building prefix hash table for lengths 1-{prefix_len}...")
        for length in range(1, prefix_len + 1):
            len_count = 0
            for combo in itertools.product(self.charset, repeat=length):
                s = ''.join(combo)
                h = fnv1_hash(s)
                self.prefix_table[h] = s
                count += 1
                len_count += 1
            log(f"  Length {length}: {len_count:,} prefixes (total: {count:,})")
        return count

    def search_with_suffix(self, suffix_len: int) -> List[Tuple[str, int]]:
        """
        For each possible suffix, compute what prefix hash is needed.
        If that hash exists in prefix_table, we found a match!
        """
        matches = []
        checked = 0

        log(f"Searching suffixes length 1-{suffix_len} against {len(self.targets):,} targets...")
        for length in range(1, suffix_len + 1):
            suffix_count = len(self.charset) ** length
            log(f"  Suffix length {length}: {suffix_count:,} suffixes to test...")

            for combo in itertools.product(self.charset, repeat=length):
                suffix = ''.join(combo)
                checked += 1

                for target in self.targets:
                    # What prefix hash would produce this target with this suffix?
                    needed_prefix_hash = fnv1_inverse(target, suffix)

                    if needed_prefix_hash in self.prefix_table:
                        prefix = self.prefix_table[needed_prefix_hash]
                        full_name = prefix + suffix
                        # Verify!
                        if fnv1_hash(full_name) == target:
                            matches.append((full_name, target))
                            log_match(full_name, target)

                if checked % 10000 == 0:
                    log(f"    Checked {checked:,} suffixes, matches={len(matches)}")

        log(f"  Search complete: {checked:,} suffixes checked, {len(matches)} matches")
        return matches

    def attack(self, total_length: int) -> List[Tuple[str, int]]:
        """Run MITM attack for strings of given total length."""
        prefix_len = total_length // 2
        suffix_len = total_length - prefix_len

        log(f"MITM Attack: target length={total_length}, prefix={prefix_len}, suffix={suffix_len}")
        table_size = self.build_prefix_table(prefix_len)
        log(f"Prefix table ready: {table_size:,} entries, {sys.getsizeof(self.prefix_table)/1024/1024:.1f} MB")

        matches = self.search_with_suffix(suffix_len)
        log(f"MITM complete: {len(matches)} matches found")

        return matches


# ============================================================================
# WWISE BRUTE FORCE (from FnvBrute charset rules)
# First char must be [a-z], rest can be [a-z0-9_]
# ============================================================================

class WwiseBruteForce:
    """
    Brute force with Wwise-specific charset rules.
    From FnvBrute/Audiokinetic SDK:
    - First character MUST be lowercase letter [a-z]
    - Remaining characters can be [a-z, 0-9, _]

    This reduces search space by ~30% compared to naive brute force.
    """

    def __init__(self, targets: Set[int]):
        self.targets = targets
        self.first_chars = CHARSET_FIRST  # a-z only
        self.rest_chars = CHARSET_REST    # a-z, 0-9, _
        self.found: List[Tuple[str, int]] = []

    def _generate_strings(self, length: int):
        """Generate all valid Wwise strings of given length."""
        if length < 1:
            return

        # First char must be letter
        for first in self.first_chars:
            if length == 1:
                yield first
            else:
                # Rest can be any valid char
                for rest in itertools.product(self.rest_chars, repeat=length - 1):
                    yield first + ''.join(rest)

    def brute_force(self, min_len: int, max_len: int,
                    progress_callback=None, use_fuzzy: bool = True) -> List[Tuple[str, int]]:
        """
        Brute force all strings from min_len to max_len.
        Uses Wwise charset rules for efficiency.

        Fuzzy optimization (from fnv.c):
        - Pre-compute target_fuzzy = target & 0xFFFFFF00 for all targets
        - At last character level, compute fuzzy_hash = (cur_hash * prime) & 0xFFFFFF00
        - If fuzzy_hash not in target_fuzzies, skip entire last-char loop
        - This prunes ~99.6% of final checks (only 1/256 fuzzy hashes match)
        """
        matches = []
        total_tested = 0

        # Pre-compute fuzzy target set for fast filtering
        if use_fuzzy:
            target_fuzzies = {(t & 0xFFFFFF00) for t in self.targets}
            print(f"[Brute] Fuzzy optimization: {len(self.targets)} targets -> {len(target_fuzzies)} fuzzy buckets")

        for length in range(min_len, max_len + 1):
            # Calculate expected count for this length
            if length == 1:
                expected = len(self.first_chars)
            else:
                expected = len(self.first_chars) * (len(self.rest_chars) ** (length - 1))

            print(f"[Brute] Testing length {length}: {expected:,} candidates")

            if length >= 2 and use_fuzzy:
                # Use fuzzy optimization for length >= 2
                matches.extend(self._brute_force_fuzzy(length, target_fuzzies, progress_callback))
            else:
                # Simple iteration for length 1
                for candidate in self._generate_strings(length):
                    h = fnv1_hash(candidate)
                    if h in self.targets:
                        matches.append((candidate, h))
                        print(f"  FOUND: {candidate} -> 0x{h:08X}")

                    total_tested += 1
                    if progress_callback and total_tested % 1000000 == 0:
                        progress_callback(total_tested, length)

        return matches

    def _brute_force_fuzzy(self, length: int, target_fuzzies: Set[int],
                           progress_callback=None) -> List[Tuple[str, int]]:
        """
        Brute force with fuzzy hash optimization (from fnv.c).

        Key insight: Different letters in charset only change the last byte of hash.
        So we can compute (prefix_hash * prime) & 0xFFFFFF00 once for prefix,
        then quickly check if ANY target could match before testing all last chars.
        """
        matches = []
        tested = 0
        skipped = 0
        prefix_count = 0
        last_report = time.time()

        # For length N, iterate over first N-1 chars as prefix
        prefix_len = length - 1
        log(f"Fuzzy brute force: length={length}, prefix_len={prefix_len}")

        for first in self.first_chars:
            if prefix_len == 0:
                # Length 1 case - compute hash for single char
                prefix = first
                prefix_hash = fnv1_hash(first[0] if prefix else '')
            else:
                for rest in itertools.product(self.rest_chars, repeat=prefix_len - 1):
                    prefix = first + ''.join(rest)
                    prefix_hash = fnv1_hash(prefix)
                    prefix_count += 1

                    # Compute fuzzy hash for last character position
                    fuzzy_hash = (prefix_hash * FNV_PRIME) & 0xFFFFFF00

                    # Quick check: if no target matches this fuzzy, skip all last chars
                    if fuzzy_hash not in target_fuzzies:
                        skipped += len(self.rest_chars)
                        continue

                    # Fuzzy matches - test each last character
                    for last_char in self.rest_chars:
                        h = ((prefix_hash * FNV_PRIME) & 0xFFFFFFFF) ^ ord(last_char)
                        if h in self.targets:
                            full = prefix + last_char
                            matches.append((full, h))
                            log_match(full, h)
                        tested += 1

                    # Progress report every 2 seconds
                    now = time.time()
                    if now - last_report >= 2.0:
                        log(f"  prefix='{prefix}' tested={tested:,} skipped={skipped:,} matches={len(matches)}")
                        last_report = now

            if prefix_len == 0:
                # Handle length 1 case
                h = fnv1_hash(first)
                if h in self.targets:
                    matches.append((first, h))
                    log_match(first, h)

        prune_pct = 100*skipped/(skipped+tested) if (skipped+tested) > 0 else 0
        log(f"  Fuzzy done: prefixes={prefix_count:,} tested={tested:,} skipped={skipped:,} ({prune_pct:.1f}% pruned) matches={len(matches)}")

        return matches

    def brute_force_with_prefix_cache(self, prefix: str, suffix_len: int) -> List[Tuple[str, int]]:
        """
        Brute force with cached prefix hash.
        More efficient when testing many suffixes with same prefix.
        """
        matches = []
        prefix_hash = fnv1_hash(prefix)

        for combo in itertools.product(self.rest_chars, repeat=suffix_len):
            suffix = ''.join(combo)
            h = fnv1_hash_continue(prefix_hash, suffix)
            if h in self.targets:
                full_name = prefix + suffix
                matches.append((full_name, h))

        return matches


# ============================================================================
# BIDIRECTIONAL SEARCH (from wwiser-utils issue #7)
# O(37^n + 37^m) instead of O(37^(n+m))
# ============================================================================

class BidirectionalSearch:
    """
    Bidirectional search using FNV invertibility.

    Key insight from wwiser-utils issue #7:
    - FNV is invertible with constant 0x359c449b
    - ifnv(fnv("abcd"), "dcba") == fnv("")
    - Search complexity: O(37^n + 37^m) << O(37^(n+m))

    For a 12-char name split 6+6:
    - Naive: 37^12 = 6.6 * 10^18 (impossible)
    - Bidirectional: 37^6 + 37^6 = 5.1 * 10^9 (feasible)
    """

    def __init__(self, targets: Set[int]):
        self.targets = targets
        self.prefix_hashes: Dict[int, str] = {}  # hash -> prefix string

    def build_prefix_table(self, max_len: int, use_wwise_rules: bool = True):
        """Build table of all prefix hashes."""
        print(f"[BiDir] Building prefix table (len 1-{max_len})...")
        count = 0

        first_chars = CHARSET_FIRST if use_wwise_rules else CHARSET_REST
        rest_chars = CHARSET_REST

        for length in range(1, max_len + 1):
            if length == 1:
                for c in first_chars:
                    h = fnv1_hash(c)
                    self.prefix_hashes[h] = c
                    count += 1
            else:
                for first in first_chars:
                    for rest in itertools.product(rest_chars, repeat=length - 1):
                        s = first + ''.join(rest)
                        h = fnv1_hash(s)
                        self.prefix_hashes[h] = s
                        count += 1

        print(f"[BiDir] Prefix table: {count:,} entries")
        return count

    def search_suffixes(self, max_len: int) -> List[Tuple[str, int]]:
        """
        For each possible suffix, compute inverse hash and check prefix table.
        """
        matches = []

        for length in range(1, max_len + 1):
            print(f"[BiDir] Searching suffixes of length {length}...")

            for combo in itertools.product(CHARSET_REST, repeat=length):
                suffix = ''.join(combo)

                for target in self.targets:
                    # What prefix hash would produce this target with this suffix?
                    needed_hash = fnv1_inverse(target, suffix)

                    if needed_hash in self.prefix_hashes:
                        prefix = self.prefix_hashes[needed_hash]
                        full_name = prefix + suffix

                        # Verify (should always match, but safety check)
                        if fnv1_hash(full_name) == target:
                            matches.append((full_name, target))
                            print(f"  FOUND: {full_name} -> 0x{target:08X}")

        return matches

    def attack(self, total_length: int) -> List[Tuple[str, int]]:
        """Run bidirectional attack for strings of given total length."""
        prefix_len = total_length // 2
        suffix_len = total_length - prefix_len

        self.build_prefix_table(prefix_len)
        return self.search_suffixes(suffix_len)


# ============================================================================
# DICTIONARY ATTACK WITH PATTERNS
# ============================================================================

class DictionaryAttack:
    """Enhanced dictionary attack using patterns and word combinations."""

    def __init__(self, targets: Set[int], ngram_filter: Optional[NgramFilter] = None):
        self.targets = targets
        self.ngram_filter = ngram_filter
        self.matches: List[Tuple[str, int]] = []

    def load_wordlist(self, path: Path) -> List[str]:
        """Load words from file."""
        words = []
        if path.exists():
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                for line in f:
                    word = line.strip().lower()
                    if word and len(word) >= 2:
                        words.append(word)
        return words

    def test_candidates(self, candidates: List[str]) -> List[Tuple[str, int]]:
        """Test list of candidate strings against targets."""
        matches = []
        total = len(candidates)
        tested = 0
        skipped = 0
        log(f"Testing {total:,} candidates against {len(self.targets):,} targets...")

        for i, candidate in enumerate(candidates):
            if self.ngram_filter and not self.ngram_filter.is_valid(candidate):
                skipped += 1
                continue
            tested += 1
            h = fnv1_hash(candidate)
            if h in self.targets:
                matches.append((candidate, h))
                log_match(candidate, h)

            # Progress every 10k
            if (i + 1) % 10000 == 0:
                log(f"  Progress: {i+1:,}/{total:,} tested={tested:,} skipped={skipped:,} matches={len(matches)}")

        log(f"  Done: tested={tested:,} skipped={skipped:,} matches={len(matches)}")
        return matches

    def run_pattern_attack(self, base_words: List[str]) -> List[Tuple[str, int]]:
        """Run pattern-based dictionary attack."""
        log(f"Starting pattern attack with {len(base_words):,} base words...")
        generator = PatternGenerator(base_words)
        candidates = generator.generate_all()
        log(f"Generated {len(candidates):,} pattern candidates")
        return self.test_candidates(candidates)

    def run_combination_attack(self, words: List[str], depth: int = 3) -> List[Tuple[str, int]]:
        """Test multi-word combinations."""
        matches = []
        total = 0

        for r in range(1, depth + 1):
            for combo in itertools.permutations(words[:100], r):  # Limit to avoid explosion
                candidate = '_'.join(combo)
                total += 1
                if self.ngram_filter and not self.ngram_filter.is_valid(candidate):
                    continue
                h = fnv1_hash(candidate)
                if h in self.targets:
                    matches.append((candidate, h))

        print(f"[Dict] Tested {total:,} combinations")
        return matches


# ============================================================================
# SUFFIX OPTIMIZATION ATTACK
# Use inverse FNV to search by suffix instead of prefix
# ============================================================================

class SuffixOptimizedSearch:
    """
    Use inverse FNV to efficiently search for strings ending with known suffixes.
    Much faster than brute force when we know common endings.
    """

    def __init__(self, targets: Set[int]):
        self.targets = targets
        # Precompute inverse target hashes for common suffixes
        self.suffix_targets: Dict[str, Dict[int, int]] = {}  # suffix -> {needed_prefix_hash: original_target}

    def precompute_suffix_targets(self, suffixes: List[str]):
        """For each suffix, compute what prefix hash is needed for each target."""
        log(f"Precomputing inverse hashes for {len(suffixes)} suffixes x {len(self.targets):,} targets...")
        for i, suffix in enumerate(suffixes):
            self.suffix_targets[suffix] = {}
            for target in self.targets:
                needed = fnv1_inverse(target, suffix)
                self.suffix_targets[suffix][needed] = target
            if (i + 1) % 10 == 0:
                log(f"  Suffix {i+1}/{len(suffixes)}: '{suffix}' -> {len(self.suffix_targets[suffix]):,} lookup entries")
        log(f"  Precompute done: {len(suffixes)} suffixes ready")

    def search_prefixes(self, prefixes: List[str]) -> List[Tuple[str, int]]:
        """Given prefixes and precomputed suffix targets, find matches."""
        matches = []
        log(f"Searching {len(prefixes):,} prefixes against {len(self.suffix_targets)} suffix tables...")

        for i, prefix in enumerate(prefixes):
            prefix_hash = fnv1_hash(prefix)

            for suffix, targets_map in self.suffix_targets.items():
                if prefix_hash in targets_map:
                    full_name = prefix + suffix
                    target = targets_map[prefix_hash]
                    # Verify
                    if fnv1_hash(full_name) == target:
                        matches.append((full_name, target))
                        log_match(full_name, target)

            if (i + 1) % 5000 == 0:
                log(f"  Progress: {i+1:,}/{len(prefixes):,} prefixes, matches={len(matches)}")

        log(f"  Search done: {len(matches)} matches found")
        return matches


# ============================================================================
# NATIVE C LIBRARY INTEGRATION
# ============================================================================

class NativeHasher:
    """Wrapper for native C hash library."""

    def __init__(self, dll_path: Path = None):
        self.available = False
        self.lib = None

        # Try multiple paths
        if dll_path is None:
            script_dir = Path(__file__).parent
            candidates = [
                script_dir / 'fnv1_hash.dll',
                Path('fnv1_hash.dll'),
                Path('./fnv1_hash.dll'),
            ]
        else:
            candidates = [dll_path]

        for path in candidates:
            if path.exists():
                try:
                    # Use absolute path for Windows
                    self.lib = ctypes.CDLL(str(path.absolute()))
                    self._setup_functions()
                    self.available = True
                    print(f"[+] Native library loaded: {path.absolute()}")
                    break
                except Exception as e:
                    print(f"[-] Failed to load {path}: {e}")

    def _setup_functions(self):
        """Set up C function signatures."""
        # wwise_hash
        self.lib.wwise_hash.argtypes = [ctypes.c_char_p]
        self.lib.wwise_hash.restype = ctypes.c_uint32

        # wwise_hash_continue
        self.lib.wwise_hash_continue.argtypes = [ctypes.c_uint32, ctypes.c_char_p]
        self.lib.wwise_hash_continue.restype = ctypes.c_uint32

        # wwise_hash_inverse
        self.lib.wwise_hash_inverse.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_int]
        self.lib.wwise_hash_inverse.restype = ctypes.c_uint32

    def hash(self, s: str) -> int:
        if self.available:
            return self.lib.wwise_hash(s.encode('ascii'))
        return fnv1_hash(s)

    def hash_continue(self, prev_hash: int, s: str) -> int:
        if self.available:
            return self.lib.wwise_hash_continue(prev_hash, s.encode('ascii'))
        return fnv1_hash_continue(prev_hash, s)

    def hash_inverse(self, target: int, suffix: str) -> int:
        if self.available:
            return self.lib.wwise_hash_inverse(target, suffix.encode('ascii'), len(suffix))
        return fnv1_inverse(target, suffix)


# ============================================================================
# MAIN ORCHESTRATOR
# ============================================================================

def load_targets(events_file: Path) -> Dict[int, str]:
    """Load target event IDs from extracted_events.json."""
    if not events_file.exists():
        print(f"[-] File not found: {events_file}")
        return {}

    with open(events_file, 'r') as f:
        data = json.load(f)

    targets = {}
    for event_id, info in data.get('events', {}).items():
        targets[int(event_id)] = info.get('bank', 'unknown')

    return targets

def load_existing_matches(matches_file: Path) -> Set[str]:
    """Load already-found matches."""
    existing = set()
    if matches_file.exists():
        with open(matches_file, 'r') as f:
            for line in f:
                if line.startswith('0x'):
                    parts = line.strip().split(',')
                    if len(parts) >= 2:
                        existing.add(parts[1].lower())
    return existing


def run_advanced_attack(args):
    """Main attack orchestrator."""
    print("=" * 70)
    print("  ADVANCED WWISE BRUTE-FORCER v3.0")
    print("  LOTR Conquest Audio RE Project")
    print("=" * 70)

    # Define paths (relative to script location)
    script_dir = Path(__file__).parent
    wwise_id_table = script_dir / 'Dictionary canditates' / 'WWiseIDTable.audio.json'
    events_file = script_dir / 'extracted_events.json'
    fnv_lst = script_dir / 'Tools' / 'wwiser-utils-master' / 'fnv' / 'fnv.lst'
    fnv3_lst = script_dir / 'Tools' / 'wwiser-utils-master' / 'fnv' / 'fnv3.lst'

    # Load targets from multiple sources
    targets = {}
    already_cracked = set()

    # Source 1: extracted_events.json (legacy)
    legacy_targets = load_targets(events_file)
    if legacy_targets:
        for t, bank in legacy_targets.items():
            targets[t] = bank
        print(f"[+] Loaded {len(legacy_targets):,} targets from extracted_events.json")

    # Source 2: WWiseIDTable.audio.json
    if wwise_id_table.exists():
        wwise_hashes, hash_to_val, already_named = load_wwise_id_table(wwise_id_table)
        for h in wwise_hashes:
            if h not in targets:
                targets[h] = f"val:{hash_to_val.get(h, 0)}"
        already_cracked.update(already_named)
        print(f"[+] Loaded {len(wwise_hashes):,} uncracked hashes from WWiseIDTable")
        print(f"[+] Found {len(already_named):,} already-named entries")

    if not targets:
        print("[-] No targets loaded!")
        return

    target_set = set(targets.keys())
    print(f"[+] Total unique targets: {len(targets):,}")

    # Load existing matches
    existing = load_existing_matches(script_dir / 'dictionary_matches.txt')
    existing.update(already_cracked)
    print(f"[+] Already have {len(existing):,} known matches")

    # Load pre-extracted LOTR dictionary (instant load from file)
    lotr_dict = set(LOTR_TERMS)
    extracted_terms = load_lotr_dictionary()
    lotr_dict.update(extracted_terms)
    log(f"LOTR dictionary ready: {len(lotr_dict):,} terms")

    # Initialize N-gram filter with proper files
    if hasattr(args, 'ngram_threshold') and args.ngram_threshold > 0 and fnv3_lst.exists():
        ngram_filter = NgramFilter(oklist_file=fnv3_lst, threshold=args.ngram_threshold)
        print(f"[+] Loaded fnv3.lst oklist (threshold={args.ngram_threshold})")
    elif fnv_lst.exists():
        ngram_filter = NgramFilter(banlist_file=fnv_lst)
        print(f"[+] Loaded fnv.lst banlist")
    else:
        ngram_filter = NgramFilter()
        print(f"[+] Using default trigram filter")

    stats = ngram_filter.get_stats()
    print(f"    Mode: {stats['mode']}, Start: {stats['start_trigrams']}, Middle: {stats['middle_trigrams']}")

    all_matches = []

    # 1. Pattern-based attack with LOTR terms
    if args.patterns:
        print("\n[PHASE 1] Pattern-based attack...")
        dict_attack = DictionaryAttack(target_set, ngram_filter)
        matches = dict_attack.run_pattern_attack(list(lotr_dict))
        all_matches.extend(matches)
        print(f"  Found: {len(matches)} matches")

    # 2. Meet-in-the-middle for medium-length names
    if args.mitm:
        print(f"\n[PHASE 2] Meet-in-the-middle attack (length {args.mitm_length})...")
        mitm = MeetInTheMiddle(target_set)
        matches = mitm.attack(args.mitm_length)
        all_matches.extend(matches)
        print(f"  Found: {len(matches)} matches")

    # 3. Suffix optimization attack
    if args.suffix:
        print("\n[PHASE 3] Suffix optimization attack...")
        suffix_search = SuffixOptimizedSearch(target_set)
        suffix_search.precompute_suffix_targets(WWISE_SUFFIXES)

        # Generate prefix candidates
        prefixes = []
        for term in LOTR_TERMS:
            prefixes.append(term)
            for p in WWISE_PREFIXES:
                prefixes.append(f"{p}{term}")

        matches = suffix_search.search_prefixes(prefixes)
        all_matches.extend(matches)
        print(f"  Found: {len(matches)} matches")

    # 4. Custom wordlist
    if args.wordlist:
        print(f"\n[PHASE 4] Custom wordlist attack: {args.wordlist}")
        dict_attack = DictionaryAttack(target_set, ngram_filter)
        words = dict_attack.load_wordlist(Path(args.wordlist))
        print(f"  Loaded {len(words):,} words")
        matches = dict_attack.run_pattern_attack(words)
        all_matches.extend(matches)
        print(f"  Found: {len(matches)} matches")

    # 5. Bidirectional search (from wwiser-utils issue #7)
    if hasattr(args, 'bidir') and args.bidir:
        print(f"\n[PHASE 5] Bidirectional search (length {args.bidir_length})...")
        bidir = BidirectionalSearch(target_set)
        matches = bidir.attack(args.bidir_length)
        all_matches.extend(matches)
        print(f"  Found: {len(matches)} matches")

    # 6. Wwise brute force with proper charset rules (from FnvBrute)
    if hasattr(args, 'brute') and args.brute:
        print(f"\n[PHASE 6] Wwise brute force (len {args.min_len}-{args.max_len})...")
        print("  Using Wwise charset rules: first char [a-z], rest [a-z0-9_]")
        use_fuzzy = not getattr(args, 'no_fuzzy', False)
        if use_fuzzy:
            print("  Fuzzy hash optimization: ENABLED")
        wwise_brute = WwiseBruteForce(target_set)
        matches = wwise_brute.brute_force(args.min_len, args.max_len, use_fuzzy=use_fuzzy)
        all_matches.extend(matches)
        print(f"  Found: {len(matches)} matches")

    # Deduplicate and filter
    seen = set()
    new_matches = []
    for name, h in all_matches:
        if name.lower() not in existing and name.lower() not in seen:
            seen.add(name.lower())
            new_matches.append((name, h, targets.get(h, 'unknown')))

    # Results
    print("\n" + "=" * 70)
    print("RESULTS")
    print("=" * 70)
    print(f"Total matches:    {len(all_matches)}")
    print(f"NEW matches:      {len(new_matches)}")

    if new_matches:
        print("\nNew matches found:")
        for name, h, bank in sorted(new_matches, key=lambda x: x[0]):
            print(f"  0x{h:08X} -> {name:30} [{bank}]")

        # Save
        with open(script_dir / 'advanced_matches.txt', 'a') as f:
            f.write(f"\n# Advanced attack: {datetime.now().isoformat()}\n")
            for name, h, bank in new_matches:
                f.write(f"0x{h:08X},{name},{bank}\n")
        print(f"\n[+] Saved to advanced_matches.txt")


def main():
    parser = argparse.ArgumentParser(
        description='Advanced Wwise Event Name Brute-Forcer v3.0',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Integrated Techniques (from research):
  - Inverse FNV optimization (suffix searches) - wwiser-utils #7
  - N-gram filtering (skip impossible trigrams) - wwiser-utils
  - Meet-in-the-middle attack O(37^(n/2)) - wwiser-utils #7
  - Bidirectional search O(37^n + 37^m) - wwiser-utils #7
  - Pattern generation (play_%s, %s_loop) - wwiser-utils words.py
  - Roman numeral patterns (_i, _ii, _iii) - WwiseNameCracker
  - Wwise charset rules (first char [a-z]) - FnvBrute/Audiokinetic SDK
  - Dictionary combination attacks

Data Sources (auto-loaded):
  - WWiseIDTable.audio.json: Uncracked hex hashes from game
  - Dictionary canditates/*/English.json: LOTR terms for dictionary
  - fnv.lst/fnv3.lst: N-gram filters from wwiser-utils

Examples:
  python brute_force_advanced.py --patterns          # LOTR term patterns + Roman numerals
  python brute_force_advanced.py --mitm --mitm-length 10   # MITM for 10-char names
  python brute_force_advanced.py --bidir --bidir-length 12 # Bidirectional for 12-char
  python brute_force_advanced.py --brute --min-len 1 --max-len 6  # Wwise brute force
  python brute_force_advanced.py --suffix            # Suffix optimization
  python brute_force_advanced.py --wordlist words.txt      # Custom wordlist
  python brute_force_advanced.py --all               # Run all attacks
  python brute_force_advanced.py --benchmark         # Hash speed benchmark
  python brute_force_advanced.py --ngram-threshold 10      # Use fnv3.lst oklist mode
        """
    )

    parser.add_argument('--patterns', '-p', action='store_true',
                        help='Run pattern-based attack with LOTR terms + Roman numerals')
    parser.add_argument('--mitm', '-m', action='store_true',
                        help='Run meet-in-the-middle attack')
    parser.add_argument('--mitm-length', type=int, default=8,
                        help='Target string length for MITM (default: 8)')
    parser.add_argument('--bidir', action='store_true',
                        help='Run bidirectional search (O(37^n + 37^m))')
    parser.add_argument('--bidir-length', type=int, default=10,
                        help='Target string length for bidirectional (default: 10)')
    parser.add_argument('--brute', action='store_true',
                        help='Run Wwise brute force with proper charset rules')
    parser.add_argument('--min-len', type=int, default=1,
                        help='Minimum string length for brute force (default: 1)')
    parser.add_argument('--max-len', type=int, default=6,
                        help='Maximum string length for brute force (default: 6)')
    parser.add_argument('--suffix', '-s', action='store_true',
                        help='Run suffix optimization attack')
    parser.add_argument('--wordlist', '-w', type=str,
                        help='Path to custom wordlist file')
    parser.add_argument('--all', '-a', action='store_true',
                        help='Run all attack modes (patterns, mitm, suffix)')
    parser.add_argument('--benchmark', '-b', action='store_true',
                        help='Run hash benchmark')
    parser.add_argument('--ngram-threshold', type=int, default=0,
                        help='Use fnv3.lst oklist mode with this frequency threshold (0=use fnv.lst banlist)')
    parser.add_argument('--no-fuzzy', action='store_true',
                        help='Disable fuzzy hash optimization in brute force')

    args = parser.parse_args()

    # --all enables safe attacks (not brute force which can be slow)
    if args.all:
        args.patterns = True
        args.mitm = True
        args.suffix = True

    # Default to patterns if nothing specified
    if not any([args.patterns, args.mitm, args.bidir, args.brute,
                args.suffix, args.wordlist, args.benchmark]):
        args.patterns = True

    if args.benchmark:
        run_benchmark()
        return

    run_advanced_attack(args)


def run_benchmark():
    """Benchmark hash implementations."""
    print("\n[HASH BENCHMARK]")
    print("-" * 50)

    test_strings = ['test', 'hello_world', 'play_music_combat', 'a' * 20]
    iterations = 100000

    # Python
    start = time.time()
    for _ in range(iterations):
        for s in test_strings:
            fnv1_hash(s)
    py_time = time.time() - start
    py_rate = (iterations * len(test_strings)) / py_time
    print(f"Python FNV-1:      {py_rate/1e6:.2f} M/s")

    # Inverse
    start = time.time()
    for _ in range(iterations):
        for s in test_strings:
            fnv1_inverse(0x12345678, s)
    inv_time = time.time() - start
    inv_rate = (iterations * len(test_strings)) / inv_time
    print(f"Python Inverse:    {inv_rate/1e6:.2f} M/s")

    # Native
    native = NativeHasher()
    if native.available:
        start = time.time()
        for _ in range(iterations):
            for s in test_strings:
                native.hash(s)
        nat_time = time.time() - start
        nat_rate = (iterations * len(test_strings)) / nat_time
        print(f"Native C:          {nat_rate/1e6:.2f} M/s ({nat_rate/py_rate:.1f}x)")

    print("-" * 50)


if __name__ == '__main__':
    main()
