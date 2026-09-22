/******************************************************************************/
/* Copyright (C) 2026, European Organization for Nuclear Research (CERN)      */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
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

#include "XrdClHttp/XrdClHttpFactory.hh"
#include "XrdClHttp/XrdClHttpOps.hh"
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClPropertyList.hh>
#include "Server.hh"
#include "Utils.hh"

#include <XrdCl/XrdClAnyObject.hh>
#include <XrdCl/XrdClUtils.hh>
#include <XrdCl/XrdClXRootDResponses.hh>

#include <gtest/gtest.h>

#include <cerrno>
#include <vector>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unistd.h>

namespace {

struct MockHttpExchange {
    std::string response;
    std::string request;
    bool read_failed{false};
    bool write_failed{false};
};

class FixedHttpResponseHandler final : public XrdClTests::ClientHandler {
public:
    explicit FixedHttpResponseHandler(MockHttpExchange *exchange)
        : m_exchange(exchange)
    {}

    void HandleConnection(int socket) override
    {
        XrdCl::ScopedDescriptor descriptor(socket);
        char buffer[4096];

        while (m_exchange->request.find("\r\n\r\n") == std::string::npos) {
            ssize_t bytes_read;
            do {
                bytes_read = read(socket, buffer, sizeof(buffer));
            } while (bytes_read < 0 && errno == EINTR);

            if (bytes_read <= 0 ||
                m_exchange->request.size() + bytes_read > 64 * 1024) {
                m_exchange->read_failed = true;
                return;
            }
            m_exchange->request.append(buffer, bytes_read);
        }

        if (XrdClTests::Utils::Write(socket, m_exchange->response.data(),
                                    m_exchange->response.size()) !=
            static_cast<ssize_t>(m_exchange->response.size())) {
            m_exchange->write_failed = true;
        }
    }

private:
    MockHttpExchange *m_exchange;
};

class FixedHttpResponseFactory final : public XrdClTests::ClientHandlerFactory {
public:
    explicit FixedHttpResponseFactory(MockHttpExchange *exchange)
        : m_exchange(exchange)
    {}

    XrdClTests::ClientHandler *CreateHandler() override
    {
        return new FixedHttpResponseHandler(m_exchange);
    }

private:
    MockHttpExchange *m_exchange;
};

class SyncResponseHandler final : public XrdCl::ResponseHandler {
public:
    void HandleResponse(XrdCl::XRootDStatus *status,
                        XrdCl::AnyObject *response) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.reset(status);
        m_response.reset(response);
        m_cv.notify_one();
    }

    void Wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_status != nullptr; });
    }

    std::tuple<std::unique_ptr<XrdCl::XRootDStatus>,
               std::unique_ptr<XrdCl::AnyObject>> Status()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return {std::move(m_status), std::move(m_response)};
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unique_ptr<XrdCl::XRootDStatus> m_status;
    std::unique_ptr<XrdCl::AnyObject> m_response;
};

class ProgressCallback final : public XrdClHttp::CurlCopyOp::CurlProgressCallback {
public:
    explicit ProgressCallback(std::vector<off_t> &marks) : m_marks(marks) {}
    void Progress(off_t bytes) override { m_marks.push_back(bytes); }
private:
    std::vector<off_t> &m_marks;
};

class CurlCopyResponseTest : public testing::TestWithParam<std::tuple<std::string, bool, int>> {};

