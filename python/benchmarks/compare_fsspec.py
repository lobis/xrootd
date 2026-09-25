"""Compare native and scikit-hep fsspec adapters on read-only ranges.

Run with both ``xrootd[fsspec]`` and ``fsspec-xrootd`` installed. Import the
classes directly because both packages register the same ``root`` protocol.
"""

import argparse
import hashlib
import json
import statistics
import time

from XRootD import client
from XRootD.client.fsspec import XRootDFileSystem as NativeFileSystem
from fsspec_xrootd import XRootDFileSystem as ExternalFileSystem


def ranges(size, count, length):
    span = min(size, 256 * 1024 * 1024)
    if span < length:
        raise ValueError('file is too small for the benchmark')
    if count == 1:
        starts = [0]
    else:
        starts = [(span - length) * index // (count - 1)
                  for index in range(count)]
    return starts, [start + length for start in starts]


def checksum(chunks):
    digest = hashlib.sha256()
    for chunk in chunks:
        if not isinstance(chunk, bytes):
            raise TypeError('range read returned %r' % type(chunk).__name__)
        digest.update(len(chunk).to_bytes(8, 'big'))
        digest.update(chunk)
    return digest.hexdigest()


def run(url, rounds, timeout):
    parsed = client.URL(url)
    path = parsed.path_with_params
    backends = {
        'fsspec-xrootd': ExternalFileSystem(hostid=parsed.hostid,
                                            timeout=timeout),
        'xrootd-native': NativeFileSystem(hostid=parsed.hostid,
                                          timeout=timeout),
    }
    try:
        size = backends['xrootd-native'].info(path)['size']
        scenarios = {
            'scattered-128x4KiB': ranges(size, 128, 4096),
            'scattered-32x16KiB': ranges(size, 32, 16384),
            'sequential-4MiB': ranges(size, 1, 4 * 1024 * 1024),
        }
        result = {'url': url, 'size': size, 'rounds': rounds, 'scenarios': {}}
        for scenario, (starts, ends) in scenarios.items():
            timings = {name: [] for name in backends}
            checksums = {}
            for iteration in range(rounds):
                names = list(backends)
                if iteration % 2:
                    names.reverse()
                for name in names:
                    fs = backends[name]
                    begin = time.perf_counter()
                    chunks = fs.cat_ranges([path] * len(starts), starts, ends)
                    elapsed = time.perf_counter() - begin
                    digest = checksum(chunks)
                    if checksums and digest != next(iter(checksums.values())):
                        raise AssertionError(
                            'backends returned different bytes')
                    checksums[name] = digest
                    timings[name].append(elapsed)
            result['scenarios'][scenario] = {
                'bytes_per_round': sum(end - start for start, end in
                                       zip(starts, ends)),
                'seconds': timings,
                'cold_seconds': {name: values[0]
                                 for name, values in timings.items()},
                'warm_median_seconds': {
                    name: statistics.median(values[1:])
                    for name, values in timings.items() if len(values) > 1},
                'sha256': next(iter(checksums.values())),
            }
        return result
    finally:
        backends['xrootd-native'].close()
        backends['fsspec-xrootd'].invalidate_cache()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url', help='readable root:// URL')
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--timeout', type=int, default=30)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('--rounds must be positive')
    print(json.dumps(run(args.url, args.rounds, args.timeout), indent=2))


if __name__ == '__main__':
    main()
