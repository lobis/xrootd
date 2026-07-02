#!/usr/bin/env bash

set -euo pipefail

: "${XRD:?XRD must point to the xrd executable}"

tmpdir=$(mktemp -d)
trap 'rm -rf "${tmpdir}"' EXIT

printf 'hello\n' > "${tmpdir}/first"
printf 'world\n' > "${tmpdir}/second"
head -c 100000 /dev/urandom > "${tmpdir}/binary"

help_out=$("${XRD}" cat --help)
for expected in \
  "xrd cat" \
  "-V" \
  "--version" \
  "-D" \
  "--definition" \
  "-t" \
  "--timeout" \
  "-E" \
  "--key" \
  "-4" \
  "-6" \
  "-C" \
  "--client-info" \
  "--log-file" \
  "-b" \
  "--bytes"
do
  if ! grep -F -- "${expected}" <<< "${help_out}" >/dev/null; then
    echo "xrd cat --help is missing '${expected}'" >&2
    exit 1
  fi
done

version_out=$("${XRD}" cat --version)
if ! grep -E '^xrd v?[0-9]' <<< "${version_out}" >/dev/null; then
  echo "xrd cat --version did not print a version string" >&2
  exit 1
fi

# Single file content.
single_out=$("${XRD}" cat "${tmpdir}/first")
if [[ "${single_out}" != "hello" ]]; then
  echo "xrd cat printed '${single_out}'" >&2
  exit 1
fi

# Multiple files are concatenated in order.
multi_out=$("${XRD}" cat "${tmpdir}/first" "${tmpdir}/second")
expected_multi=$(printf 'hello\nworld\n')
if [[ "${multi_out}" != "${expected_multi}" ]]; then
  echo "xrd cat concatenation mismatch: '${multi_out}'" >&2
  exit 1
fi

# Binary content is preserved byte for byte.
if ! "${XRD}" cat "${tmpdir}/binary" | cmp -s - "${tmpdir}/binary"; then
  echo "xrd cat corrupted binary content" >&2
  exit 1
fi

# The -b compatibility flag is accepted.
bytes_out=$("${XRD}" cat -b "${tmpdir}/first")
if [[ "${bytes_out}" != "hello" ]]; then
  echo "xrd cat -b printed '${bytes_out}'" >&2
  exit 1
fi

# Missing files exit with ENOENT.
set +e
"${XRD}" cat "${tmpdir}/missing" >/dev/null 2>&1
missing_code=$?
set -e
if [[ "${missing_code}" != 2 ]]; then
  echo "xrd cat on a missing path exited with ${missing_code}, expected 2" >&2
  exit 1
fi

echo "ALL OK"
