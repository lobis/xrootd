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
#include "XrdClHttpTape.hh"

#include "XrdCl/XrdClAnyObject.hh"
#include "XrdOuc/XrdOucJson.hh"

#include <array>
#include <functional>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>

using namespace XrdClHttp;

namespace
{
using Json = nlohmann::json;

const std::string kStructuredStagePrefix = "xrdclhttp.tape.stage:";

std::vector<std::string> SplitLines(const std::string &value)
{
    std::vector<std::string> lines;
    if(value.empty()) return lines;

    std::size_t start = 0;
    while(start <= value.size())
    {
        const std::size_t end = value.find('\n', start);
        if(end == std::string::npos)
        {
            lines.push_back(value.substr(start));
            break;
        }
        lines.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

bool ContainsCarriageReturn(const std::string &value)
{
    return value.find('\r') != std::string::npos;
}

bool HasPrepareFlag(XrdCl::PrepareFlags::Flags flags,
                    XrdCl::PrepareFlags::Flags flag)
{
    return (static_cast<int>(flags) & static_cast<int>(flag)) != 0;
}

XrdCl::XRootDStatus ValidateTapePrepareFlags(XrdCl::PrepareFlags::Flags flags)
{
    const int requested = static_cast<int>(flags);
    const int supported =
        static_cast<int>(XrdCl::PrepareFlags::Stage)
        | static_cast<int>(XrdCl::PrepareFlags::Cancel)
        | static_cast<int>(XrdCl::PrepareFlags::Evict);

    if(requested & ~supported)
    {
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported,
            0, "HTTP Tape REST prepare supports stage, cancel, and evict only");
    }

    int operations = 0;
    if(HasPrepareFlag(flags, XrdCl::PrepareFlags::Stage)) ++operations;
    if(HasPrepareFlag(flags, XrdCl::PrepareFlags::Cancel)) ++operations;
    if(HasPrepareFlag(flags, XrdCl::PrepareFlags::Evict)) ++operations;

    if(operations == 0)
    {
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported,
            0, "HTTP Tape REST prepare supports stage, cancel, and evict only");
    }
    if(operations > 1)
    {
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidArgs,
            0, "HTTP Tape REST prepare expects exactly one operation flag");
    }
    return XrdCl::XRootDStatus();
}

void DeliverTapeResponse(XrdCl::ResponseHandler *handler,
                         const XrdCl::XRootDStatus &status,
                         const std::string &response)
{
    if(!handler) return;

    XrdCl::AnyObject *object = nullptr;
    if(status.IsOK())
    {
        auto buffer = new XrdCl::Buffer();
        buffer->FromString(response);
        object = new XrdCl::AnyObject();
        object->Set(buffer);
    }
    handler->HandleResponse(new XrdCl::XRootDStatus(status), object);
}

// Run a Tape REST operation without blocking the caller; the response
// handler is invoked from a dedicated thread once the operation completes,
// matching the asynchronous XrdCl::FileSystem contract. The operation
// receives a string to fill with the response payload.
XrdCl::XRootDStatus RunTapeOperation(
    XrdCl::ResponseHandler *handler,
    std::function<XrdCl::XRootDStatus(std::string &)> operation)
{
    try
    {
        std::thread([handler, operation = std::move(operation)]() {
            std::string response;
            const XrdCl::XRootDStatus status = operation(response);
            DeliverTapeResponse(handler, status, response);
        }).detach();
    }
    catch(const std::system_error &)
    {
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError,
            0, "unable to start tape operation thread");
    }
    return XrdCl::XRootDStatus();
}

int TapeTimeout(time_t timeout)
{
    if(timeout < 0) return -1;
    if(timeout > 0)
    {
        if(timeout >= std::numeric_limits<int>::max())
        {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(timeout);
    }

    struct timespec ts =
        XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);
    if(ts.tv_sec < 0 || (ts.tv_sec == 0 && ts.tv_nsec <= 0)) return -1;
    if(ts.tv_sec >= std::numeric_limits<int>::max())
    {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ts.tv_sec + (ts.tv_nsec > 0 ? 1 : 0));
}

