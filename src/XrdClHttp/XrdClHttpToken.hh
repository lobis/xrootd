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

#ifndef XRDCLHTTP_TOKEN_HH
#define XRDCLHTTP_TOKEN_HH

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace XrdClHttp {

constexpr std::size_t kMaxTokenResponseSize = 1024 * 1024;

struct TokenRequest {
    std::string path;
    std::uint64_t validity{60};
    bool write{false};
    std::vector<std::string> activities;
};

// Accept HTTPS and DAVS URLs only, normalizing DAVS to HTTPS.
bool NormalizeTokenUrl(std::string_view input, std::string &https_url);

// Parse the QueryCode::Visa argument and apply the default activities when the
// caller did not provide any.
bool ParseTokenRequest(std::string_view input, TokenRequest &request,
                       std::string &error);

// Construct the byte-for-byte request format emitted by gfal2 for a direct
// storage-element macaroon request.
std::string BuildMacaroonRequest(
    std::uint64_t validity, const std::vector<std::string> &activities);

// Extract a macaroon without exposing the response contents in diagnostics.
bool ParseMacaroonResponse(std::string_view input, std::string &token,
                           std::string &error);

} // namespace XrdClHttp

#endif // XRDCLHTTP_TOKEN_HH
