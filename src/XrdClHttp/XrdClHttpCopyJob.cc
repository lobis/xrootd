/******************************************************************************/
/* Copyright (C) 2026, XRootD Collaboration                                  */
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
#include "XrdClHttpOps.hh"
#include <XrdCl/XrdClCopyProcess.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClUtils.hh>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <mutex>

using namespace XrdClHttp;
namespace {
class CopyReply final : public XrdCl::ResponseHandler {
public:
    void HandleResponse(XrdCl::XRootDStatus *status, XrdCl::AnyObject *response) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.reset(status);
        m_response.reset(response);
        m_cv.notify_one();
    }
    XrdCl::XRootDStatus Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return bool(m_status); });
        return *m_status;
    }
    template<class T> T *Get() {
        T *value = nullptr;
        if (m_response) m_response->Get(value);
        return value;
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unique_ptr<XrdCl::XRootDStatus> m_status;
    std::unique_ptr<XrdCl::AnyObject> m_response;
};

class CopyProgress final : public CurlCopyOp::CurlProgressCallback {
public:
    CopyProgress(XrdCl::CopyProgressHandler *handler, uint32_t job, uint64_t size)
        : m_handler(handler), m_job(job), m_size(size) {}
    void Progress(off_t bytes) override {
        if (m_handler) m_handler->JobProgress(m_job, bytes, m_size);
    }
private:
    XrdCl::CopyProgressHandler *m_handler;
    uint32_t m_job;
    uint64_t m_size;
};

bool IsHttp(const std::string &value) {
    const auto scheme = XrdCl::URL(value).GetProtocol();
    return scheme == "http" || scheme == "https" || scheme == "dav" || scheme == "davs";
}

XrdCl::URL ResourceUrl(const std::string &value) {
    HttpClientConfig ignored;
    auto wire = ExtractHttpClientConfig(value, ignored);
    if (wire.compare(0, 6, "dav://") == 0) wire.replace(0, 3, "http");
    else if (wire.compare(0, 7, "davs://") == 0) wire.replace(0, 4, "https");
    XrdCl::URL url(wire);
    auto host = url.GetHostName();
    std::transform(host.begin(), host.end(), host.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    url.SetHostPort(host, url.GetPort());
    return url;
}

bool SameResource(const std::string &source, const std::string &target) {
    const auto src = ResourceUrl(source), dst = ResourceUrl(target);
    auto src_params = src.GetParams(), dst_params = dst.GetParams();
    // Authorization changes access, not the identity of the storage object.
    src_params.erase("authz");
    dst_params.erase("authz");
    return src.GetProtocol() == dst.GetProtocol() && src.GetHostName() == dst.GetHostName()
        && src.GetPort() == dst.GetPort() && src.GetPath() == dst.GetPath()
        && src_params == dst_params;
}

}