TEST_P(CurlCopyResponseTest, RequiresSuccessfulTerminalMarker)
{
    const auto &body = std::get<0>(GetParam());
    MockHttpExchange exchange;
    std::string wire_body = body;
    std::string framing = "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (body.find("Perf Marker") == 0) {
        framing = "Transfer-Encoding: chunked\r\n";
        wire_body.clear();
        for (char byte : body) wire_body += std::string("1\r\n") + byte + "\r\n";
        wire_body += "0\r\n\r\n";
    }
    exchange.response =
        "HTTP/1.1 " + std::to_string(std::get<2>(GetParam())) + " Test\r\n"
        "Content-Type: text/plain\r\n"
        + framing + "Connection: close\r\n\r\n" + wire_body;
    XrdClTests::Server server(XrdClTests::Server::Inet4);
    ASSERT_TRUE(server.Setup(0, 1, new FixedHttpResponseFactory(&exchange)));
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(server.GetPort());
    auto factory = std::make_unique<XrdClHttp::Factory>();
    std::unique_ptr<XrdCl::FileSystemPlugIn> filesystem(factory->CreateFileSystem(endpoint));
    ASSERT_NE(filesystem, nullptr);
    SyncResponseHandler handler;
    std::vector<off_t> progress;
    auto operation = std::make_unique<XrdClHttp::CurlCopyOp>(
        &handler, endpoint + "/source", XrdClHttp::CurlCopyOp::Headers{},
        endpoint + "/target", XrdClHttp::CurlCopyOp::Headers{},
        timespec{5, 0}, XrdCl::DefaultEnv::GetLog(), nullptr);
    operation->SetCallback(std::make_unique<ProgressCallback>(progress));
    ASSERT_TRUE(server.Start());
    factory->Produce(std::move(operation));
    handler.Wait();
    auto [status, response] = handler.Status();
    ASSERT_TRUE(server.Stop());
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->IsOK(), std::get<1>(GetParam())) << status->ToString();
    if (!std::get<1>(GetParam()) && std::get<2>(GetParam()) == 201) {
        EXPECT_EQ(status->code, XrdCl::errErrorResponse);
        EXPECT_EQ(status->errNo, body.find("aborted:") == 0 ? kXR_Cancelled : kXR_IOError);
    }
    if (std::get<2>(GetParam()) == 403) {
        EXPECT_EQ(status->errNo, kXR_NotAuthorized);
    }
    if (std::get<2>(GetParam()) == 500) {
        EXPECT_EQ(status->errNo, kXR_IOError);
    }
    if (body.find("Transferred: 42") != std::string::npos) {
        EXPECT_EQ(progress, std::vector<off_t>{42});
    } else {
        EXPECT_TRUE(progress.empty());
    }
    EXPECT_FALSE(exchange.read_failed);
    EXPECT_FALSE(exchange.write_failed);
    EXPECT_EQ(exchange.request.substr(0, exchange.request.find("\r\n")), "COPY /target HTTP/1.1");
    EXPECT_NE(exchange.request.find("Source: " + endpoint + "/source\r\n"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(ControlBody, CurlCopyResponseTest, testing::Values(
    std::make_tuple("success: Created\n", true, 201),
    std::make_tuple("success: Created", true, 201),
    std::make_tuple("Perf Marker\r\n\tStripe Bytes Transferred: 42\r\nEnd\r\nsuccess: Created\r\n", true, 201),
    std::make_tuple("failure: remote write failed\n", false, 201),
    std::make_tuple("failed: remote write failed", false, 201),
    std::make_tuple("aborted: timeout\r\n", false, 201),
    std::make_tuple("failure:\n", false, 201),
    std::make_tuple("", false, 201),
    std::make_tuple("Perf Marker\nEnd\n", false, 201),
    std::make_tuple("success: Created\nfailure: checksum mismatch\n", false, 201),
    std::make_tuple("failure: checksum mismatch\nsuccess: Created\n", false, 201),
    std::make_tuple("failure: ignore this HTTP error body\n", false, 403),
    std::make_tuple("Internal Server Error", false, 500)
));

TEST(CopyResponseParser, ParsesEverySplitAndSingleByteDelivery)
{
    const std::string body = "Perf Marker\r\n\tStripe Bytes Transferred: 42\r\nEnd\r\nsuccess: Created";
    for (size_t split = 0; split <= body.size(); ++split) {
        SCOPED_TRACE(split);
        XrdClHttp::CopyResponse response;
        std::vector<off_t> marks;
        response.SetCallback([&marks](off_t bytes) { marks.push_back(bytes); });
        EXPECT_TRUE(response.Feed(std::string_view(body).substr(0, split)));
        EXPECT_TRUE(response.Feed(std::string_view(body).substr(split)));
        EXPECT_TRUE(response.Finish().IsOK());
        EXPECT_EQ(marks, std::vector<off_t>{42});
    }
    XrdClHttp::CopyResponse response;
    for (char byte : body) EXPECT_TRUE(response.Feed(std::string_view(&byte, 1)));
    EXPECT_TRUE(response.Finish().IsOK());
}

TEST(CopyResponseParser, PreservesFailureAcrossFragments)
{
    const std::string body = "failure: remote write failed";
    for (size_t split = 0; split <= body.size(); ++split) {
        XrdClHttp::CopyResponse response;
        EXPECT_TRUE(response.Feed(std::string_view(body).substr(0, split)));
        EXPECT_TRUE(response.Feed(std::string_view(body).substr(split)));
        const auto status = response.Finish();
        EXPECT_FALSE(status.IsOK());
        EXPECT_EQ(status.GetErrorMessage(), "HTTP COPY failure: remote write failed");
    }
}

TEST(CopyResponseParser, BoundsUnterminatedLinesAndRetainsFailure)
{
    XrdClHttp::CopyResponse response;
    EXPECT_TRUE(response.Feed(std::string(64 * 1024, 'x')));
    EXPECT_FALSE(response.Feed("x"));
    EXPECT_FALSE(response.Feed("\nsuccess: Created\n"));
    EXPECT_FALSE(response.Finish().IsOK());
}

TEST(CopyResponseParser, RejectsMalformedByteCountsAndDuplicateEnds)
{
    for (const auto *value : {"-1", "42junk", "99999999999999999999999999999", ""}) {
        XrdClHttp::CopyResponse response;
        std::vector<off_t> marks;
        response.SetCallback([&marks](off_t bytes) { marks.push_back(bytes); });
        EXPECT_TRUE(response.Feed(std::string("Perf Marker\nStripe Bytes Transferred: ") + value +
            "\nEnd\nEnd\nsuccess: Created\n"));
        EXPECT_TRUE(response.Finish().IsOK());
        EXPECT_TRUE(marks.empty());
    }
    XrdClHttp::CopyResponse response;
    std::vector<off_t> marks;
    response.SetCallback([&marks](off_t bytes) { marks.push_back(bytes); });
    EXPECT_TRUE(response.Feed("Perf Marker\nStripe Bytes Transferred: 0\nEnd\nEnd\nsuccess: Created\n"));
    EXPECT_TRUE(response.Finish().IsOK());
    EXPECT_EQ(marks, std::vector<off_t>{0});
}

} // namespace


TEST(HttpCopySafety, RejectSelfCopyBeforeNetworkOrOverwrite)
{
    XrdClHttp::Factory factory;
    const std::vector<std::pair<std::string, std::string>> urls = {
        {"http://storage.invalid/file", "http://storage.invalid/file"},
        {"dav://storage.invalid/file", "http://storage.invalid:80/file"},
        {"https://STORAGE.invalid/file?authz=source", "davs://storage.invalid/file?authz=target"},
        {"http://storage.invalid/file?xrdcl.http.noauth=true", "http://storage.invalid/file"}
    };
    for (const auto &url : urls) {
        for (const auto *mode : {"pull", "push", "auto"}) {
            XrdCl::PropertyList properties, results;
            properties.Set("source", url.first);
            properties.Set("target", url.second);
            properties.Set("force", true);
            properties.Set("thirdPartyMode", std::string(mode));
            auto status = factory.ThirdPartyCopy(0, properties, results, nullptr);
            EXPECT_EQ(status.code, XrdCl::errInvalidArgs);
            EXPECT_EQ(status.errNo, EINVAL);
        }
    }
}