XrdCl::XRootDStatus PrepareStageFiles(
    const std::vector<std::string> &fileList,
    std::vector<std::array<std::string, 4>> &files)
{
    files.clear();
    files.reserve(fileList.size());
    for(const auto &file : fileList)
    {
        if(file.compare(0, kStructuredStagePrefix.size(),
                        kStructuredStagePrefix) == 0)
        {
            try
            {
                Json json = Json::parse(
                    file.substr(kStructuredStagePrefix.size()));
                if(!json.is_object())
                {
                    return XrdCl::XRootDStatus(XrdCl::stError,
                        XrdCl::errInvalidArgs, 0,
                        "structured tape stage entry must be a JSON object");
                }

                std::array<std::string, 4> entry;
                if(json.contains("url"))
                {
                    if(!json["url"].is_string())
                    {
                        return XrdCl::XRootDStatus(XrdCl::stError,
                            XrdCl::errInvalidArgs, 0,
                            "structured tape stage entry url must be a string");
                    }
                    entry[0] = json["url"].get<std::string>();
                }
                if(json.contains("path"))
                {
                    if(!json["path"].is_string())
                    {
                        return XrdCl::XRootDStatus(XrdCl::stError,
                            XrdCl::errInvalidArgs, 0,
                            "structured tape stage entry path must be a string");
                    }
                    entry[1] = json["path"].get<std::string>();
                }
                if(entry[0].empty() && entry[1].empty())
                {
                    return XrdCl::XRootDStatus(XrdCl::stError,
                        XrdCl::errInvalidArgs, 0,
                        "structured tape stage entry requires url or path");
                }
                if(json.contains("diskLifetime"))
                {
                    if(!json["diskLifetime"].is_string())
                    {
                        return XrdCl::XRootDStatus(XrdCl::stError,
                            XrdCl::errInvalidArgs, 0,
                            "structured tape stage entry diskLifetime must be a string");
                    }
                    entry[2] = json["diskLifetime"].get<std::string>();
                }
                if(json.contains("targetedMetadata"))
                {
                    if(!json["targetedMetadata"].is_object())
                    {
                        return XrdCl::XRootDStatus(XrdCl::stError,
                            XrdCl::errInvalidArgs, 0,
                            "structured tape stage entry targetedMetadata must be a JSON object");
                    }
                    entry[3] = json["targetedMetadata"].dump();
                }
                files.push_back(entry);
                continue;
            }
            catch(const std::exception &ex)
            {
                return XrdCl::XRootDStatus(XrdCl::stError,
                    XrdCl::errInvalidArgs, 0,
                    "malformed structured tape stage entry: "
                    + std::string(ex.what()));
            }
        }
        files.push_back({file, "", "", ""});
    }
    return XrdCl::XRootDStatus();
}

std::vector<std::string>
PreparePathsAfterRequestId(const std::vector<std::string> &fileList)
{
    if(fileList.size() <= 1) return {};
    return std::vector<std::string>(fileList.begin() + 1, fileList.end());
}
}

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
Filesystem::DirList(const std::string          &path,
                    XrdCl::DirListFlags::Flags  flags,
                    XrdCl::ResponseHandler     *handler,
                    time_t                      timeout )
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);

    auto full_url = GetCurrentURL(path);

    m_logger->Debug(kLogXrdClHttp, "Filesystem::DirList path %s", path.c_str());
    std::unique_ptr<XrdClHttp::CurlListdirOp> listdirOp(
        new XrdClHttp::CurlListdirOp(
            handler, full_url,
            m_url.GetHostName() + ":" + std::to_string(m_url.GetPort()),
            SendResponseInfo(), ts, m_logger,
            GetConnCallout(), m_header_callout.load(std::memory_order_acquire)
        )
    );

    try {
        m_queue->Produce(std::move(listdirOp));
    } catch (...) {
        m_logger->Warning(kLogXrdClHttp, "Failed to add dirlist op to queue");
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError);
    }

    return XrdCl::XRootDStatus();
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

    std::unique_ptr<CurlMkcolOp> mkdirOp(
        new CurlMkcolOp(
            handler, full_url, ts, m_logger, SendResponseInfo(), GetConnCallout(),
            m_header_callout.load(std::memory_order_acquire)
        )
    );
    try {
        m_queue->Produce(std::move(mkdirOp));
    } catch (...) {
        m_logger->Warning(kLogXrdClHttp, "Failed to add filesystem mkdir op to queue");
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError);
    }

    return XrdCl::XRootDStatus();
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
            0, "missing prepare file list");
    }
    XrdCl::XRootDStatus status = ValidateTapePrepareFlags(flags);
    if(!status.IsOK()) return status;

    // Arguments are validated synchronously; the Tape REST round trips run
    // on a dedicated thread which invokes the handler once done.
    XrdClHttp::TapeOptions options;
    options.timeout = TapeTimeout(timeout);
    options.headerCallout = m_header_callout.load(std::memory_order_acquire);
    const std::string url = m_url.GetURL();

    if(HasPrepareFlag(flags, XrdCl::PrepareFlags::Stage))
    {
        std::vector<std::array<std::string, 4>> files;
        status = PrepareStageFiles(fileList, files);
        if(!status.IsOK()) return status;

        return RunTapeOperation(handler,
            [url, files = std::move(files), options](std::string &response) {
                return XrdClHttp::TapeStage(url, files, options, response);
            });
    }

    const std::string requestId = fileList.front();
    const std::vector<std::string> paths = PreparePathsAfterRequestId(fileList);

    if(HasPrepareFlag(flags, XrdCl::PrepareFlags::Cancel))
    {
        return RunTapeOperation(handler,
            [url, requestId, paths, options](std::string &) {
                return XrdClHttp::TapeStageCancel(url, requestId, paths,
                                                  options);
            });
    }

    if(HasPrepareFlag(flags, XrdCl::PrepareFlags::Evict))
    {
        return RunTapeOperation(handler,
            [url, requestId, paths, options](std::string &) {
                return XrdClHttp::TapeRelease(url, requestId, paths, options);
            });
    }

    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported,
        0, "HTTP Tape REST prepare supports stage, cancel, and evict only");
}

