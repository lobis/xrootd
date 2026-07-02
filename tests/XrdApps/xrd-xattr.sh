#!/usr/bin/env bash

set -euo pipefail

: "${XRD:?XRD must point to the xrd executable}"

tmpdir=$(mktemp -d)
trap 'rm -rf "${tmpdir}"' EXIT

file="${tmpdir}/xattr-file"
touch "${file}"

help_out=$("${XRD}" xattr --help)
for expected in \
  "xrd xattr" \
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
  "attribute"
do
  if ! grep -F -- "${expected}" <<< "${help_out}" >/dev/null; then
    echo "xrd xattr --help is missing '${expected}'" >&2
    exit 1
  fi
done

version_out=$("${XRD}" xattr --version)
if ! grep -E '^xrd v?[0-9]' <<< "${version_out}" >/dev/null; then
  echo "xrd xattr --version did not print a version string" >&2
  exit 1
fi

# Listing attributes of a plain file succeeds (possibly with no output).
"${XRD}" xattr "${file}" >/dev/null

# Missing files exit with ENOENT.
set +e
"${XRD}" xattr "${tmpdir}/missing" >/dev/null 2>&1
missing_code=$?
set -e
if [[ "${missing_code}" != 2 ]]; then
  echo "xrd xattr on a missing path exited with ${missing_code}, expected 2" >&2
  exit 1
fi

# Setting attributes is not implemented yet and must fail clearly.
set +e
set_err=$("${XRD}" xattr "${file}" user.name=value 2>&1 >/dev/null)
set_code=$?
set -e
if [[ "${set_code}" == 0 ]]; then
  echo "xrd xattr key=value unexpectedly succeeded" >&2
  exit 1
fi
if ! grep -F "not implemented" <<< "${set_err}" >/dev/null; then
  echo "xrd xattr key=value did not report not implemented: ${set_err}" >&2
  exit 1
fi

# When the platform supports user xattrs, verify get and list output.
attr_set=false
if command -v xattr >/dev/null 2>&1; then
  if xattr -w user.test hello "${file}" 2>/dev/null; then
    attr_set=true
  fi
elif command -v setfattr >/dev/null 2>&1; then
  if setfattr -n user.test -v hello "${file}" 2>/dev/null; then
    attr_set=true
  fi
fi

if [[ "${attr_set}" == true ]]; then
  get_out=$("${XRD}" xattr "${file}" user.test)
  if [[ "${get_out}" != "hello" ]]; then
    echo "xrd xattr get printed '${get_out}'" >&2
    exit 1
  fi

  list_out=$("${XRD}" xattr "${file}")
  if ! grep -F "user.test = hello" <<< "${list_out}" >/dev/null; then
    echo "xrd xattr list output mismatch:" >&2
    echo "${list_out}" >&2
    exit 1
  fi
else
  echo "NOTE: could not set a user xattr; skipping value checks"
fi

echo "ALL OK"
