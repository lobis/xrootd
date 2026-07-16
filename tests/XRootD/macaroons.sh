#!/usr/bin/env bash

X509_CERT_DIR="${BINARY_DIR}/tests/tls"
X509_USER_KEY="${X509_CERT_DIR}/client.key"
X509_USER_CERT="${X509_CERT_DIR}/client.crt"
XRD_HTTPCLIENTKEYFILE="${X509_USER_KEY}"
XRD_HTTPCLIENTCERTFILE="${X509_USER_CERT}"
MACAROONS_PLUGINCONFDIR="${PWD}/macaroons/client.plugins.d"

export XrdSecPROTOCOL X509_CERT_DIR X509_USER_KEY X509_USER_CERT
export XRD_HTTPCLIENTKEYFILE XRD_HTTPCLIENTCERTFILE

function setup_macaroons() {
	require_commands curl jq openssl

	mkdir -p "${MACAROONS_PLUGINCONFDIR}"
	cat >| "${MACAROONS_PLUGINCONFDIR}/http.conf" <<-EOF
	url = http://*;https://*;dav://*;davs://*
	lib = libXrdClHttp.so
	enable = true
	EOF

	cat >| macaroons.authdb <<-EOF
	u client /rw a
	u * / lr
	EOF

	cat >| macaroons.gridmap <<-EOF
	"/CN=client" client
	EOF

	cat >| "${REMOTE_DIR}/hello.txt" <<-EOF
	Hello, macaroons!
	EOF

	cat >| "${REMOTE_DIR}/deleteme.txt" <<-EOF
	Delete me if you can!
	EOF

	mkdir "${REMOTE_DIR}/rw"

	openssl rand -base64 -out macaroons.secret 64
}

function teardown_macaroons() {
	rm macaroons.{authdb,gridmap,secret}
	rm -f "${MACAROONS_PLUGINCONFDIR}/http.conf"
	rmdir "${MACAROONS_PLUGINCONFDIR}"
}

# Obtain a macaroon for $1 (path, default "/") with an optional JSON caveats
# array element $2.  Sets MACAROON on success.
function get_macaroon() {
	local path="${1:-/}"
	local caveats="${2:-}"
	local body

	if [[ -n "$caveats" ]]; then
		body='{"validity":"PT1H","caveats":['"$caveats"']}'
	else
		body='{"validity":"PT1H"}'
	fi

	curl -vf \
		--capath "${X509_CERT_DIR}" --cert "${X509_USER_CERT}" --key "${X509_USER_KEY}" \
		-H 'Content-Type: application/macaroon-request' \
		-X POST -d "$body" "${HOST}${path}" -o macaroon.response

	MACAROON=$(macaroon.response | jq -r '.macaroon' < macaroon.response)

	cat macaroon.response
	echo -e "\nmacaroon: \"${MACAROON}\""
	rm macaroon.response

	if [[ -n "$MACAROON" && "$MACAROON" != "null" ]]; then
		return
	fi

	error "Failed to obtain macaroon"
}

