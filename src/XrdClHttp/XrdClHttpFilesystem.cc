/******************************************************************************/
/* Copyright (C) 2025, Pelican Project, Morgridge Institute for Research      */
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
/*                                                                            */
/* The copyright holder's institutional names and contributor's names may not */
/* be used to endorse or promote products derived from this software without  */
/* specific prior written permission of the institution or contributor.       */
/******************************************************************************/

#include "XrdClHttpFactory.hh"
#include "XrdClHttpFilesystem.hh"
#include "XrdClHttpOps.hh"
#include "XrdClHttpResponses.hh"
#include "XrdClHttpToken.hh"

#include "XrdCl/XrdClAnyObject.hh"
#include <XrdOuc/XrdOucJson.hh>

#include <cerrno>
#include <chrono>
#include <exception>
#include <memory>
#include <new>

using namespace XrdClHttp;

namespace {

std::chrono::steady_clock::time_point
TokenWorkflowExpiry(struct timespec timeout)
{
    // Match CurlOperation's zero-timeout default, but calculate it only once
    // so discovery and all fallback requests share one operation deadline.
    auto now = std::chrono::steady_clock::now();
    if (timeout.tv_sec == 0 && timeout.tv_nsec == 0) {
        return now + std::chrono::seconds(30);
    }
    return now + std::chrono::seconds(timeout.tv_sec) +
        std::chrono::nanoseconds(timeout.tv_nsec);
}

struct TokenWorkflowQueueError {};
struct TokenWorkflowExpired {};

bool NormalizeIssuerEndpoint(std::string_view input, std::string &url)
{
    if (!NormalizeTokenUrl(input, url)) return false;
    const auto authority_start = url.find("://") + 3;
    const auto authority_end = url.find_first_of("/?#", authority_start);
    if (url.substr(authority_start, authority_end - authority_start).find('@') !=
        std::string::npos) {
        url.clear();
        return false;
    }
    return true;
}

// Coordinate the issuer workflow without blocking a curl worker.  Each step is
// a regular CurlTokenOp queued through the same worker pool as any other HTTP
// filesystem request; this handler only interprets the step result and queues
// its successor.
class TokenIssuerWorkflow final
    : public XrdCl::ResponseHandler,
      public std::enable_shared_from_this<TokenIssuerWorkflow> {
public:
    TokenIssuerWorkflow(std::shared_ptr<HandlerQueue> queue,
                        XrdCl::ResponseHandler *handler,
                        TokenRequest request,
                        struct timespec timeout, XrdCl::Log *logger,
                        CreateConnCalloutType callout)
        : m_queue(std::move(queue)), m_handler(handler),
          m_request(std::move(request)),
          m_expiry(TokenWorkflowExpiry(timeout)), m_logger(logger),
          m_callout(callout)
    {}

    // Queue the first stage.  A false return means nothing was queued and the
    // caller should return an immediate error without invoking the handler.
    bool Start()
    {
        try {
            if (m_request.type == TokenRequest::Type::DeviceStart &&
                !m_request.device_endpoint.empty()) {
                std::string endpoint;
                if (!NormalizeIssuerEndpoint(m_request.device_endpoint, endpoint) ||
                    !NormalizeIssuerEndpoint(m_request.token_endpoint,
                                       m_token_endpoint)) return false;
                QueueDeviceRequest(endpoint);
            } else {
                BeginOAuthDiscovery();
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void HandleResponse(XrdCl::XRootDStatus *status_raw,
                        XrdCl::AnyObject *response_raw) override
    {
        std::unique_ptr<XrdCl::XRootDStatus> status(status_raw);
        std::unique_ptr<XrdCl::AnyObject> response(response_raw);

        try {
            bool success = status && status->IsOK();
            std::string value;
            if (success) {
                XrdCl::Buffer *buffer = nullptr;
                if (response) response->Get(buffer);
                if (buffer) {
                    value = buffer->ToString();
                } else {
                    success = false;
                }
            }

            if (std::chrono::steady_clock::now() > m_expiry) {
                FinishError(XrdCl::errOperationExpired, 0,
                            "Token issuer workflow expired");
                return;
            }

            switch (m_stage) {
            case Stage::OAuthDiscovery:
                if (success && m_request.type == TokenRequest::Type::DeviceStart) {
                    if (BeginDeviceRequest(value)) return;
                } else if (success) {
                    BeginOAuthRequest(value);
                    return;
                }
                BeginOpenIdDiscovery();
                return;
            case Stage::OpenIdDiscovery:
                if (success && m_request.type == TokenRequest::Type::DeviceStart) {
                    if (BeginDeviceRequest(value)) return;
                } else if (success) {
                    BeginOAuthRequest(value);
                    return;
                }
                FinishError(XrdCl::errInvalidResponse, 0,
                            "Issuer discovery did not provide token endpoints");
                return;
            case Stage::OAuthRequest:
                if (success) {
                    Finish(std::move(status), std::move(response));
                } else if (status && !status->IsOK()) {
                    Finish(std::move(status), std::move(response));
                } else {
                    FinishError(XrdCl::errInvalidResponse, 0,
                                "Token request returned an invalid response");
                }
                return;
            case Stage::DeviceRequest:
                if (!success) {
                    Finish(std::move(status), std::move(response));
                    return;
                }
                {
                    auto parsed = nlohmann::json::parse(value, nullptr, false);
                    if (parsed.is_object() &&
                        !parsed.contains("verification_uri") &&
                        parsed.contains("verification_url") &&
                        parsed["verification_url"].is_string()) {
                        parsed["verification_uri"] = parsed["verification_url"];
                    }
                    if (parsed.is_discarded() || !parsed.is_object() ||
                        !parsed.contains("device_code") ||
                        !parsed["device_code"].is_string() ||
                        !parsed.contains("user_code") ||
                        !parsed["user_code"].is_string() ||
                        !parsed.contains("verification_uri") ||
                        !parsed["verification_uri"].is_string() ||
                        !parsed.contains("expires_in") ||
                        !parsed["expires_in"].is_number_unsigned()) {
                        FinishError(XrdCl::errInvalidResponse, 0,
                                    "Invalid device authorization response");
                        return;
                    }
                    std::string verification_url;
                    if (!NormalizeTokenUrl(parsed["verification_uri"].get<std::string>(),
                                           verification_url)) {
                        FinishError(XrdCl::errInvalidResponse, 0,
                                    "Device verification URL must use HTTPS");
                        return;
                    }
                    parsed["token_endpoint"] = m_token_endpoint;
                    auto result = std::make_unique<XrdCl::AnyObject>();
                    auto buffer = new XrdCl::Buffer();
                    buffer->FromString(parsed.dump());
                    result->Set(buffer);
                    Finish(std::move(status), std::move(result));
                }
                return;
            }
        } catch (const TokenWorkflowExpired &) {
            FinishError(XrdCl::errOperationExpired, 0,
                        "Token issuer workflow expired");
        } catch (...) {
            FinishError(XrdCl::errOSError, 0,
                        "Failed to queue the next token request stage");
        }
    }

private:
    enum class Stage {
        OAuthDiscovery,
        OpenIdDiscovery,
        OAuthRequest,
        DeviceRequest
    };

    using HttpVerb = CurlOperation::HttpVerb;
    using HeaderList = CurlOperation::HeaderList;

    void Queue(Stage stage, const std::string &url, HttpVerb verb,
               HeaderList headers, const std::string &body,
               const std::string &response_key)
    {
        if (std::chrono::steady_clock::now() > m_expiry) {
            throw TokenWorkflowExpired{};
        }
        m_stage = stage;
        auto self = shared_from_this();
        std::unique_ptr<CurlTokenOp> operation(new CurlTokenOp(
            this, std::move(self), url, verb, std::move(headers), body,
            response_key, m_expiry, m_logger, m_callout));
        // Start() runs on the caller thread and follows the normal queue
        // backpressure behavior. Successor stages run from a curl worker
        // callback, where blocking behind a full queue could stall every
        // worker.
        if (m_first_stage) {
            m_first_stage = false;
            m_queue->Produce(std::move(operation));
        } else if (!m_queue->TryProduce(std::move(operation))) {
            if (std::chrono::steady_clock::now() > m_expiry) {
                throw TokenWorkflowExpired{};
            }
            throw TokenWorkflowQueueError{};
        }
    }

    void BeginOAuthDiscovery()
    {
        std::string url;
        if (!BuildOAuthAuthorizationServerUrl(m_request.issuer, url)) {
            BeginOpenIdDiscovery();
            return;
        }
        Queue(Stage::OAuthDiscovery, url, HttpVerb::GET, {}, {},
              m_request.type == TokenRequest::Type::DeviceStart ?
                  "$json" : "token_endpoint");
    }

    void BeginOpenIdDiscovery()
    {
        std::string url;
        if (!BuildOpenIdConfigurationUrl(m_request.issuer, url)) {
            FinishError(XrdCl::errInvalidArgs, 0, "Invalid issuer URL");
            return;
        }
        Queue(Stage::OpenIdDiscovery, url, HttpVerb::GET, {}, {},
              m_request.type == TokenRequest::Type::DeviceStart ?
                  "$json" : "token_endpoint");
    }

    bool BeginDeviceRequest(const std::string &metadata)
    {
        auto parsed = nlohmann::json::parse(metadata, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() ||
            !parsed.contains("token_endpoint") ||
            !parsed["token_endpoint"].is_string() ||
            !parsed.contains("device_authorization_endpoint") ||
            !parsed["device_authorization_endpoint"].is_string()) return false;
        std::string device_url;
        if (!NormalizeIssuerEndpoint(parsed["token_endpoint"].get<std::string>(),
                               m_token_endpoint) ||
            !NormalizeIssuerEndpoint(
                parsed["device_authorization_endpoint"].get<std::string>(),
                device_url)) return false;
        QueueDeviceRequest(device_url);
        return true;
    }

    void QueueDeviceRequest(const std::string &device_url)
    {
        Queue(Stage::DeviceRequest, device_url, HttpVerb::POST,
              {{"Content-Type", "application/x-www-form-urlencoded"},
               {"Accept", "application/json"}},
              BuildDeviceAuthorizationRequest(m_request.scope,
                                              m_request.client_id), "$json");
    }

    void BeginOAuthRequest(const std::string &endpoint)
    {
        std::string url;
        std::string body;
        std::string error;
        if (!NormalizeIssuerEndpoint(endpoint, url)) {
            if (m_stage == Stage::OAuthDiscovery) {
                BeginOpenIdDiscovery();
            } else {
                FinishError(XrdCl::errInvalidResponse, 0,
                            "Issuer returned an invalid token endpoint");
            }
            return;
        }
        if (m_request.type == TokenRequest::Type::OAuth) {
            body = BuildOAuthRequest(m_request.scope, m_request.client_id,
                                     m_request.client_secret);
        } else {
            if (!BuildOAuthMacaroonRequest(m_request.path, m_request.validity,
                                           m_request.activities,
                                           m_request.client_id,
                                           m_request.client_secret,
                                           body, error)) {
                FinishError(XrdCl::errInvalidArgs, 0, error);
                return;
            }
        }
        Queue(Stage::OAuthRequest, url, HttpVerb::POST,
              {{"Content-Type", "application/x-www-form-urlencoded"},
               {"Accept", "application/json"}},
              body, "access_token");
    }

    void Finish(std::unique_ptr<XrdCl::XRootDStatus> status,
                std::unique_ptr<XrdCl::AnyObject> response)
    {
        auto handler = m_handler;
        m_handler = nullptr;
        if (handler) {
            handler->HandleResponse(status.release(), response.release());
        }
    }

    void FinishError(uint16_t err_code, uint32_t err_num,
                     const std::string &message)
    {
        auto status = std::make_unique<XrdCl::XRootDStatus>(
            XrdCl::stError, err_code, err_num, message);
        Finish(std::move(status), {});
    }

    std::shared_ptr<HandlerQueue> m_queue;
    XrdCl::ResponseHandler *m_handler{nullptr};
    TokenRequest m_request;
    std::string m_token_endpoint;
    std::chrono::steady_clock::time_point m_expiry;
    XrdCl::Log *m_logger{nullptr};
    CreateConnCalloutType m_callout{nullptr};
    Stage m_stage{Stage::OAuthDiscovery};
    bool m_first_stage{true};
};

} // anonymous namespace

Filesystem::Filesystem(const std::string &url, std::shared_ptr<HandlerQueue> queue, XrdCl::Log *log)
    : m_queue(queue),
      m_logger(log),
      m_url(url)
{
    m_logger->Debug(kLogXrdClHttp, "Constructing filesystem object with base URL %s", url.c_str());
    // When constructed from the root protocol handler, we've observed it include the
    // path here (the code paths appear to be slightly different from http://).  Strip
    // it out so it's not included twice later.
    m_url.SetPath("/");
    XrdCl::URL::ParamsMap map;
    m_url.SetParams(map);
}

Filesystem::~Filesystem() noexcept {}

XrdCl::XRootDStatus
Filesystem::QueueOperation(std::unique_ptr<CurlOperation> operation,
                           const char *description)
{
    try
    {
        m_queue->Produce(std::move(operation));
    }
    catch(const std::exception &ex)
    {
        m_logger->Warning(kLogXrdClHttp,
            "Failed to add %s to queue: %s", description, ex.what());
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError,
                                  EIO, ex.what());
    }
    return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus
Filesystem::DirList(const std::string          &path,
                    XrdCl::DirListFlags::Flags  flags,
                    XrdCl::ResponseHandler     *handler,
                    time_t                      timeout )
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);

    auto full_url = GetCurrentURL(path);

    m_logger->Debug(kLogXrdClHttp, "Filesystem::DirList path %s", path.c_str());
    auto listdirOp = std::make_unique<XrdClHttp::CurlListdirOp>(
        handler, full_url,
        m_url.GetHostName() + ":" + std::to_string(m_url.GetPort()),
        SendResponseInfo(), ts, m_logger,
        GetConnCallout(), m_header_callout.load(std::memory_order_acquire));
    return QueueOperation(std::move(listdirOp), "directory list operation");
}

CreateConnCalloutType
Filesystem::GetConnCallout() const {
    std::string pointer_str;
    if (!GetProperty("XrdClConnectionCallout", pointer_str) && pointer_str.empty()) {
        return nullptr;
    }
    long long pointer;
    try {
        pointer = std::stoll(pointer_str, nullptr, 16);
    } catch (...) {
        return nullptr;
    }
    if (!pointer) {
        return nullptr;
    }
    return reinterpret_cast<CreateConnCalloutType>(pointer);
}

bool
Filesystem::GetProperty(const std::string &name,
                        std::string       &value) const
{
    std::shared_lock lock(m_properties_mutex);

    const auto p = m_properties.find(name);
    if (p == std::end(m_properties)) {
        return false;
    }

    value = p->second;
    return true;
}

// Trivial implementation of the "locate" call
//
// On Linux, this is invoked by the XrdCl client prior to directory listings.
// Given there's no concept of multiple locations currently, we just return
// the original host and port as the available "location".
XrdCl::XRootDStatus
Filesystem::Locate( const std::string        &path,
                    XrdCl::OpenFlags::Flags   flags,
                    XrdCl::ResponseHandler   *handler,
                    time_t                    timeout )
{
    if (!handler) return XrdCl::XRootDStatus();

    auto locateInfo = std::make_unique<XrdCl::LocationInfo>();
    locateInfo->Add(XrdCl::LocationInfo::Location(m_url.GetHostName() + ":" + std::to_string(m_url.GetPort()), XrdCl::LocationInfo::ServerOnline, XrdCl::LocationInfo::Read));

    auto obj = std::make_unique<XrdCl::AnyObject>();
    obj->Set(locateInfo.release());
    handler->HandleResponse(new XrdCl::XRootDStatus(), obj.release());

    return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus Filesystem::MkDir(const std::string        &path,
                                      XrdCl::MkDirFlags::Flags  flags,
                                      XrdCl::Access::Mode       mode,
                                      XrdCl::ResponseHandler   *handler,
                                      time_t                    timeout)
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);

