# Using `xrdfs` for read-only gfal2-util workflows

This document covers the first compatibility slice for operators moving common
read-only workflows from gfal2-util to XRootD tools.

The implementation stays inside `xrdfs`. It does not add an `xrd` application,
a wrapper, or a second execution layer. Complete URLs and compatibility options
are normalized and passed to the existing `xrdfs` command handlers and XrdCl
operations.

This is a migration aid, not a promise of byte-for-byte gfal2-util
compatibility.

## Complete URL operands

Traditional `xrdfs` syntax separates the endpoint from the command and path:

```console
xrdfs root://storage.example.org stat //store/data/file.dat
```

The same operation can now be written in the command-first form used by
gfal2-util:

```console
xrdfs stat root://storage.example.org//store/data/file.dat
```

The traditional form remains supported.

When one invocation contains multiple complete URLs, they must all use the same
endpoint. `xrdfs` extracts that endpoint once and executes the existing command
against the normalized paths. Mixed endpoints are rejected before any remote
operation is attempted.

## Command mapping

Assume these example operands:

```sh
DIR='root://storage.example.org//store/data/'
FILE_A='root://storage.example.org//store/data/a.dat'
FILE_B='root://storage.example.org//store/data/b.dat'
```

| gfal2-util | `xrdfs` | Compatibility provided |
| --- | --- | --- |
| `gfal-stat "$FILE_A"` | `xrdfs stat "$FILE_A"` | Stats a complete URL using the existing `xrdfs stat` implementation. |
| `gfal-ls "$DIR"` | `xrdfs ls "$DIR"` | Lists the contents of the directory. |
| `gfal-ls -lH "$DIR"` | `xrdfs ls -lH "$DIR"` | Accepts the gfal-style `-H` human-readable-size option, including grouped `-lH`. The existing `xrdfs ls -h` spelling remains supported. |
| `gfal-ls -d "$DIR"` | `xrdfs ls -d "$DIR"` | Prints the directory operand itself instead of listing its contents. |
| `gfal-cat -b "$FILE_A"` | `xrdfs cat -b "$FILE_A"` | Accepts `-b` as a compatibility no-op because `xrdfs cat` already writes file data to standard output without text conversion. |
| `gfal-cat -b "$FILE_A" "$FILE_B"` | `xrdfs cat -b "$FILE_A" "$FILE_B"` | Concatenates multiple files from the same endpoint. |
| `gfal-sum "$FILE_A" ADLER32` | `xrdfs sum "$FILE_A" ADLER32` | Selects the requested checksum algorithm and validates that the server returned that algorithm. |
| `gfal-xattr "$FILE_A"` | `xrdfs xattr "$FILE_A"` | Uses the read-only shorthand to list extended attributes. |
| `gfal-xattr "$FILE_A" user.example` | `xrdfs xattr "$FILE_A" user.example` | Uses the read-only shorthand to retrieve one extended attribute. |

The existing native `xrdfs` xattr forms remain available:

```console
xrdfs xattr "$FILE_A" list
xrdfs xattr "$FILE_A" get user.example
```

Only list and get have implicit shorthand. Mutating xattr operations continue
to require their explicit native forms.

## Testing approach

The XRootD tests exercise the compatibility behavior against a local XRootD
server and controlled fixtures. They cover:

- complete-URL parsing and preservation of URL parameters;
- legacy server-first syntax;
- `stat`;
- normal, long human-readable, and directory-entry `ls`;
- binary-safe and multiple-file `cat`;
- algorithm-selecting checksum queries through `sum`;
- xattr list and get shorthand;
- rejection of local URLs and mixed remote endpoints.

The expected behavior is derived from the corresponding gfal2-util commands,
but gfal2 is not a build-time or runtime dependency of `xrdfs`, and it is not
required to run the XRootD test suite. The tests compare operation semantics
and relevant data, not exact diagnostic or presentation formatting.

## Compatibility boundaries

The output remains native `xrdfs` output. In particular, `stat`, long `ls`, and
xattr formatting may differ from gfal2-util in field names, ordering, path
presentation, size rounding, timestamps, and diagnostics. Scripts that parse
gfal2-util output should not assume identical text.

Exit statuses also remain XRootD statuses. Portable migration code should
distinguish success from failure rather than depending on a particular nonzero
value matching gfal2-util.

Accepting a complete URL does not add protocol support. Operations use the
XrdCl protocol implementation and plugins available in the installation. For
example, HTTP and HTTPS access requires the XrdCl HTTP plugin, and support for
an operation such as directory listing still depends on that plugin and the
remote server. Protocols supported by GFAL but without an XrdCl implementation
are not added by this compatibility work.

The following are intentionally outside this first slice:

- a new `xrd` command;
- raw `query` in command-first form;
- `gfal-copy` or `gfal-cp` command mapping;
- gfal2 common-option parity;
- exact output, diagnostic, or exit-code parity.
