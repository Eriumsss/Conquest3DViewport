#!/usr/bin/env python3
"""Fast 6-char brute force using optimized iteration"""
import time
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

CHARSET = 'abcdefghijklmnopqrstuvwxyz0123456789_'
CHARSET_SIZE = 37

TARGETS = {
    0xDD7978E6, 0xDCD9D5DD, 0xDF91450F, 0xD1E41CDA,
    0xA6D835D7, 0xFF74FDE5, 0xEF688F80, 0x94BDA720,
    0xE234322F, 0x783CDC38, 0xB53A0D23, 0xD6454E24,
    0x8DCE21D5, 0x79D92FB7, 0x0CCA70A9, 0x4C480561,
    0x84405926, 0x5BBF9654, 0x2EB326D8, 0xD9A5464C, 0x214CA366,
}

TARGET_BANKS = {
    0xDD7978E6: 'Creatures', 0xDCD9D5DD: 'SFXSiegeTower',
    0xDF91450F: 'SFXOliphant', 0xD1E41CDA: 'SFXBalrog',
    0xA6D835D7: 'HeroSaruman', 0xFF74FDE5: 'HeroGimli',
    0xEF688F80: 'HeroMouth', 0x94BDA720: 'Level_Isengard',
    0xE234322F: 'Ambience', 0x783CDC38: 'Ambience',
    0xB53A0D23: 'SFXBallista', 0xD6454E24: 'SFXBallista',
    0x8DCE21D5: 'SFXBatteringRam', 0x79D92FB7: 'SFXBatteringRam',
    0x0CCA70A9: 'SFXCatapult', 0x4C480561: 'SFXCatapult',
    0x84405926: 'HeroIsildur', 0x5BBF9654: 'HeroIsildur',
    0x2EB326D8: 'HeroIsildur', 0xD9A5464C: 'HeroLegolas',
    0x214CA366: 'HeroLegolas',
}

def main():
    print('Starting 6-char brute force...', flush=True)
    print(f'Total patterns: {CHARSET_SIZE**6:,}', flush=True)
    print(f'Targets: {len(TARGETS)}', flush=True)
    
    found = []
    start = time.time()
    count = 0
    
    # Direct iteration is faster than index conversion
    for c1 in CHARSET:
        for c2 in CHARSET:
            for c3 in CHARSET:
                for c4 in CHARSET:
                    for c5 in CHARSET:
                        for c6 in CHARSET:
                            pattern = c1 + c2 + c3 + c4 + c5 + c6
                            h = fnv1_hash(pattern)
                            if h in TARGETS:
                                found.append((h, pattern))
                                bank = TARGET_BANKS.get(h, 'Unknown')
                                print(f'CRACKED: 0x{h:08X} = {pattern} ({bank})', flush=True)
                            count += 1
        
        # Progress after each first char
        elapsed = time.time() - start
        rate = count / elapsed if elapsed > 0 else 0
        pct = 100 * count / (CHARSET_SIZE**6)
        print(f'Progress: {pct:.1f}% ({rate/1e6:.2f}M/s) tested={count:,}', flush=True)
    
    elapsed = time.time() - start
    print(f'\nCompleted {count:,} patterns in {elapsed:.1f}s', flush=True)
    print(f'Rate: {count/elapsed/1e6:.2f}M/s', flush=True)
    print(f'Found: {len(found)}/{len(TARGETS)}', flush=True)
    
    for h, p in found:
        print(f'  0x{h:08X} = {p}', flush=True)

if __name__ == '__main__':
    main()
