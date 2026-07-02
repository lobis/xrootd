#!/usr/bin/env bash

set -euo pipefail

: "${XRD:?XRD must point to the xrd executable}"

tmpdir=$(mktemp -d)
trap 'rm -rf "${tmpdir}"' EXIT

dir="${tmpdir}/ls-directory"
mkdir "${dir}"
printf 'hello\n' > "${dir}/bfile"
chmod 644 "${dir}/bfile"
touch "${dir}/afile" "${dir}/.hidden"
mkdir "${dir}/subdir"

help_out=$("${XRD}" ls --help)
for expected in \
  "xrd ls" \
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
  "-a" \
  "--all" \
  "-l" \
  "--long" \
  "-d" \
  "--directory" \
  "-H" \
  "--human-readable" \
  "--xattr" \
  "--time-style" \
  "--full-time" \
  "--color"
do
  if ! grep -F -- "${expected}" <<< "${help_out}" >/dev/null; then
    echo "xrd ls --help is missing '${expected}'" >&2
    exit 1
  fi
done

version_out=$("${XRD}" ls --version)
if ! grep -E '^xrd v?[0-9]' <<< "${version_out}" >/dev/null; then
  echo "xrd ls --version did not print a version string" >&2
  exit 1
fi

# Default listing: sorted names, hidden entries excluded.
default_out=$("${XRD}" ls "${dir}")
expected_default=$(printf 'afile\nbfile\nsubdir\n')
if [[ "${default_out}" != "${expected_default}" ]]; then
  echo "xrd ls default output mismatch:" >&2
  echo "${default_out}" >&2
  exit 1
fi

# -a shows hidden entries.
all_out=$("${XRD}" ls -a "${dir}")
expected_all=$(printf '.hidden\nafile\nbfile\nsubdir\n')
if [[ "${all_out}" != "${expected_all}" ]]; then
  echo "xrd ls -a output mismatch:" >&2
  echo "${all_out}" >&2
  exit 1
fi

# Long format: mode, link count, uid, gid, size, date, name, trailing tab.
long_line=$("${XRD}" ls -l "${dir}" | grep 'bfile')
if ! grep -E $'^-rw-r--r-- +[0-9]+ +[0-9]+ +[0-9]+ +6 .* bfile\t$' \
  <<< "${long_line}" >/dev/null; then
  echo "xrd ls -l long format mismatch: '${long_line}'" >&2
  exit 1
fi

# Long format with long-iso timestamps.
long_iso_line=$("${XRD}" ls -l --time-style=long-iso "${dir}" | grep 'bfile')
if ! grep -E $'[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}' \
  <<< "${long_iso_line}" >/dev/null; then
  echo "xrd ls --time-style=long-iso mismatch: '${long_iso_line}'" >&2
  exit 1
fi

# --full-time matches gfal2-util behavior (long-iso output).
full_time_line=$("${XRD}" ls -l --full-time "${dir}" | grep 'bfile')
if ! grep -E $'[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}' \
  <<< "${full_time_line}" >/dev/null; then
  echo "xrd ls --full-time mismatch: '${full_time_line}'" >&2
  exit 1
fi

# Human readable sizes with -H.
human_line=$("${XRD}" ls -lH "${dir}" | grep 'bfile')
if ! grep -E ' 6\.0 ' <<< "${human_line}" >/dev/null; then
  echo "xrd ls -lH mismatch: '${human_line}'" >&2
  exit 1
fi

# Listing a file prints the path as given.
file_out=$("${XRD}" ls "${dir}/bfile")
if [[ "${file_out}" != "${dir}/bfile" ]]; then
  echo "xrd ls on a file printed '${file_out}'" >&2
  exit 1
fi

# -d lists the directory itself.
dir_out=$("${XRD}" ls -d "${dir}")
if [[ "${dir_out}" != "${dir}" ]]; then
  echo "xrd ls -d printed '${dir_out}'" >&2
  exit 1
fi

# Missing files exit with ENOENT.
set +e
"${XRD}" ls "${dir}/missing" >/dev/null 2>&1
missing_code=$?
set -e
if [[ "${missing_code}" != 2 ]]; then
  echo "xrd ls on a missing path exited with ${missing_code}, expected 2" >&2
  exit 1
fi

echo "ALL OK"
