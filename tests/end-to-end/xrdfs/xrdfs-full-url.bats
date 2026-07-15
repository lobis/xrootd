#!/usr/bin/env bash

bats_require_minimum_version 1.5.0

bats_load_library 'bats-support'
bats_load_library 'bats-assert'

load ../helper/common.bash

setup() {
    mkdir -p "$BATS_TEST_TMPDIR/xrdfs-full-url/data"
    printf 'first' > "$BATS_TEST_TMPDIR/xrdfs-full-url/data/first.txt"
    printf 'second' > "$BATS_TEST_TMPDIR/xrdfs-full-url/data/second.txt"
    : > "$BATS_TEST_TMPDIR/xrdfs-full-url/data/empty.txt"
    printf '\000\001\177\200\377' \
        > "$BATS_TEST_TMPDIR/xrdfs-full-url/data/binary.dat"
    printf 'dash' > "$BATS_TEST_TMPDIR/xrdfs-full-url/-dash.txt"

    launch_xrootd xrdfs-full-url.cfg xrdfs-full-url

    export TEST_ENDPOINT=root://localhost:11965
    export TEST_DIRECTORY=$TEST_ENDPOINT//data/
    export TEST_FILE=$TEST_ENDPOINT//data/first.txt
    export TEST_SECOND_FILE=$TEST_ENDPOINT//data/second.txt
    export TEST_EMPTY_FILE=$TEST_ENDPOINT//data/empty.txt
    export TEST_BINARY_FILE=$TEST_ENDPOINT//data/binary.dat

    local ready=false
    for _ in {1..50}; do
        if "$XRDFS" "$TEST_ENDPOINT" stat /data/first.txt \
            >/dev/null 2>&1; then
            ready=true
            break
        fi
        sleep 0.1
    done
    "$ready"

    # Seed one attribute on the ephemeral local fixture. The commands under
    # test only use the read-only list/get forms.
    "$XRDFS" "$TEST_ENDPOINT" xattr /data/first.txt set \
        user.test=fixture >/dev/null
}

teardown() {
    # The server can exit between the last command and teardown. Cleanup must
    # remain idempotent when a recorded PID has already disappeared.
    kill_pid_files 2>/dev/null || true
}

bats::on_failure() {
    print_log_files
}

@test "legacy and complete-URL stat forms have identical output" {
    run "$XRDFS" "$TEST_ENDPOINT" stat /data/first.txt
    assert_success
    local legacy_output=$output

    run "$XRDFS" stat "$TEST_FILE"
    assert_success
    assert_output "$legacy_output"
    assert_output --partial 'Size:   5'
}

@test "legacy and complete-URL ls forms have identical output" {
    run "$XRDFS" "$TEST_ENDPOINT" ls -l /data/
    assert_success
    local legacy_output=$output

    run "$XRDFS" ls -l "$TEST_DIRECTORY"
    assert_success
    assert_output "$legacy_output"
    assert_output --partial first.txt
}

@test "ls accepts gfal human-readable and directory options" {
    run "$XRDFS" ls -lH "$TEST_DIRECTORY"
    assert_success
    assert_output --partial first.txt

    run "$XRDFS" ls --long --human-readable --directory "$TEST_DIRECTORY"
    assert_success
    assert_output --partial /data/
    refute_output --partial first.txt
}

@test "ls option delimiter preserves dash-prefixed paths" {
    run bash -c \
        'printf "cd /\nls -- -dash.txt\nexit\n" | "$1" "$2"' \
        _ "$XRDFS" "$TEST_ENDPOINT"
    assert_success
    assert_output --partial /-dash.txt
}

@test "legacy and complete-URL cat forms have identical output" {
    run "$XRDFS" "$TEST_ENDPOINT" cat /data/first.txt
    assert_success
    local legacy_output=$output

    run "$XRDFS" cat "$TEST_FILE"
    assert_success
    assert_output "$legacy_output"
    assert_output first
}

@test "cat accepts gfal bytes options and multiple same-endpoint URLs" {
    run "$XRDFS" cat -b "$TEST_FILE" "$TEST_SECOND_FILE"
    assert_success
    assert_output firstsecond

    run "$XRDFS" cat --bytes "$TEST_EMPTY_FILE"
    assert_success
    assert_output ''

    local destination="$BATS_TEST_TMPDIR/binary-download.dat"
    run bash -c '"$1" cat -b "$2" > "$3"' \
        _ "$XRDFS" "$TEST_BINARY_FILE" "$destination"
    assert_success
    run cmp "$BATS_TEST_TMPDIR/xrdfs-full-url/data/binary.dat" "$destination"
    assert_success
}