    auto full_url = GetCurrentURL(path);
    m_logger->Debug(kLogXrdClHttp, "Filesystem::MkDir path %s", full_url.c_str());

    auto mkdirOp = std::make_unique<CurlMkcolOp>(
        handler, full_url, ts, m_logger, SendResponseInfo(), GetConnCallout(),
        m_header_callout.load(std::memory_order_acquire));
    return QueueOperation(std::move(mkdirOp), "filesystem mkdir operation");
}

XrdCl::XRootDStatus Filesystem::Prepare(
    const std::vector<std::string> &fileList,
    XrdCl::PrepareFlags::Flags      flags,
    uint8_t                         priority,
    XrdCl::ResponseHandler         *handler,
    time_t                          timeout)
{
    (void)priority;

    if(fileList.empty())
    {
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidArgs,
            EINVAL, "missing prepare file list");
    }

    const auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);
    auto tapeOp = std::make_unique<CurlTapePrepareOp>(
        handler, m_url.GetURL(), fileList, flags, ts, m_logger,
        GetConnCallout(),
        m_header_callout.load(std::memory_order_acquire));
    return QueueOperation(std::move(tapeOp), "Tape prepare operation");
}

XrdCl::XRootDStatus Filesystem::Query(XrdCl::QueryCode::Code  queryCode,
    const XrdCl::Buffer     &arg,
    XrdCl::ResponseHandler  *handler,
    time_t                   timeout)
{
    const auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);
    std::unique_ptr<CurlOperation> operation;
    const char *description = nullptr;

    switch(queryCode)
    {
        case XrdCl::QueryCode::Prepare:
        case XrdCl::QueryCode::Opaque:
            operation = std::make_unique<CurlTapeQueryOp>(
                handler, m_url.GetURL(), queryCode, arg, ts, m_logger,
                GetConnCallout(),
                m_header_callout.load(std::memory_order_acquire));
            description = "Tape query operation";
            break;
        case XrdCl::QueryCode::Checksum:
        {
            const auto url = GetCurrentURL(arg.ToString());
            m_logger->Debug(kLogXrdClHttp,
                "XrdClHttp::Filesystem::Query checksum path %s", url.c_str());

            XrdClHttp::ChecksumType preferred =
                XrdClHttp::ChecksumType::kAll;
            XrdCl::URL urlObj;
            urlObj.FromString(url);
            const auto iter = urlObj.GetParams().find("cks.type");
            if(iter != urlObj.GetParams().end())
            {
                preferred = XrdClHttp::GetTypeFromString(iter->second);
                if(preferred == XrdClHttp::ChecksumType::kUnknown)
                {
                    m_logger->Error(kLogXrdClHttp,
                        "Unknown checksum type %s", iter->second.c_str());
                    return XrdCl::XRootDStatus(XrdCl::stError,
                        XrdCl::errInvalidArgs, EINVAL,
                        "unknown checksum type '" + iter->second + "'");
                }
            }
            operation = std::make_unique<CurlChecksumOp>(
                handler, url, preferred, ts, m_logger, SendResponseInfo(),
                GetConnCallout(),
                m_header_callout.load(std::memory_order_acquire));
            description = "checksum operation";
            break;
        }
        case XrdCl::QueryCode::XAttr:
        {
            const std::string path = arg.ToString();
            m_logger->Debug(kLogXrdClHttp,
                "XrdClHttp::Filesystem::Query xattr full_url %s, path %s",
                m_url.GetURL().c_str(), path.c_str());
            operation = std::make_unique<CurlQueryOp>(
                handler, path, ts, m_logger, SendResponseInfo(),
                GetConnCallout(), queryCode,
                m_header_callout.load(std::memory_order_acquire));
            description = "xattr query operation";
            break;
        }
        case XrdCl::QueryCode::Visa:
        {
            std::size_t size = arg.GetSize();
            if (size && arg.GetBuffer()[size - 1] == '\0') --size;
            std::string input;
            if (size) input.assign(arg.GetBuffer(), size);

            TokenRequest request;
            std::string error;
            if (!ParseTokenRequest(input, request, error)) {
                return XrdCl::XRootDStatus(
                    XrdCl::stError, XrdCl::errInvalidArgs, 0, error
                );
            }

            std::string target_url;
            if (!NormalizeTokenUrl(GetCurrentURL(request.path), target_url)) {
                return XrdCl::XRootDStatus(
                    XrdCl::stError, XrdCl::errNotSupported, 0,
                    "Token requests require an HTTPS or DAVS filesystem URL"
                );
            }

            if (request.type == TokenRequest::Type::DevicePoll) {
                std::string endpoint;
                if (!NormalizeIssuerEndpoint(request.token_endpoint, endpoint)) {
                    return XrdCl::XRootDStatus(
                        XrdCl::stError, XrdCl::errInvalidArgs, 0,
                        "Device token endpoint must use HTTPS");
                }
                operation = std::make_unique<CurlTokenOp>(
                    handler, std::shared_ptr<XrdCl::ResponseHandler>{},
                    endpoint, CurlOperation::HttpVerb::POST,
                    CurlOperation::HeaderList{
                        {"Content-Type", "application/x-www-form-urlencoded"},
                        {"Accept", "application/json"}},
                    BuildDeviceTokenRequest(request.device_code,
                                            request.client_id,
                                            request.client_secret),
                    "$poll", ts, m_logger, GetConnCallout());
                description = "device token poll";
                break;
            }
            if (request.type != TokenRequest::Type::Macaroon) {
                std::string normalized_issuer;
                if (!NormalizeTokenUrl(request.issuer, normalized_issuer)) {
                    return XrdCl::XRootDStatus(
                        XrdCl::stError, XrdCl::errNotSupported, 0,
                        "Token issuers require an HTTPS or DAVS URL"
                    );
                }
                std::string discovery_url;
                if (!BuildOAuthAuthorizationServerUrl(request.issuer,
                                                      discovery_url)) {
                    return XrdCl::XRootDStatus(
                        XrdCl::stError, XrdCl::errInvalidArgs, 0,
                        "Invalid token issuer URL"
                    );
                }
                auto workflow = std::make_shared<TokenIssuerWorkflow>(
                    m_queue, handler, std::move(request), ts,
                    m_logger, GetConnCallout());
                if (!workflow->Start()) {
                    m_logger->Warning(kLogXrdClHttp,
                        "Failed to add issuer token workflow to queue");
                    return XrdCl::XRootDStatus(XrdCl::stError,
                                               XrdCl::errOSError);
                }
                return XrdCl::XRootDStatus();
            }

            operation = std::make_unique<CurlTokenOp>(
                handler, target_url,
                BuildMacaroonRequest(request.validity,
                                     request.activities),
                ts, m_logger, GetConnCallout()
            );
            description = "token operation";
            break;
        }
        default:
            return XrdCl::XRootDStatus(
                XrdCl::stError, XrdCl::errNotImplemented);
    }

    return QueueOperation(std::move(operation), description);
}

