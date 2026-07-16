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

#include "XrdClHttp/XrdClHttpToken.hh"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::string MacaroonResponseOfSize(std::size_t size)
{
    const std::string prefix = "{\"macaroon\":\"";
    const std::string suffix = "\"}";
    if (size <= prefix.size() + suffix.size()) return {};
    return prefix +
        std::string(size - prefix.size() - suffix.size(), 'x') + suffix;
}

} // namespace

TEST(HttpToken, AcceptsHttpsUrl)
{
    std::string normalized;
    EXPECT_TRUE(XrdClHttp::NormalizeTokenUrl(
        "https://storage.example/eos/file?opaque=value", normalized));
    EXPECT_EQ(normalized,
              "https://storage.example/eos/file?opaque=value");
}

TEST(HttpToken, NormalizesDavsUrlToHttps)
{
    std::string normalized;
    EXPECT_TRUE(XrdClHttp::NormalizeTokenUrl(
        "davs://storage.example:8443/eos/file", normalized));
    EXPECT_EQ(normalized, "https://storage.example:8443/eos/file");

    EXPECT_TRUE(XrdClHttp::NormalizeTokenUrl(
        "HTTPS://storage.example/eos/file", normalized));
    EXPECT_EQ(normalized, "https://storage.example/eos/file");
}

TEST(HttpToken, RejectsInsecureOrMalformedUrl)
{
    const std::vector<std::string> invalid = {
        "http://storage.example/eos/file",
        "dav://storage.example/eos/file",
        "root://storage.example//eos/file",
        "file:///tmp/file",
        "https:///missing-host",
        "https://:8443/missing-host",
        "https://storage.example:/empty-port",
        "https://storage.example:abc/non-numeric-port",
        "https://storage.example:65536/out-of-range-port",
        "https://[::1]suffix/malformed-bracket-suffix",
        "https://[::1]:abc/non-numeric-ipv6-port",
        "https://::1/unbracketed-ipv6",
        "https://storage.example/path with space",
        "https://storage.example/path\nhttp://redirect.example/"
    };

    for (const auto &input : invalid) {
        std::string normalized = "stale-url";
        EXPECT_FALSE(XrdClHttp::NormalizeTokenUrl(input, normalized))
            << input;
        EXPECT_TRUE(normalized.empty()) << input;
    }
}

TEST(HttpToken, ParsesReadDefaults)
{
    XrdClHttp::TokenRequest request;
    std::string error;

    ASSERT_TRUE(XrdClHttp::ParseTokenRequest(
        R"({"path":"/store/file","validity":60,"write":false,"activities":[]})",
        request, error)) << error;
    EXPECT_EQ(request.path, "/store/file");
    EXPECT_EQ(request.validity, 60U);
    EXPECT_FALSE(request.write);
    EXPECT_EQ(request.activities,
              (std::vector<std::string>{"LIST", "DOWNLOAD"}));
}

TEST(HttpToken, ParsesWriteDefaults)
{
    XrdClHttp::TokenRequest request;
    std::string error;

    ASSERT_TRUE(XrdClHttp::ParseTokenRequest(
        R"({"path":"/rw","validity":15,"write":true,"activities":[]})",
        request, error)) << error;
    EXPECT_EQ(request.path, "/rw");
    EXPECT_EQ(request.validity, 15U);
    EXPECT_TRUE(request.write);
    EXPECT_EQ(request.activities,
              (std::vector<std::string>{
                  "LIST", "DOWNLOAD", "MANAGE", "UPLOAD", "DELETE"}));
}

TEST(HttpToken, PreservesCustomActivities)
{
    XrdClHttp::TokenRequest request;
    std::string error;

    ASSERT_TRUE(XrdClHttp::ParseTokenRequest(
        R"({"path":"/custom?opaque=value","validity":5,"write":true,"activities":["READ_METADATA","LIST"]})",
        request, error)) << error;
    EXPECT_EQ(request.path, "/custom?opaque=value");
    EXPECT_EQ(request.activities,
              (std::vector<std::string>{"READ_METADATA", "LIST"}));
}