@test "cat bytes compatibility option still requires a file" {
    run "$XRDFS" "$TEST_ENDPOINT" cat -b
    assert_failure
    assert_output --partial 'Invalid arguments'
}

@test "cat option delimiter preserves dash-prefixed paths" {
    run bash -c \
        'printf "cd /\ncat -- -dash.txt\nexit\n" | "$1" "$2"' \
        _ "$XRDFS" "$TEST_ENDPOINT"
    assert_success
    assert_output --partial dash
}

@test "xattr accepts gfal read-only list and get shorthand" {
    run "$XRDFS" "$TEST_ENDPOINT" xattr /data/first.txt list
    assert_success
    local list_output=$output

    run "$XRDFS" xattr "$TEST_FILE"
    assert_success
    assert_output "$list_output"
    assert_output --partial 'user.test="fixture"'

    run "$XRDFS" "$TEST_ENDPOINT" xattr /data/first.txt get user.test
    assert_success
    local get_output=$output

    run "$XRDFS" xattr "$TEST_FILE" user.test
    assert_success
    assert_output "$get_output"
}

@test "URL-like xattr values remain command operands" {
    run "$XRDFS" xattr "$TEST_FILE" set link=https://example.org/resource
    assert_success

    run "$XRDFS" xattr "$TEST_FILE" get link
    assert_success
    assert_output --partial 'link="https://example.org/resource"'
}

@test "sum maps gfal positional arguments to the native checksum query" {
    run "$XRDFS" "$TEST_ENDPOINT" query checksum \
        '/data/first.txt?cks.type=adler32'
    assert_success
    local query_output=$output

    run "$XRDFS" sum "$TEST_FILE" ADLER32
    assert_success
    assert_output "$query_output"
    assert_output --regexp '^adler32 [[:xdigit:]]{8}$'

    run "$XRDFS" "$TEST_ENDPOINT" sum /data/first.txt adler32
    assert_success
    assert_output "$query_output"
}

@test "sum preserves existing URL parameters when selecting an algorithm" {
    run "$XRDFS" sum "$TEST_FILE?xrdcl.test=1" ADLER32
    assert_success
    assert_output --regexp '^adler32 [[:xdigit:]]{8}$'
}

@test "sum rejects invalid and unsupported checksum types" {
    run "$XRDFS" sum "$TEST_FILE" 'md5&injected=true'
    assert_failure

    run "$XRDFS" sum "$TEST_FILE" sha256
    assert_failure
}

@test "URL parameters are preserved in the operand path" {
    run "$XRDFS" stat "$TEST_FILE?xrdcl.test=1"
    assert_success
    assert_output --partial 'Size:   5'
}

@test "mixed URL endpoints are rejected before execution" {
    run "$XRDFS" cat "$TEST_FILE" \
        root://127.0.0.1:11965//data/second.txt
    assert_failure 1
    assert_output 'xrdfs: all URL operands must use the same endpoint'
}

@test "mixed URL credentials are rejected before execution" {
    run "$XRDFS" mv root://alice@localhost:11965//data/first.txt \
        root://bob@localhost:11965//data/renamed.txt
    assert_failure 1
    assert_output 'xrdfs: all URL operands must use the same endpoint'
}

@test "local and malformed URL operands are rejected" {
    for url in file:///tmp/examplefile FILE://localhost/tmp/examplefile \
        STDIO://-/examplefile '1root://localhost:11965//data/first.txt'; do
        run "$XRDFS" stat "$url"
        assert_failure 1
        assert_output 'xrdfs: invalid remote URL operand'
    done
}

@test "command-first raw queries remain unsupported" {
    run "$XRDFS" query "$TEST_FILE"
    assert_failure 1
    assert_output "xrdfs: command-first full URLs are not supported for 'query'"
}

@test "missing paths fail through the existing command handler" {
    run "$XRDFS" stat "$TEST_ENDPOINT//data/missing.txt"
    refute_output --partial 'invalid remote URL operand'
    assert_failure
}