XrdCl::XRootDStatus
Filesystem::Rm(const std::string      &path,
               XrdCl::ResponseHandler *handler,
               time_t                  timeout)
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);

    auto full_url = GetCurrentURL(path);
    m_logger->Debug(kLogXrdClHttp, "Filesystem::Rm path %s", full_url.c_str());

    auto deleteOp = std::make_unique<CurlDeleteOp>(
        handler, full_url, ts, m_logger, SendResponseInfo(),
        GetConnCallout(), m_header_callout.load(std::memory_order_acquire));
    return QueueOperation(std::move(deleteOp), "filesystem delete operation");
}

XrdCl::XRootDStatus
Filesystem::RmDir(const std::string      &path,
                  XrdCl::ResponseHandler *handler,
                  time_t                  timeout)
{
    return Rm(path, handler, timeout);
}

bool
Filesystem::SetProperty(const std::string &name,
                        const std::string &value)
{
    if (name == "XrdClHttpHeaderCallout") {
        long long pointer;
        try {
            pointer = std::stoll(value, nullptr, 16);
        } catch (...) {
            pointer = 0;
        }
        if (!pointer) {
            pointer = 0;
        }
        m_header_callout.store(reinterpret_cast<XrdClHttp::HeaderCallout*>(pointer), std::memory_order_release);
    }

    std::unique_lock lock(m_properties_mutex);
    m_properties[name] = value;
    return true;
}