XrdCl::XRootDStatus Filesystem::Query(XrdCl::QueryCode::Code  queryCode,
    const XrdCl::Buffer     &arg,
    XrdCl::ResponseHandler  *handler,
    time_t                   timeout)
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);
    // Tape REST queries validate their arguments synchronously; the HTTP
    // round trips run on a dedicated thread which invokes the handler.
    XrdClHttp::TapeOptions tapeOptions;
    tapeOptions.timeout = TapeTimeout(timeout);
    tapeOptions.headerCallout =
        m_header_callout.load(std::memory_order_acquire);
    const std::string tapeUrl = m_url.GetURL();

    if (queryCode == XrdCl::QueryCode::Prepare)
    {
        std::vector<std::string> args = SplitLines(arg.ToString());
        if(args.size() != 1 || args.front().empty()
           || ContainsCarriageReturn(args.front()))
        {
            return XrdCl::XRootDStatus(XrdCl::stError,
                XrdCl::errInvalidArgs, 0,
                "prepare query expects a single request id");
        }
        const std::string requestId = args.front();
        return RunTapeOperation(handler,
            [tapeUrl, requestId, tapeOptions](std::string &response) {
                return XrdClHttp::TapeStageStatus(tapeUrl, requestId,
                                                  tapeOptions, response);
            });
    }
    else if (queryCode == XrdCl::QueryCode::Opaque)
    {
        std::vector<std::string> args = SplitLines(arg.ToString());
        if(args.empty() || args.front().empty()
           || ContainsCarriageReturn(args.front()))
        {
            return XrdCl::XRootDStatus(XrdCl::stError,
                XrdCl::errInvalidArgs, 0, "missing opaque query command");
        }

        if(args[0] == "tape.discover")
        {
            return RunTapeOperation(handler,
                [tapeUrl, tapeOptions](std::string &response) {
                    std::string uri, version, sitename;
                    XrdCl::XRootDStatus status = XrdClHttp::TapeDiscover(
                        tapeUrl, tapeOptions, uri, version, sitename);
                    if(!status.IsOK()) return status;

                    Json json;
                    json["uri"] = uri;
                    json["version"] = version;
                    json["sitename"] = sitename;
                    response = json.dump();
                    return status;
                });
        }
        else if(args[0] == "tape.archiveinfo")
        {
            if(args.size() < 2)
            {
                return XrdCl::XRootDStatus(XrdCl::stError,
                    XrdCl::errInvalidArgs, 0,
                    "tape.archiveinfo expects non-empty URLs");
            }
            for(auto it = args.begin() + 1; it != args.end(); ++it)
            {
                if(it->empty() || ContainsCarriageReturn(*it))
                {
                    return XrdCl::XRootDStatus(XrdCl::stError,
                        XrdCl::errInvalidArgs, 0,
                        "tape.archiveinfo expects non-empty URLs");
                }
            }
            const std::vector<std::string> urls(args.begin() + 1, args.end());
            return RunTapeOperation(handler,
                [urls, tapeOptions](std::string &response) {
                    return XrdClHttp::TapeArchiveInfo(urls, tapeOptions,
                                                      response);
                });
        }
        else if(args[0] == "tape.stage_delete")
        {
            if(args.size() != 2 || args[1].empty()
               || ContainsCarriageReturn(args[1]))
            {
                return XrdCl::XRootDStatus(XrdCl::stError,
                    XrdCl::errInvalidArgs, 0,
                    "tape.stage_delete expects a request id");
            }
            const std::string requestId = args[1];
            return RunTapeOperation(handler,
                [tapeUrl, requestId, tapeOptions](std::string &) {
                    return XrdClHttp::TapeStageDelete(tapeUrl, requestId,
                                                      tapeOptions);
                });
        }
        else
        {
            return XrdCl::XRootDStatus(XrdCl::stError,
                XrdCl::errNotSupported, 0,
                "unsupported HTTP opaque query");
        }
    }
    else if (queryCode == XrdCl::QueryCode::Checksum)
    {
        auto url = GetCurrentURL(arg.ToString());
        m_logger->Debug(kLogXrdClHttp, "XrdClHttp::Filesystem::Query checksum path %s", url.c_str());

        XrdClHttp::ChecksumType preferred = XrdClHttp::ChecksumType::kCRC32C;
        XrdCl::URL url_obj;
        url_obj.FromString(url);
        auto iter = url_obj.GetParams().find("cks.type");
        if (iter != url_obj.GetParams().end())
        {
            preferred = XrdClHttp::GetTypeFromString(iter->second);
            if (preferred == XrdClHttp::ChecksumType::kUnknown)
            {
                m_logger->Error(kLogXrdClHttp, "Unknown checksum type %s", iter->second.c_str());
                preferred = XrdClHttp::ChecksumType::kCRC32C;
            }
        }
        // On miss, queue a checksum operation
        std::unique_ptr<CurlChecksumOp> cksumOp(
            new CurlChecksumOp(
                handler, url, preferred, ts, m_logger, SendResponseInfo(),
                GetConnCallout(), m_header_callout.load(std::memory_order_acquire)
            )
        );
        try
        {
            m_queue->Produce(std::move(cksumOp));
        }
        catch (...)
        {
            m_logger->Warning(kLogXrdClHttp, "Failed to add checksum operation to queue");
            return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError);
        }
    }
    else if (queryCode == XrdCl::QueryCode::XAttr)
    {
        std::string path = arg.ToString();
        std::string full_url = m_url.GetURL();
        m_logger->Debug(kLogXrdClHttp, "XrdClHttp::Filesystem::Query xattr full_url %s, path %s", full_url.c_str(), path.c_str());
        full_url = m_url.GetURL();
        std::unique_ptr<CurlQueryOp> queryOp(
            new CurlQueryOp(
                handler, path, ts, m_logger,SendResponseInfo(),
                GetConnCallout(), queryCode, m_header_callout.load(std::memory_order_acquire)
            )
        );
        try
        {
            m_queue->Produce(std::move(queryOp));
        }
        catch (...)
        {
            m_logger->Warning(kLogXrdClHttp, "Failed to add xattr query operation to queue");
            return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError);
        }
    }
    else
    {
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotImplemented);
    }
    return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus
