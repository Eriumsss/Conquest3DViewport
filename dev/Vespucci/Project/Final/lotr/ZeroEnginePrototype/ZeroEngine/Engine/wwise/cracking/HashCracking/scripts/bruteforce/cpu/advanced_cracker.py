#!/usr/bin/env python3
"""
Advanced Multi-Threaded Wwise Event Hash Cracker
Uses pattern analysis, precomputed hash tables, and parallel processing
"""

import csv
import json
import time
import psutil
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from collections import Counter, defaultdict
from itertools import product
import string
import sys
from pathlib import Path

# Shared Wwise hash implementation (matches SDK AkFNVHash)
SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

class SystemMonitor:
    """Monitor system health during intensive operations"""
    def __init__(self, max_cpu_temp=85, max_ram_percent=70):
        self.max_cpu_temp = max_cpu_temp
        self.max_ram_percent = max_ram_percent
        self.should_throttle = False
        self._stop = False
        
    def start(self):
        self._stop = False
        self.thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.thread.start()
        
    def stop(self):
        self._stop = True
        
    def _monitor_loop(self):
        while not self._stop:
            ram = psutil.virtual_memory().percent
            cpu = psutil.cpu_percent(interval=1)
            self.should_throttle = ram > self.max_ram_percent or cpu > 95
            if self.should_throttle:
                print(f"[MONITOR] Throttling: RAM={ram:.1f}%, CPU={cpu:.1f}%")
            time.sleep(2)

class HashCracker:
    def __init__(self):
        self.targets = {}
        self.found = {}
        self.patterns_tested = 0
        self.lock = threading.Lock()
        self.monitor = SystemMonitor()
        
    def load_targets(self, events_file, overrides_file):
        """Load uncracked event hashes"""
        with open(events_file, 'r') as f:
            data = json.load(f)
        events = data['events']
        
        cracked = set()
        with open(overrides_file, 'r') as f:
            reader = csv.reader(f)
            next(reader, None)
            for row in reader:
                if len(row) >= 2 and row[0]:
                    cracked.add(row[0].lower())
        
        for eid, info in events.items():
            eid_hex = f'0x{int(eid):08X}'.lower()
            if eid_hex not in cracked:
                bank = info.get('bank', 'Unknown')
                self.targets[int(eid)] = bank
                
        print(f"Loaded {len(self.targets)} uncracked targets")
        
    def test_pattern(self, pattern):
        """Test a single pattern against all targets"""
        h = fnv1_hash(pattern)
        if h in self.targets:
            with self.lock:
                if h not in self.found:
                    self.found[h] = pattern
                    print(f"[CRACK] 0x{h:08X} = {pattern} ({self.targets[h]})")
        with self.lock:
            self.patterns_tested += 1
            
    def test_batch(self, patterns):
        """Test a batch of patterns"""
        for p in patterns:
            if self.monitor.should_throttle:
                time.sleep(0.1)
            self.test_pattern(p)
            
    def parallel_attack(self, pattern_generator, num_workers=8, batch_size=10000):
        """Run parallel hash cracking"""
        self.monitor.start()
        start_time = time.time()
        
        try:
            with ThreadPoolExecutor(max_workers=num_workers) as executor:
                batch = []
                futures = []
                
                for pattern in pattern_generator:
                    batch.append(pattern)
                    if len(batch) >= batch_size:
                        futures.append(executor.submit(self.test_batch, batch.copy()))
                        batch = []
                        
                        # Process completed futures
                        done = [f for f in futures if f.done()]
                        for f in done:
                            f.result()
                            futures.remove(f)
                            
                if batch:
                    futures.append(executor.submit(self.test_batch, batch))
                    
                for f in as_completed(futures):
                    f.result()
                    
        finally:
            self.monitor.stop()
            
        elapsed = time.time() - start_time
        rate = self.patterns_tested / elapsed if elapsed > 0 else 0
        print(f"\nTested {self.patterns_tested:,} patterns in {elapsed:.1f}s ({rate:,.0f}/s)")
        print(f"Found {len(self.found)} matches")
        
    def save_results(self, overrides_file):
        """Append found results to overrides.csv"""
        if not self.found:
            return
        with open(overrides_file, 'a', newline='') as f:
            writer = csv.writer(f)
            for h, name in self.found.items():
                bank = self.targets.get(h, 'unknown')
                writer.writerow([f'0x{h:08X}', name, 'sfx', 'high', f'Cracked from {bank}'])
        print(f"Saved {len(self.found)} new events")

if __name__ == '__main__':
    cracker = HashCracker()
    cracker.load_targets('extracted_events.json', 'overrides.csv')