XrdCl::XRootDStatus
Filesystem::Stat(const std::string      &path,
                 XrdCl::ResponseHandler *handler,
                 time_t                  timeout)
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);

    auto full_url = GetCurrentURL(path);
    m_logger->Debug(kLogXrdClHttp, "Filesystem::Stat path %s", full_url.c_str());

    auto statOp = std::make_unique<CurlStatOp>(
        handler, full_url, ts, m_logger, SendResponseInfo(),
        GetConnCallout(), m_header_callout.load(std::memory_order_acquire));
    return QueueOperation(std::move(statOp), "filesystem stat operation");
}

bool Filesystem::SendResponseInfo() const {
    std::string val;
    return GetProperty(ResponseInfoProperty, val) && val == "true";
}

std::string Filesystem::GetCurrentURL(const std::string &path) const {

    // Compute the URL without trailing slash.
    auto prefix = m_url.GetURL();
    std::string_view prefix_view = prefix;
    while (!prefix_view.empty() && prefix_view[prefix_view.size() - 1] == '/')
        prefix_view = prefix_view.substr(0, prefix_view.size() - 1);

    // Compute the target path without the '/' prefix
    std::string_view path_view = path;
    while (!path_view.empty() && path_view[0] == '/')
        path_view = path_view.substr(1);
    auto retval = std::string(prefix_view) + "/" + std::string(path_view);

    // Add in the query parameters, if relevant.
    {
        std::shared_lock lock(m_properties_mutex);
        auto iter = m_properties.find("XrdClHttpQueryParam");
        if (iter != m_properties.end() && !iter->second.empty()) {
            retval += ((retval.find('?') == std::string::npos) ? '?' : ':') + iter->second;
        }
    }
    return retval;
}