function test_macaroons() {
	HOST="https://localhost:15043"
	export XRD_PLUGINCONFDIR="${MACAROONS_PLUGINCONFDIR}"

	local response xrdfs_macaroon

	# Issuance tests

	# xrdfs uses the same local HTTPS macaroon endpoint as gfal-token.
	xrdfs_macaroon=$(xrdfs token "${HOST}/")
	[[ -n "${xrdfs_macaroon}" ]] || error "xrdfs returned an empty macaroon"

	# Tokens and request bodies must only be returned to the caller, not copied
	# into the XrdCl client log.
	if grep -Fq "${xrdfs_macaroon}" "${XRD_LOGFILE}"; then
		error "xrdfs logged the issued macaroon"
	fi
	if grep -Fq '"caveats"' "${XRD_LOGFILE}"; then
		error "xrdfs logged the macaroon request body"
	fi

	# The write defaults and explicit activity list are both accepted by the
	# local issuer; these requests do not perform a remote write.
	response=$(xrdfs token --write --validity 15 "${HOST}/rw")
	[[ -n "${response}" ]] || error "xrdfs returned an empty write macaroon"
	response=$(xrdfs token --validity 5 "${HOST}/" DOWNLOAD LIST)
	[[ -n "${response}" ]] || error "xrdfs returned an empty custom macaroon"
	response=$(xrdfs "${HOST}" token --validity 1 /)
	[[ -n "${response}" ]] || error "legacy xrdfs syntax returned an empty macaroon"

	# Exercise curl handle reuse in one process: the GET following the token POST
	# must remain a GET after the easy handle returns to the pool.
	response=$(printf 'token /\ncat /hello.txt\nexit\n' | xrdfs "${HOST}")
	[[ "${response}" == *"Hello, macaroons!"* ]] || \
		error "a read following token issuance did not return the file"

	# Reject insecure endpoints and unsupported issuer discovery before making a
	# token request.
	assert_failure xrdfs token "http://localhost:15043/"
	assert_failure xrdfs token --issuer https://issuer.example "${HOST}/"
	assert_failure xrdfs token --validity -1 "${HOST}/"
	assert_failure xrdfs token --val 5 "${HOST}/"

	# can obtain a macaroon via HTTPS POST
	get_macaroon /
	[[ -n "$MACAROON" ]] || error "Failed to obtain macaroon for /"

	# macaroon response includes expires_in field
	response=$(curl -sf --capath "${X509_CERT_DIR}" -X POST -d '{ "validity":"PT30S" }' \
		-H 'Content-Type: application/macaroon-request' "${HOST}/")
	echo "${response}" | jq -e '.expires_in == 30'

	# macaroon request without validity is rejected with HTTP error
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -X POST -d '{}' \
		-H 'Content-Type: application/macaroon-request' "${HOST}/"

	# macaroon request with reserved caveat 'path:' is rejected
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -X POST -d '{"validity":"PT1H","caveats":["path:/secret"]}' \
		-H 'Content-Type: application/macaroon-request' "${HOST}/"

	# macaroon request with reserved caveat 'name:' is rejected
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -X POST -d '{"validity":"PT1H","caveats":["name:root"]}' \
		-H 'Content-Type: application/macaroon-request' "${HOST}/"

	# macaroon request with unsupported caveat type is rejected
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -X POST -d '{"validity":"PT1H","caveats":["foobar:baz"]}' \
		-H 'Content-Type: application/macaroon-request' "${HOST}/"

	# Access tests

	# can read a file using a DOWNLOAD macaroon
	unset MACAROON
	get_macaroon / '"activity:DOWNLOAD,LIST"'
	assert curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" "${HOST}/hello.txt"

	# reading without a macaroon is permitted (authdb allows anonymous reads)
	assert curl -S -if --capath "${X509_CERT_DIR}" "${HOST}/hello.txt"

	# macaroon restricted to a path cannot access a file outside that path
	unset MACAROON
	get_macaroon /subdir/
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" "${HOST}/hello.txt"

	# uploading without a macaroon is denied for anonymous users
	echo "upload attempt" >| upload.txt
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -X PUT -T upload.txt "${HOST}/upload.txt"

	# macaroon with UPLOAD activity cannot write beyond authdb permissions
	unset MACAROON
	get_macaroon / '"activity:MANAGE,UPLOAD"'
	echo "exploit content" >| exploit.txt
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" \
		-X PUT -T exploit.txt "${HOST}/exploit.txt"

	# macaroon with UPLOAD activity can write where authdb permits
	unset MACAROON
	get_macaroon /rw '"activity:MANAGE,UPLOAD"'
	echo "exploit content" >| exploit.txt
	assert curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" \
		-X PUT -T exploit.txt "${HOST%/}/rw/exploit.txt"

	# macaroon with DELETE activity cannot delete beyond authdb permissions
	unset MACAROON
	assert get_macaroon / '"activity:DELETE"'
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" \
		-X DELETE "${HOST}/deleteme.txt"

	# macaroon with MANAGE activity cannot create directories beyond authdb permissions
	unset MACAROON
	assert get_macaroon / '"activity:MANAGE"'
	assert_failure curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" \
		-X MKCOL "${HOST}/newdir/"

	# macaroon with MANAGE activity can create directories if authdb permits
	unset MACAROON
	assert get_macaroon /rw '"activity:MANAGE"'
	assert curl -S -if --capath "${X509_CERT_DIR}" -H "Authorization: Bearer ${MACAROON}" \
		-X MKCOL "${HOST%/}/rw/newdir/"

	# all tests passed, exit successfully
	exit 0
}
