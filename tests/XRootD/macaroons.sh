#!/usr/bin/env bash

X509_CERT_DIR="${BINARY_DIR}/tests/tls"
X509_USER_KEY="${X509_CERT_DIR}/client.key"
X509_USER_CERT="${X509_CERT_DIR}/client.crt"
XRD_HTTPCLIENTKEYFILE="${X509_USER_KEY}"
XRD_HTTPCLIENTCERTFILE="${X509_USER_CERT}"
MACAROONS_PLUGINCONFDIR="${PWD}/macaroons/client.plugins.d"
TOKEN_ISSUER_PORT=15044
TOKEN_ISSUER_HOST="https://localhost:${TOKEN_ISSUER_PORT}"
TOKEN_ISSUER_DAVS_HOST="davs://localhost:${TOKEN_ISSUER_PORT}"
TOKEN_ISSUER_TRACE="${PWD}/macaroons/token-issuer-trace.jsonl"
TOKEN_ISSUER_LOG="${PWD}/macaroons/token-issuer.log"

export XrdSecPROTOCOL X509_CERT_DIR X509_USER_KEY X509_USER_CERT
export XRD_HTTPCLIENTKEYFILE XRD_HTTPCLIENTCERTFILE

function setup_macaroons() {
	require_commands curl jq openssl python3

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

	: > "${TOKEN_ISSUER_TRACE}"
	python3 "${SOURCE_DIR}/token_issuer_mock.py" \
		--bind 127.0.0.1 --port "${TOKEN_ISSUER_PORT}" \
		--cert "${X509_CERT_DIR}/host.pem" \
		--key "${X509_CERT_DIR}/host.key" \
		--trace-file "${TOKEN_ISSUER_TRACE}" \
		> "${TOKEN_ISSUER_LOG}" 2>&1 &
	echo "$!" > "${PWD}/macaroons/token-issuer.pid"

	local attempt
	for ((attempt = 0; attempt < 50; ++attempt)); do
		if curl -sf --capath "${X509_CERT_DIR}" \
			"${TOKEN_ISSUER_HOST}/healthz" >/dev/null; then
			return
		fi
		if ! kill -0 "$(cat "${PWD}/macaroons/token-issuer.pid")" 2>/dev/null; then
			break
		fi
		sleep 0.1
	done

	cat "${TOKEN_ISSUER_LOG}" >&2
	error "token issuer mock did not become ready"
}

function teardown_macaroons() {
	local issuer_pid_file="${PWD}/macaroons/token-issuer.pid"
	if [[ -f "${issuer_pid_file}" ]]; then
		kill "$(cat "${issuer_pid_file}")" 2>/dev/null || true
		wait "$(cat "${issuer_pid_file}")" 2>/dev/null || true
		rm -f "${issuer_pid_file}"
	fi
	rm macaroons.{authdb,gridmap,secret}
	rm -f "${MACAROONS_PLUGINCONFDIR}/http.conf"
	rmdir "${MACAROONS_PLUGINCONFDIR}"
}

function reset_token_issuer_trace() {
	: > "${TOKEN_ISSUER_TRACE}"
}

function assert_token_issuer_trace() {
	local expected="$1"
	local actual
	actual=$(jq -cs \
		'map({method, url, content_type, accept, body})' \
		"${TOKEN_ISSUER_TRACE}") || \
		error "failed to parse token issuer trace"

	if [[ "${actual}" != "${expected}" ]]; then
		echo "expected token issuer trace: ${expected}" >&2
		echo "actual token issuer trace:   ${actual}" >&2
		error "token issuer workflow did not make the expected requests"
	fi
}

