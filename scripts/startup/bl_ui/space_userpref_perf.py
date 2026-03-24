# SPDX-FileCopyrightText: 2009-2023 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Performance profiling utilities for category/tag system.
Use to identify slow functions during UI draw calls.
"""

import time
from contextlib import contextmanager

# Global storage for timing data
_perf_timings = {}
_perf_enabled = True  # Set to False to disable all profiling


def _log_perf(message, elapsed_time):
    """Log performance data with consistent format."""
    if not _perf_enabled:
        return
    
    elapsed_ms = elapsed_time * 1000
    print(f"[PERF] {message}: {elapsed_ms:.2f}ms")
    
    # Store for later analysis
    if message not in _perf_timings:
        _perf_timings[message] = []
    _perf_timings[message].append(elapsed_ms)


@contextmanager
def profile_block(name):
    """
    Context manager for profiling a code block.
    
    Usage:
        with profile_block("my_function"):
            # code to profile
    """
    if not _perf_enabled:
        yield
        return
    
    start = time.perf_counter()
    try:
        yield
    finally:
        elapsed = time.perf_counter() - start
        _log_perf(name, elapsed)


def profile_function(func):
    """
    Decorator for profiling a function.
    
    Usage:
        @profile_function
        def my_function():
            # code to profile
    """
    if not _perf_enabled:
        return func
    
    def wrapper(*args, **kwargs):
        func_name = f"{func.__module__}.{func.__qualname__}"
        with profile_block(func_name):
            return func(*args, **kwargs)
    
    return wrapper


def print_perf_summary():
    """Print summary of all collected timing data."""
    if not _perf_timings:
        print("[PERF] No timing data collected")
        return
    
    print("\n" + "=" * 80)
    print("[PERF] TIMING SUMMARY")
    print("=" * 80)
    
    for name, times in sorted(_perf_timings.items()):
        if not times:
            continue
        
        count = len(times)
        total = sum(times)
        avg = total / count
        min_time = min(times)
        max_time = max(times)
        
        print(f"{name}:")
        print(f"  Count: {count}, Total: {total:.2f}ms")
        print(f"  Avg: {avg:.2f}ms, Min: {min_time:.2f}ms, Max: {max_time:.2f}ms")
        print()
    
    print("=" * 80)


def clear_perf_data():
    """Clear all collected timing data."""
    _perf_timings.clear()
    print("[PERF] Timing data cleared")


def enable_profiling():
    """Enable performance profiling."""
    global _perf_enabled
    _perf_enabled = True
    print("[PERF] Profiling enabled")


def disable_profiling():
    """Disable performance profiling."""
    global _perf_enabled
    _perf_enabled = False
    print("[PERF] Profiling disabled")
