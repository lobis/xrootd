# Public EOS range-read comparison (2026-09-25)

`compare_fsspec.py` read a public 1,522,131,812-byte CMS ROOT file from
`root://eospublic.cern.ch`. It used XRootD client read operations only. The
script checks that both implementations return the same bytes for every
scenario and records the SHA-256 hashes in the adjacent JSON files. No server
administration or writes were performed.

These measurements use native implementation commit
`ac7af6fdad4a77ba69f978169491625710b40371`, after range normalization
and the first CodeRabbit fixes. The current implementation commit
`435cf415bc83704154993adf2acd6a08a716934c` additionally corrects
directory-listing paths; its read path is unchanged.

Environment: macOS 27 arm64, Python 3.14.7, XRootD client 6.1.1, fsspec
2026.2.0, and `fsspec-xrootd` at commit `54de1d7ef773`. The native adapter
was run from this PR's source tree against the locally installed XRootD
extension. Each adapter read the same offsets for five rounds; the order was
alternated between rounds. The table shows the median of rounds 2–5, which
reuse each adapter's handles and connections.

| Read pattern, fsspec 2026.2.0 | Bytes per round | fsspec-xrootd | XRootD native |
| --- | ---: | ---: | ---: |
| 128 × 4 KiB scattered | 512 KiB | 23.07 ms | 31.79 ms |
| 32 × 16 KiB scattered | 512 KiB | 23.36 ms | 28.74 ms |
| 1 × 4 MiB sequential | 4 MiB | 141.96 ms | 149.26 ms |

The same five-round comparison was repeated with fsspec 2026.9.0, which
Uproot 5.7.6 installed in the integration environment. Its full timings are
in `2026-09-25-eospublic-fsspec-2026.9.json`.

| Read pattern, fsspec 2026.9.0 | Bytes per round | fsspec-xrootd | XRootD native |
| --- | ---: | ---: | ---: |
| 128 × 4 KiB scattered | 512 KiB | 39.51 ms | 31.47 ms |
| 32 × 16 KiB scattered | 512 KiB | 30.10 ms | 26.93 ms |
| 1 × 4 MiB sequential | 4 MiB | 139.26 ms | 143.31 ms |

These runs do not identify a reliable winner. The 2026.2 run favored the
external adapter, whereas the 2026.9 run favored the native adapter for the
scattered reads. Network and server-cache conditions also changed between
runs. The first 2026.9 scattered call took more than one second for both
adapters and is recorded in the JSON; prior runs had already accessed the
file, so it is not a controlled cold-cache measurement. This measures adapter
range reads, not end-to-end analysis throughput.

For a separate integration smoke test, Uproot 5.7.6 with fsspec 2026.9.0
opened the same file through the native adapter and found the `Events` tree
(16,639 entries and 5,448 branches). It also opened the file and listed the
same top-level keys through `fsspec-xrootd`. That smoke test did not measure
Uproot or Coffea throughput; Coffea was not installed.

The `lobis-eos-dev` namespace was also reachable for client-side stat/list
checks, but nonempty file opens expired during the run. It was therefore not
used for performance figures. This says nothing about a persistent server
fault; it only bounds this test.

Run with both adapters available on `PYTHONPATH` or installed in the active
environment:

```sh
python3 python/benchmarks/compare_fsspec.py \
  root://eospublic.cern.ch//eos/opendata/cms/Run2010B/Mu/AOD/Apr21ReReco-v1/0000/00459D48-EB70-E011-AF09-90E6BA19A252.root \
  --rounds 5 --timeout 30
```
