#!/usr/bin/env bash
#
# Purging. This is the code that deletes cached data, so what has to be pinned
# down is not only that it frees space but that it frees the right files: least
# recently accessed first.
#
# Slow by construction. The resource monitor checks for a required purge on a
# fixed 60 s cycle, and files that were open recently are purge-protected, so
# reaching the target usually takes more than one cycle. There is no way to ask
# for a purge on demand.

export XrdSecPROTOCOL=host

# shellcheck source=/dev/null
source "$(dirname "${BASH_SOURCE[0]}")/cachelib.sh"

PFC_BLOCKSIZE=$((128 * 1024))

# No PFC_GSTREAM_PORT: every assertion here reads the cache directory, so this
# test does not need a g-stream collector.

# Must match the "files <baseline> <nominal> <max>" budget in cachepurge.cfg.
PFC_FILES_MAX=$((4 * 1024 * 1024))

# Two groups of files, read a couple of seconds apart so their access times
# fall either side of a clear boundary. Together they overrun the budget.
PFC_GROUP=4
PFC_FILE_MB=1

function setup_cachepurge() {
	cache_setup
}

function cached_file() {
	echo "$(cache_store)/pfc-purge-$1.dat"
}

# Total bytes of cached data files, ignoring cinfo and the stats directory.
function cached_bytes() {
	local total=0 f
	for f in "$(cache_store)"/pfc-purge-*.dat; do
		[[ -f "${f}" ]] || continue
		total=$((total + $(file_bytes "${f}")))
	done
	echo "${total}"
}

# Space-separated indices of the files still cached, in order.
function cached_indices() {
	local i out=()
	for i in $(seq 1 $((2 * PFC_GROUP))); do
		[[ -f "$(cached_file "${i}")" ]] && out+=("${i}")
	done
	echo "${out[*]}"
}

# A completed download can still have an open access record. Wait for the
# close write too, since its cinfo mtime is what purge uses to order files.
function wait_for_purge_cinfo() {
	local nblk ndone state detached
	for _ in $(seq 100); do
		if [[ -f "$1" ]]; then
			read -r nblk ndone state <<< "$(cinfo_blocks "$1")"
			detached="$(xrdpfc_print -u B "$1" | awk '$1 == 1 { print $3; exit }')"
			if [[ "${state}" == complete ]] && [[ "${nblk}" == "${ndone}" ]] &&
			   [[ -n "${detached}" ]] && [[ "${detached}" != ------:------ ]]; then
				return 0
			fi
		fi
		sleep 0.2
	done
	report_cinfo_state "$1"
	echo "  first access record detach: ${detached:-missing}"
	error "timed out after 20 s waiting for complete, closed cinfo $1"
}