function test_token_issuer_workflows() {
	local response expected
	export XRD_TOKEN_CLIENT_SECRET=secret
	reset_token_issuer_trace
	response=$(xrdtoken oauth --issuer "${TOKEN_ISSUER_HOST}/oauth-success" \
		--client-id id --scope 'storage.read:/storage/object')
	assert_eq 'oauth-token' "${response}" "OAuth issuer returned an unexpected token"
	expected='['
	expected+='{"method":"GET","url":"https://localhost:15044/.well-known/oauth-authorization-server/oauth-success","content_type":"","accept":"*/*","body":""},'
	expected+='{"method":"POST","url":"https://localhost:15044/token/oauth-success","content_type":"application/x-www-form-urlencoded","accept":"application/json","body":"grant_type=client_credentials&scope=storage.read%3A%2Fstorage%2Fobject&client_id=id&client_secret=secret"}'
	expected+=']'
	assert_token_issuer_trace "${expected}"

	reset_token_issuer_trace
	response=$(xrdtoken oauth-macaroon --issuer \
		"${TOKEN_ISSUER_DAVS_HOST}/openid-success" --client-id id \
		--validity 2 "${TOKEN_ISSUER_HOST}/storage/object")
	assert_eq 'openid-token' "${response}" "OIDC discovery did not return the token"
	expected='['
	expected+='{"method":"GET","url":"https://localhost:15044/.well-known/oauth-authorization-server/openid-success","content_type":"","accept":"*/*","body":""},'
	expected+='{"method":"GET","url":"https://localhost:15044/openid-success/.well-known/openid-configuration","content_type":"","accept":"*/*","body":""},'
	expected+='{"method":"POST","url":"https://localhost:15044/token/openid-success","content_type":"application/x-www-form-urlencoded","accept":"application/json","body":"grant_type=client_credentials&scope=LIST%3A%2Fstorage%2Fobject%20DOWNLOAD%3A%2Fstorage%2Fobject&client_id=id&client_secret=secret&expire_in=120"}'
	expected+=']'
	assert_token_issuer_trace "${expected}"

	reset_token_issuer_trace
	assert_failure xrdtoken oauth --issuer "${TOKEN_ISSUER_HOST}/oauth-failure" \
		--client-id id --scope 'storage.read:/storage/object'
	expected='['
	expected+='{"method":"GET","url":"https://localhost:15044/.well-known/oauth-authorization-server/oauth-failure","content_type":"","accept":"*/*","body":""},'
	expected+='{"method":"POST","url":"https://localhost:15044/token/oauth-failure","content_type":"application/x-www-form-urlencoded","accept":"application/json","body":"grant_type=client_credentials&scope=storage.read%3A%2Fstorage%2Fobject&client_id=id&client_secret=secret"}'
	expected+=']'
	assert_token_issuer_trace "${expected}"

	reset_token_issuer_trace
	response=$(xrdtoken id --issuer "${TOKEN_ISSUER_HOST}/device-success" \
		--client-id id --scope 'openid profile')
	assert_eq 'device-id-token' "${response}" \
		"Device authorization did not return an ID token"
	response=$(xrdtoken oauth-device \
		--issuer "${TOKEN_ISSUER_HOST}/device-success" \
		--client-id id --scope 'storage.read:/storage/object')
	assert_eq 'device-access-token' "${response}" \
		"Device authorization did not return an access token"
	reset_token_issuer_trace
	response=$(xrdtoken oauth-device \
		--issuer "${TOKEN_ISSUER_HOST}/missing-discovery" \
		--device-endpoint "${TOKEN_ISSUER_HOST}/device/device-success" \
		--token-endpoint "${TOKEN_ISSUER_HOST}/token/device-success" \
		--client-id id --scope 'storage.read:/storage/object')
	assert_eq 'device-access-token' "${response}" \
		"Explicit device endpoints did not return an access token"
	if jq -es 'any(.[]; .method == "GET")' \
		"${TOKEN_ISSUER_TRACE}" >/dev/null; then
		error "explicit device endpoints unexpectedly performed discovery"
	fi
	response=$(xrdtoken id --issuer "${TOKEN_ISSUER_HOST}/missing-discovery" \
		--device-endpoint "${TOKEN_ISSUER_HOST}/device/device-google" \
		--token-endpoint "${TOKEN_ISSUER_HOST}/token/device-google" \
		--client-id id --scope openid)
	assert_eq 'google-device-id-token' "${response}" \
		"Google-style device response did not return an ID token"

	reset_token_issuer_trace
	assert_failure xrdtoken oauth --timeout 4 \
		--issuer "${TOKEN_ISSUER_HOST}/slow-deadline" \
		--client-id id --scope read
	expected='['
	expected+='{"method":"GET","url":"https://localhost:15044/.well-known/oauth-authorization-server/slow-deadline","content_type":"","accept":"*/*","body":""},'
	expected+='{"method":"GET","url":"https://localhost:15044/slow-deadline/.well-known/openid-configuration","content_type":"","accept":"*/*","body":""}'
	expected+=']'
	assert_token_issuer_trace "${expected}"
	unset XRD_TOKEN_CLIENT_SECRET
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

	local response macaroon

	macaroon=$(xrdtoken macaroon "${HOST}/")
	[[ -n "${macaroon}" ]] || error "xrdtoken returned an empty macaroon"
	if grep -Fq "${macaroon}" "${XRD_LOGFILE}"; then
		error "xrdtoken logged the issued macaroon"
	fi
	if grep -Fq '"caveats"' "${XRD_LOGFILE}"; then
		error "xrdtoken logged the macaroon request body"
	fi

	response=$(xrdtoken macaroon --write --validity 15 "${HOST}/rw")
	[[ -n "${response}" ]] || error "xrdtoken returned an empty write macaroon"
	response=$(xrdtoken macaroon --validity 5 --activity DOWNLOAD \
		--activity LIST "${HOST}/")
	[[ -n "${response}" ]] || error "xrdtoken returned an empty custom macaroon"

	test_token_issuer_workflows
	response=$(xrdtoken oauth-macaroon --issuer "${HOST}" \
		--validity 5 "${HOST}/")
	[[ -n "${response}" ]] || error "mTLS issuer returned an empty macaroon"

	# Keep the OAuth endpoint compatible with both the standard singular
	# spelling and the plural spelling emitted by gfal-token.
	response=$(curl -sf \
		--capath "${X509_CERT_DIR}" --cert "${X509_USER_CERT}" --key "${X509_USER_KEY}" \
		-H 'Content-Type: application/x-www-form-urlencoded' \
		-X POST -d 'grant_type=client_credentials&expire_in=300&scope=DOWNLOAD%3A%2F' \
		"${HOST}/.oauth2/token")
	response=$(echo "${response}" | jq -er \
		'.access_token | select(type == "string" and length > 0)')
	[[ -n "${response}" ]] || error "singular OAuth scope returned an empty macaroon"

	response=$(curl -sf \
		--capath "${X509_CERT_DIR}" --cert "${X509_USER_CERT}" --key "${X509_USER_KEY}" \
		-H 'Content-Type: application/x-www-form-urlencoded' \
		-X POST -d 'grant_type=client_credentials&expire_in=300&scopes=LIST%3A%2F%20DOWNLOAD%3A%2F' \
		"${HOST}/.oauth2/token")
	response=$(echo "${response}" | jq -er \
		'.access_token | select(type == "string" and length > 0)')
	[[ -n "${response}" ]] || error "plural OAuth scopes returned an empty macaroon"

	assert_failure xrdtoken macaroon "http://localhost:15043/"
	assert_failure xrdtoken macaroon --validity -1 "${HOST}/"
	assert_failure xrdtoken macaroon --unknown "${HOST}/"
	assert_failure xrdtoken oauth --issuer http://localhost:15043 \
		--client-id id --scope read

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
