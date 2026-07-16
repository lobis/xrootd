/******************************************************************************/
/* Copyright (C) 2026 by European Organization for Nuclear Research (CERN)   */
/*                                                                            */
/* This file is part of the XrdClHttp client plugin for XRootD.               */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/******************************************************************************/

#include "XrdClHttpToken.hh"

#include <XrdOuc/XrdOucJson.hh>

namespace {

const std::vector<std::string> &DefaultActivities(bool write)
{
    static const std::vector<std::string> read{"LIST", "DOWNLOAD"};
    static const std::vector<std::string> write_access{
        "LIST", "DOWNLOAD", "MANAGE", "UPLOAD", "DELETE"
    };
    return write ? write_access : read;
}

bool ValidPort(std::string_view port)
{
    if (port.empty()) return false;
    unsigned int value = 0;
    for (char character : port) {
        if (character < '0' || character > '9') return false;
        value = value * 10 + static_cast<unsigned int>(character - '0');
        if (value > 65535) return false;
    }
    return true;
}

} // namespace

bool
XrdClHttp::NormalizeTokenUrl(std::string_view input, std::string &https_url)
{
    https_url.clear();
    auto separator = input.find("://");
    if (separator == std::string_view::npos) {
        return false;
    }
    auto scheme = input.substr(0, separator);
    std::string lowered_scheme;
    lowered_scheme.reserve(scheme.size());
    for (char value : scheme) {
        lowered_scheme += value >= 'A' && value <= 'Z'
            ? static_cast<char>(value - 'A' + 'a') : value;
    }
    if (lowered_scheme != "https" && lowered_scheme != "davs") return false;

    auto authority_start = separator + 3;
    https_url = "https://";
    https_url.append(input.substr(authority_start));

    auto authority_end = input.find_first_of("/?#", authority_start);
    if (authority_end == std::string_view::npos) authority_end = input.size();
    auto authority = input.substr(authority_start,
                                  authority_end - authority_start);
    auto userinfo_end = authority.rfind('@');
    auto host_port = userinfo_end == std::string_view::npos
        ? authority : authority.substr(userinfo_end + 1);
    if (host_port.empty()) {
        https_url.clear();
        return false;
    }

    std::string_view host;
    if (host_port.front() == '[') {
        auto bracket = host_port.find(']');
        if (bracket == std::string_view::npos) {
            https_url.clear();
            return false;
        }
        host = host_port.substr(1, bracket - 1);
        auto remainder = host_port.substr(bracket + 1);
        if (!remainder.empty() &&
            (remainder.front() != ':' || !ValidPort(remainder.substr(1)))) {
            https_url.clear();
            return false;
        }
    } else {
        auto colon = host_port.rfind(':');
        if (colon != std::string_view::npos &&
            host_port.find(':') != colon) {
            https_url.clear();
            return false;
        }
        host = colon == std::string_view::npos
            ? host_port : host_port.substr(0, colon);
        if (colon != std::string_view::npos &&
            !ValidPort(host_port.substr(colon + 1))) {
            https_url.clear();
            return false;
        }
    }
    if (host.empty()) {
        https_url.clear();
        return false;
    }

    for (char value : input) {
        auto byte = static_cast<unsigned char>(value);
        if (byte <= 0x20 || byte == 0x7f) {
            https_url.clear();
            return false;
        }
    }
    return true;
}

bool
XrdClHttp::ParseTokenRequest(std::string_view input, TokenRequest &request,
                             std::string &error)
{
    error.clear();
    request = TokenRequest{};

    auto parsed = nlohmann::json::parse(input.begin(), input.end(), nullptr,
                                        false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "Token query must be a valid JSON object";
        return false;
    }

    auto path = parsed.find("path");
    if (path == parsed.end() || !path->is_string() ||
        path->get_ref<const std::string &>().empty() ||
        path->get_ref<const std::string &>().front() != '/') {
        error = "Token query requires a non-empty absolute string 'path'";
        return false;
    }
    request.path = path->get<std::string>();

    auto validity = parsed.find("validity");
    if (validity != parsed.end()) {
        if (!validity->is_number_integer()) {
            error = "Token query 'validity' must be a nonnegative integer";
            return false;
        }
        if (validity->is_number_unsigned()) {
            request.validity = validity->get<std::uint64_t>();
        } else {
            auto signed_validity = validity->get<std::int64_t>();
            if (signed_validity < 0) {
                error = "Token query 'validity' must be a nonnegative integer";
                return false;
            }
            request.validity = static_cast<std::uint64_t>(signed_validity);
        }
    }

    auto write = parsed.find("write");
    if (write != parsed.end()) {
        if (!write->is_boolean()) {
            error = "Token query 'write' must be a boolean";
            return false;
        }
        request.write = write->get<bool>();
    }

    auto activities = parsed.find("activities");
    if (activities != parsed.end()) {
        if (!activities->is_array()) {
            error = "Token query 'activities' must be an array of strings";
            return false;
        }
        for (const auto &activity : *activities) {
            if (!activity.is_string() ||
                activity.get_ref<const std::string &>().empty()) {
                error = "Token query 'activities' must contain non-empty strings";
                return false;
            }
            request.activities.emplace_back(activity.get<std::string>());
        }
    }

    if (request.activities.empty()) {
        request.activities = DefaultActivities(request.write);
    }
    return true;
}

std::string
XrdClHttp::BuildMacaroonRequest(
    std::uint64_t validity, const std::vector<std::string> &activities)
{
    std::string caveat = "activity:";
    bool first = true;
    for (const auto &activity : activities) {
        if (!first) caveat += ',';
        caveat += activity;
        first = false;
    }

    // Use the JSON serializer for the user-provided caveat while retaining the
    // exact whitespace and member order used by gfal2.
    return "{\"caveats\": [" + nlohmann::json(caveat).dump() +
        "], \"validity\": \"PT" + std::to_string(validity) + "M\"}";
}

bool
XrdClHttp::ParseMacaroonResponse(std::string_view input, std::string &token,
                                 std::string &error)
{
    token.clear();
    error.clear();
    if (input.size() >= kMaxTokenResponseSize) {
        error = "Token response exceeds maximum size";
        return false;
    }
    if (input.empty()) {
        error = "Token response contained no data";
        return false;
    }

    auto parsed = nlohmann::json::parse(input.begin(), input.end(), nullptr,
                                        false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "Token response was not valid JSON";
        return false;
    }

    auto macaroon = parsed.find("macaroon");
    if (macaroon == parsed.end() || !macaroon->is_string() ||
        macaroon->get_ref<const std::string &>().empty()) {
        error = "Token response did not include a non-empty string 'macaroon'";
        return false;
    }
    token = macaroon->get<std::string>();
    return true;
}