XrdCl::XRootDStatus Factory::ThirdPartyCopy(uint32_t jobId,
    const XrdCl::PropertyList &properties, XrdCl::PropertyList &results,
    XrdCl::CopyProgressHandler *progress)
{
    using namespace XrdCl;
    std::string source, target, mode = "pull", checksum_mode = "none", checksum_type, preset;
    properties.Get("source", source);
    properties.Get("target", target);
    properties.Get("thirdPartyMode", mode);
    std::string third_party = "only";
    properties.Get("thirdParty", third_party);
    if (!IsHttp(source) || !IsHttp(target))
        return XRootDStatus(stError, errNotSupported, ENOTSUP, "HTTP TPC requires two HTTP/WebDAV endpoints");
    if (SameResource(source, target))
        return XRootDStatus(stError, errInvalidArgs, EINVAL, "HTTP TPC source and destination are the same resource");
    if (mode != "pull" && mode != "push" && mode != "auto")
        return XRootDStatus(stError, errInvalidArgs, EINVAL, "Unknown HTTP TPC direction");
    for (const auto *key : {"delegate", "makeDir", "coerce", "dynamicSource", "zipArchive", "zipAppend", "continue", "preserveXAttr", "xcp", "doServer"}) {
        bool enabled = false;
        properties.Get(key, enabled);
        if (enabled) return XRootDStatus(stError, errNotSupported, ENOTSUP,
            std::string("HTTP TPC does not support copy property: ") + key);
    }
    std::vector<std::string> additional_checksums;
    properties.Get("addcksums", additional_checksums);
    if (!additional_checksums.empty())
        return XRootDStatus(stError, errNotSupported, ENOTSUP, "HTTP TPC does not support additional client checksums");
    for (const auto *key : {"xrate", "xrateThreshold"}) {
        int64_t value = 0;
        properties.Get(key, value);
        if (value > 0) return XRootDStatus(stError, errNotSupported, ENOTSUP,
            std::string("HTTP TPC cannot enforce client transfer rate property: ") + key);
    }
    bool force = false, posc = false, remove_bad_checksum = false;
    properties.Get("force", force);
    properties.Get("posc", posc);
    properties.Get("rmOnBadCksum", remove_bad_checksum);
    properties.Get("checkSumMode", checksum_mode);
    properties.Get("checkSumType", checksum_type);
    properties.Get("checkSumPreset", preset);
    if (checksum_mode != "none" && checksum_mode != "source" &&
        checksum_mode != "target" && checksum_mode != "end2end")
        return XRootDStatus(stError, errInvalidArgs, EINVAL, "Unknown checksum mode");
    auto preferred = checksum_type == "auto" ? ChecksumType::kAll : GetTypeFromString(checksum_type);
    if (checksum_mode != "none" && preferred == ChecksumType::kUnknown)
        return XRootDStatus(stError, errNotSupported, ENOTSUP, "Unsupported HTTP TPC checksum algorithm");

    Initialize();
    time_t timeout = 0;
    properties.Get("tpcTimeout", timeout);
    time_t copy_timeout = 0;
    properties.Get("cpTimeout", copy_timeout);
    if (copy_timeout > 0 && (timeout <= 0 || copy_timeout < timeout)) timeout = copy_timeout;
    const auto duration = GetHeaderTimeoutWithDefault(timeout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration.tv_sec)
        + std::chrono::nanoseconds(duration.tv_nsec);
    const auto cancelled = [progress, jobId] { return progress && progress->ShouldCancel(jobId); };
    const auto remaining = [&] {
        auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) left = std::chrono::nanoseconds(1);
        return timespec{static_cast<time_t>(left.count() / 1000000000), static_cast<long>(left.count() % 1000000000)};
    };
    const auto ready = [&] {
        if (cancelled()) return XRootDStatus(stError, errErrorResponse, kXR_Cancelled, "HTTP TPC cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            return XRootDStatus(stError, errOperationExpired, kXR_ReqTimedOut, "HTTP TPC deadline expired");
        return XRootDStatus();
    };
    const auto stat = [&](const std::string &url, uint64_t &size) {
        auto status = ready();
        if (!status.IsOK()) return status;
        CopyReply reply;
        auto operation = std::make_unique<CurlStatOp>(&reply, url, remaining(), m_log, false, nullptr, nullptr);
        operation->SetCancelCallback(cancelled);
        Produce(std::move(operation));
        status = reply.Wait();
        if (!status.IsOK()) return status;
        auto info = reply.Get<StatInfo>();
        if (!info) return XRootDStatus(stError, errInvalidResponse, EIO, "Missing HTTP TPC stat response");
        if (info->TestFlags(StatInfo::IsDir))
            return XRootDStatus(stError, errErrorResponse, kXR_isDirectory, "HTTP TPC requires file operands");
        size = info->GetSize();
        return XRootDStatus();
    };
    const auto checksum = [&](const std::string &url, std::string &value) {
        auto status = ready();
        if (!status.IsOK()) return status;
        CopyReply reply;
        auto operation = std::make_unique<CurlChecksumOp>(&reply, url, preferred, remaining(), m_log, false, nullptr, nullptr);
        operation->SetCancelCallback(cancelled);
        Produce(std::move(operation));
        status = reply.Wait();
        if (!status.IsOK()) return status;
        auto buffer = reply.Get<Buffer>();
        if (!buffer) return XRootDStatus(stError, errInvalidResponse, EIO, "Missing HTTP TPC checksum response");
        const auto text = buffer->ToString();
        const auto separator = text.find(' ');
        if (separator == std::string::npos)
            return XRootDStatus(stError, errInvalidResponse, EIO, "Malformed HTTP TPC checksum response");
        checksum_type = text.substr(0, separator);
        preferred = GetTypeFromString(checksum_type);
        value = checksum_type + ":" + Utils::NormalizeChecksum(checksum_type, text.substr(separator + 1));
        return XRootDStatus();
    };
    uint64_t size = 0, target_size = 0;
    auto status = stat(source, size);
    if (!status.IsOK()) return status;
    status = stat(target, target_size);
    const bool existed = status.IsOK();
    const bool target_absent = status.errNo == kXR_NotFound;
    if (existed && !force)
        return XRootDStatus(stError, errErrorResponse, kXR_ItExists, "HTTP TPC destination exists");
    if (!status.IsOK() && !target_absent && status.errNo != kXR_NotAuthorized) return status;
    std::string source_checksum;
    if (checksum_mode == "source" || checksum_mode == "end2end") {
        status = checksum(source, source_checksum);
        if (!status.IsOK()) return status;
        results.Set("sourceCheckSum", source_checksum);
        if (!preset.empty() && source_checksum != checksum_type + ":" + Utils::NormalizeChecksum(checksum_type, preset))
            return XRootDStatus(stError, errCheckSumError, EIO, "Source checksum mismatch before HTTP TPC");
    }
    status = ready();
    if (!status.IsOK()) return status;
    const auto cleanup_target = [&](XRootDStatus failure) {
        if (!target_absent || !posc) return failure;
        CopyReply cleanup;
        Produce(std::make_unique<CurlDeleteOp>(&cleanup, target, timespec{5, 0}, m_log, false, nullptr, nullptr));
        auto cleanup_status = cleanup.Wait();
        if (!cleanup_status.IsOK() && cleanup_status.errNo != kXR_NotFound)
            failure.SetErrorMessage(failure.GetErrorMessage() + "; HTTP TPC destination cleanup failed: " + cleanup_status.GetErrorMessage());
        return failure;
    };
    const auto transfer = [&](bool push, bool overwrite) {
        auto state = ready();
        if (!state.IsOK()) return state;
        CurlCopyOp::Headers source_headers, target_headers;
        auto &control_headers = push ? source_headers : target_headers;
        control_headers.emplace_back("Overwrite", overwrite ? "T" : "F");
        // Do not request proxy delegation until a delegation implementation exists.
        control_headers.emplace_back("Credential", "none");
        CopyReply reply;
        auto operation = std::make_unique<CurlCopyOp>(&reply, source, source_headers, target,
            target_headers, remaining(), m_log, nullptr, push);
        operation->SetCallback(std::make_unique<CopyProgress>(progress, jobId, size));
        operation->SetCancelCallback(cancelled);
        Produce(std::move(operation));
        return reply.Wait();
    };
    // Only transfer-channel I/O failures are eligible for automatic retry.
    // Authentication, cancellation, deadlines and validation failures are terminal.
    const auto retryable = [](const XRootDStatus &state) {
        return state.code == errErrorResponse && state.errNo == kXR_IOError;
    };
    status = transfer(mode == "push", force);
    if (mode == "auto" && retryable(status)) {
        status = cleanup_target(status);
        // A prior attempt may have created a partial destination. Overwriting it
        // is allowed only when the initial preflight confirmed it did not exist.
        status = transfer(true, force || target_absent);
        if (retryable(status) && third_party == "first") {
            status = cleanup_target(status);
            auto state = ready();
            if (!state.IsOK()) return state;
            results.Set("httpTpcOverwrite", target_absent);
            const auto left = remaining();
            results.Set("httpTpcRemaining", left.tv_sec + (left.tv_nsec > 0 ? 1 : 0));
            return XRootDStatus(stError, errNotSupported, EIO,
                "HTTP pull and push failed; streamed copy may be attempted");
        }
    }
    if (mode != "auto" && status.code == errErrorResponse && status.errNo == kXR_Unsupported)
        return XRootDStatus(stError, errNotSupported, ENOTSUP, "Server does not support HTTP COPY");
    if (status.IsOK()) {
        status = stat(target, target_size);
        // A write-scoped token need not grant metadata read access. The COPY
        // control channel remains authoritative when target HEAD is forbidden.
        if (status.errNo == kXR_NotAuthorized) status = XRootDStatus();
        else if (status.IsOK() && target_size != size)
            status = XRootDStatus(stError, errDataError, EIO, "HTTP TPC destination size differs from source");
    }
    if (status.IsOK() && (checksum_mode == "target" || checksum_mode == "end2end")) {
        std::string target_checksum;
        status = checksum(target, target_checksum);
        if (status.IsOK()) {
            results.Set("targetCheckSum", target_checksum);
            const auto expected = preset.empty() ? source_checksum : checksum_type + ":" + Utils::NormalizeChecksum(checksum_type, preset);
            if (!expected.empty() && expected != target_checksum)
                status = XRootDStatus(stError, errCheckSumError, EIO, "Destination checksum mismatch after HTTP TPC");
        }
    }
    if (!status.IsOK() && (posc || (remove_bad_checksum && status.code == errCheckSumError)) && target_absent) {
        CopyReply cleanup;
        // Cleanup gets a separate bounded window even if the transfer expired.
        Produce(std::make_unique<CurlDeleteOp>(&cleanup, target, timespec{5, 0}, m_log, false, nullptr, nullptr));
        auto cleanup_status = cleanup.Wait();
        if (!cleanup_status.IsOK() && cleanup_status.errNo != kXR_NotFound)
            status.SetErrorMessage(status.GetErrorMessage() + "; HTTP TPC destination cleanup failed: " + cleanup_status.GetErrorMessage());
    }
    if (status.IsOK()) {
        results.Set("size", size);
        if (progress) progress->JobProgress(jobId, size, size);
    }
    return status;
}