Filesystem::Rm(const std::string      &path,
               XrdCl::ResponseHandler *handler,
               time_t                  timeout)
{
    auto ts = XrdClHttp::Factory::GetHeaderTimeoutWithDefault(timeout);

    auto full_url = GetCurrentURL(path);
    m_logger->Debug(kLogXrdClHttp, "Filesystem::Rm path %s", full_url.c_str());

    std::unique_ptr<CurlDeleteOp> deleteOp(
        new CurlDeleteOp(
            handler, full_url, ts, m_logger, SendResponseInfo(),
            GetConnCallout(), m_header_callout.load(std::memory_order_acquire)
        )
    );
    try {
        m_queue->Produce(std::move(deleteOp));
    } catch (...) {
        m_logger->Warning(kLogXrdClHttp, "Failed to add filesystem delete op to queue");
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError);
    }

    return XrdCl::XRootDStatus();
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

    std::unique_ptr<CurlStatOp> statOp(
        new CurlStatOp(
            handler, full_url, ts, m_logger, SendResponseInfo(),
            GetConnCallout(), m_header_callout.load(std::memory_order_acquire)
        )
    );
    try {
        m_queue->Produce(std::move(statOp));
    } catch (...) {
        m_logger->Warning(kLogXrdClHttp, "Failed to add filesystem stat op to queue");
        return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError);
    }

    return XrdCl::XRootDStatus();
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
