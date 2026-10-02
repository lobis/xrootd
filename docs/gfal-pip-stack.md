# Pinned GFAL pip client stack

This development branch assembles the following topics over XRootD v6.2.0
(`06570df04beefd3735219e122ea6168571eda2ae`). It is intended for the GFAL
native CLI package; GFAL still invokes subprocesses rather than the bindings.

| Topic | Original pinned head | Purpose |
| --- | --- | --- |
| [Fork #58](https://github.com/lobis/xrootd/pull/58) | `270be8eea9ec92482f25de2995c11b8d8c544452` | Pythonic helpers, scoped authentication/WebDAV and checksum error propagation; inherited base topics remain intact. |
| [Upstream #2920](https://github.com/xrootd/xrootd/pull/2920) | `3ace41a99e2d59360c7a0720e9b5e68458545039` | Six native CLI/checksum helper commits. |
| [Upstream #2945](https://github.com/xrootd/xrootd/pull/2945) | `d2fa26ca7e4492dc176f5a49ec51ab2aa60110b5` | Padded Base64 CRC32 HTTP response digests. |
| [Fork #61](https://github.com/lobis/xrootd/pull/61) | `e84334cbf0929f07c2be179660e622de0203ba18` | Pip wheels containing xrdfs, xrdcp, xrdcopy, matching libraries and HTTP/security plugins. |

Cherry-picked commits retain their original provenance. The wheel packaging
conflict resolution preserves the base topic's Python version requirement.
The explicit VERSION avoids dependence on fetched Git tags. GFAL must pin an
immutable commit from this branch, not its moving branch name.

This stack does not include the separate xrdtoken implementation, recursive
HTTP directory copying, or the older development JSON/TPC compatibility stack.
No fsspec extra is enabled by GFAL.
