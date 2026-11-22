#!/usr/bin/env python3
"""SDNX 16.188: set the program summary of .gcda files that gcov-tool merge-stream made from the console's stream.

__gcov_info_to_gcda (nx/sdretail/src/sdnx_pgo.cpp) leaves each file's OBJECT_SUMMARY record at zero. -fprofile-use
reads two numbers from it: runs (how many program runs the counts cover: a block whose count is below that is "never
executed") and sum_max (the largest arc count: "hot" is a fraction of it). Zeros would make every block hot. This
writes runs = <sessions> and sum_max = the largest arc counter found in any file, what libgcov's own exit-time dump
would have written for one run per session. (The approach is the one renut_nx_ultimate_base documents for its PGO
streams; this is an independent implementation.)

  python3 sd_pgo_summary.py <folder with .gcda> <sessions>

GCC 12+ gcda layout, little-endian target: four u32 words (magic 'gcda', version, stamp, checksum), then records of
(u32 tag, s32 length in bytes, payload). Tag 0xA1000000 = object summary (u32 runs, u32 sum_max); tag 0x01A10000 =
arc counters (u64 each); a negative length on a counter record means that many bytes of zero counters, no payload.
"""
import os
import struct
import sys

GCDA_MAGIC = 0x67636461
TAG_OBJECT_SUMMARY = 0xA1000000
TAG_ARC_COUNTERS = 0x01A10000


def walk_records(blob):
    magic, = struct.unpack_from('<I', blob, 0)
    if magic != GCDA_MAGIC:
        raise ValueError('not a little-endian .gcda')
    at = 16
    while at + 8 <= len(blob):
        tag, size = struct.unpack_from('<Ii', blob, at)
        payload = at + 8
        if size < 0:  # zero counters, stored as their (negated) size only
            yield tag, payload, 0
            at = payload
        else:
            yield tag, payload, size
            at = payload + size


def gcda_files(folder):
    for top, _, names in os.walk(folder):
        for name in names:
            if name.endswith('.gcda'):
                yield os.path.join(top, name)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder, sessions = sys.argv[1], int(sys.argv[2])
    paths = list(gcda_files(folder))
    largest = 0
    for path in paths:
        blob = open(path, 'rb').read()
        for tag, payload, size in walk_records(blob):
            if tag == TAG_ARC_COUNTERS and size:
                counters = struct.unpack_from('<%dQ' % (size // 8), blob, payload)
                largest = max(largest, max(counters))
    sum_max = min(largest, 0xFFFFFFFF)
    done = 0
    for path in paths:
        blob = bytearray(open(path, 'rb').read())
        for tag, payload, size in walk_records(blob):
            if tag == TAG_OBJECT_SUMMARY and size >= 8:
                struct.pack_into('<II', blob, payload, sessions, sum_max)
                done += 1
                break
        with open(path, 'wb') as f:
            f.write(blob)
    print('%d .gcda files, %d summaries set: runs = %d, sum_max = %d (largest arc counter %d)' %
          (len(paths), done, sessions, sum_max, largest))


if __name__ == '__main__':
    main()