TEST(HttpToken, RejectsInvalidQueryFields)
{
    const std::vector<std::string> invalid = {
        R"({})",
        R"({"path":""})",
        R"({"path":"relative/file"})",
        R"({"path":42})",
        R"({"path":"/file","validity":-1})",
        R"({"path":"/file","validity":"60"})",
        R"({"path":"/file","write":"yes"})",
        R"({"path":"/file","activities":"LIST"})",
        R"({"path":"/file","activities":[""]})",
        R"({"path":"/file","activities":["LIST",7]})"
    };

    for (const auto &input : invalid) {
        XrdClHttp::TokenRequest request;
        std::string error;
        EXPECT_FALSE(XrdClHttp::ParseTokenRequest(input, request, error))
            << input;
        EXPECT_FALSE(error.empty()) << input;
    }
}

TEST(HttpToken, BuildsGfalCompatibleReadRequest)
{
    EXPECT_EQ(XrdClHttp::BuildMacaroonRequest(
                  60, {"LIST", "DOWNLOAD"}),
              "{\"caveats\": [\"activity:LIST,DOWNLOAD\"], "
              "\"validity\": \"PT60M\"}");
}

TEST(HttpToken, BuildsGfalCompatibleWriteRequest)
{
    EXPECT_EQ(XrdClHttp::BuildMacaroonRequest(
                  10, {"LIST", "DOWNLOAD", "MANAGE", "UPLOAD", "DELETE"}),
              "{\"caveats\": "
              "[\"activity:LIST,DOWNLOAD,MANAGE,UPLOAD,DELETE\"], "
              "\"validity\": \"PT10M\"}");
}

TEST(HttpToken, BuildsGfalCompatibleCustomRequest)
{
    EXPECT_EQ(XrdClHttp::BuildMacaroonRequest(
                  5, {"READ_METADATA", "LIST"}),
              "{\"caveats\": [\"activity:READ_METADATA,LIST\"], "
              "\"validity\": \"PT5M\"}");
}

TEST(HttpToken, EscapesCustomActivitiesInJson)
{
    EXPECT_EQ(XrdClHttp::BuildMacaroonRequest(1, {"LIST\"INJECT"}),
              "{\"caveats\": [\"activity:LIST\\\"INJECT\"], "
              "\"validity\": \"PT1M\"}");
}

TEST(HttpToken, ParsesMacaroonResponse)
{
    std::string token;
    std::string error;
    ASSERT_TRUE(XrdClHttp::ParseMacaroonResponse(
        R"({"expires_in":3600,"macaroon":"issued-token"})", token,
        error)) << error;
    EXPECT_EQ(token, "issued-token");
    EXPECT_TRUE(error.empty());
}

TEST(HttpToken, RejectsMissingOrEmptyMacaroon)
{
    const std::vector<std::string> invalid = {
        R"({})",
        R"({"macaroon":""})",
        R"({"macaroon":null})",
        R"({"macaroon":42})",
        ""
    };

    for (const auto &input : invalid) {
        std::string token = "stale-token";
        std::string error;
        EXPECT_FALSE(XrdClHttp::ParseMacaroonResponse(input, token, error))
            << input;
        EXPECT_TRUE(token.empty()) << input;
        EXPECT_FALSE(error.empty()) << input;
    }
}

TEST(HttpToken, AcceptsResponseJustBelowMaximumSize)
{
    const std::string response = MacaroonResponseOfSize(
        XrdClHttp::kMaxTokenResponseSize - 1);
    ASSERT_EQ(response.size(), XrdClHttp::kMaxTokenResponseSize - 1);

    std::string token;
    std::string error;
    ASSERT_TRUE(XrdClHttp::ParseMacaroonResponse(response, token, error))
        << error;
    EXPECT_FALSE(token.empty());
}

TEST(HttpToken, RejectsResponseAtMaximumSize)
{
    const std::string response = MacaroonResponseOfSize(
        XrdClHttp::kMaxTokenResponseSize);
    ASSERT_EQ(response.size(), XrdClHttp::kMaxTokenResponseSize);

    std::string token;
    std::string error;
    EXPECT_FALSE(XrdClHttp::ParseMacaroonResponse(response, token, error));
    EXPECT_TRUE(token.empty());
    EXPECT_NE(error.find("maximum size"), std::string::npos);
}

TEST(HttpToken, DoesNotIncludeResponseBodyInErrors)
{
    const std::string marker = "sensitive-response-body";
    std::string token;
    std::string error;

    EXPECT_FALSE(XrdClHttp::ParseMacaroonResponse(marker, token, error));
    EXPECT_EQ(error.find(marker), std::string::npos);

    EXPECT_FALSE(XrdClHttp::ParseMacaroonResponse(
        "{\"secret\":\"" + marker + "\"}", token, error));
    EXPECT_EQ(error.find(marker), std::string::npos);
}