function test_cachepurge() {
	local i n_total=$((2 * PFC_GROUP))

	#---------------------------------------------------------------------------
	echo "--- fill the cache past its file-usage budget"
	#---------------------------------------------------------------------------

	assert dd if=/dev/urandom of=purge-src.dat bs=1048576 count="${PFC_FILE_MB}" status=none
	for i in $(seq 1 ${n_total}); do
		assert xrdcp -fs purge-src.dat "${ORIGIN_HOST}//pfc-purge-${i}.dat"
	done

	# Group A first, then a pause, then group B, so that every file in B is
	# strictly more recently accessed than every file in A.
	for i in $(seq 1 ${PFC_GROUP}); do
		assert xrdcp -fs "${HOST}//pfc-purge-${i}.dat" "purge-out-${i}.dat"
	done
	for i in $(seq 1 ${PFC_GROUP}); do
		wait_for_purge_cinfo "$(cinfo_path "pfc-purge-${i}.dat")"
	done
	sleep 3
	for i in $(seq $((PFC_GROUP + 1)) ${n_total}); do
		assert xrdcp -fs "${HOST}//pfc-purge-${i}.dat" "purge-out-${i}.dat"
	done
	for i in $(seq $((PFC_GROUP + 1)) ${n_total}); do
		wait_for_purge_cinfo "$(cinfo_path "pfc-purge-${i}.dat")"
	done

	# Purge compares whole-second cinfo mtimes. Check the actual boundary,
	# rather than assuming that read order produced distinct timestamps.
	assert python3 - "$(cache_store)" "${PFC_GROUP}" <<'PY'
import os
import sys

store, group = sys.argv[1], int(sys.argv[2])
mtimes = []
for i in range(1, 2 * group + 1):
    path = os.path.join(store, "pfc-purge-{}.dat.cinfo".format(i))
    mtimes.append(int(os.stat(path).st_mtime))
old, new = mtimes[:group], mtimes[group:]
print("cinfo mtimes: old={}, new={}".format(old, new))
if max(old) >= min(new):
    sys.exit(
        "cache access cohorts overlap: every old cinfo must predate "
        "every new cinfo"
    )
PY

	local filled
	filled="$(cached_bytes)"
	if [[ "${filled}" -le "${PFC_FILES_MAX}" ]]; then
		error "test did not overrun the budget: cached ${filled} B, max is ${PFC_FILES_MAX} B"
	fi
	# Build the expected list the same way cached_indices does. Not
	# "seq -s' '": BSD seq puts the separator after the last number too, so
	# the two sides differed by a trailing space on macOS.
	local expected=()
	for i in $(seq 1 ${n_total}); do
		expected+=("${i}")
	done
	assert_eq "${expected[*]}" "$(cached_indices)" \
		"all files should be cached before purging"

	#---------------------------------------------------------------------------
	echo "--- wait for the purge to bring usage back under the budget"
	#---------------------------------------------------------------------------

	# The check runs once a minute and purge protection on recently used files
	# means it can take more than one pass to converge. Wait for usage to fall
	# to the max, not to the nominal it aims for: how far a single pass gets
	# depends on what was protected at the time, and a run that stops at the
	# max has still enforced the budget.
	local now
	for _ in $(seq 300); do
		now="$(cached_bytes)"
		[[ "${now}" -le "${PFC_FILES_MAX}" ]] && break
		sleep 1
	done

	now="$(cached_bytes)"
	if [[ "${now}" -gt "${PFC_FILES_MAX}" ]]; then
		error "purge did not bring usage down: ${now} B still cached," \
		      "the limit is ${PFC_FILES_MAX} B"
	fi
	echo "cached bytes: ${filled} before, ${now} after"

	#---------------------------------------------------------------------------
	echo "--- the files it removed are the least recently used ones"
	#---------------------------------------------------------------------------

	# The exact number purged is not fixed -- it depends on how many passes ran
	# and on what was purge-protected at the time -- but the ordering is the
	# contract: a file from the newer group must not be dropped while one from
	# the older group is kept. Files within a group can have equal mtimes.
	local survivors
	survivors="$(cached_indices)"
	echo "survivors: ${survivors:-none}"

	local old_survivor=0 new_purged=0 new_survivor=0 newest_purged=0
	for i in $(seq 1 ${n_total}); do
		if [[ -f "$(cached_file "${i}")" ]]; then
			if [[ "${i}" -le "${PFC_GROUP}" ]]; then
				old_survivor="${i}"
			else
				new_survivor="${i}"
			fi
		else
			newest_purged="${i}"
			if [[ "${i}" -gt "${PFC_GROUP}" ]]; then
				new_purged="${i}"
			fi
		fi
	done

	if [[ "${newest_purged}" -eq 0 ]]; then
		error "nothing was purged even though usage came down; the test is not testing purge"
	fi
	if [[ "${new_survivor}" -eq 0 ]]; then
		error "purge removed every file from the newer group; survivors were: ${survivors:-none}"
	fi
	if [[ "${old_survivor}" -ne 0 ]] && [[ "${new_purged}" -ne 0 ]]; then
		error "purge kept old file ${old_survivor} but dropped newer" \
		      "file ${new_purged}; survivors were: ${survivors}"
	fi

	#---------------------------------------------------------------------------
	echo "--- a purged file is gone from the cache, not just hidden"
	#---------------------------------------------------------------------------

	local victim="pfc-purge-${newest_purged}.dat"

	if [[ -e "$(cinfo_path "${victim}")" ]]; then
		error "purge removed the data file but left the cinfo: $(cinfo_path "${victim}")"
	fi

	# It still reads correctly, and reading it puts it back in the cache --
	# which it could only do by going to the origin, since the cinfo above is
	# gone. Asserted through the cinfo rather than the g-stream so that this
	# long test depends on nothing but the filesystem.
	assert xrdcp -f "${HOST}//${victim}" purge-refetch.dat
	assert cmp purge-src.dat purge-refetch.dat

	wait_for_access_record "$(cinfo_path "${victim}")" 1
	local n_blocks=$((PFC_FILE_MB * 1024 * 1024 / PFC_BLOCKSIZE))
	assert_eq "${n_blocks} ${n_blocks} complete" "$(cinfo_blocks "$(cinfo_path "${victim}")")" \
		"a purged file should be fetched from the origin again"
}
