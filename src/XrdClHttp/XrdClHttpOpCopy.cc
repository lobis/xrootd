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

#include "XrdClHttpOps.hh"

using namespace XrdClHttp;

CurlCopyOp::CurlCopyOp(XrdCl::ResponseHandler *handler, const std::string &source_url, const Headers &source_hdrs,
    const std::string &dest_url, const Headers &dest_hdrs, struct timespec timeout, XrdCl::Log *logger,
    CreateConnCalloutType callout, bool push) :
        CurlOperation(handler, push ? source_url : dest_url, timeout, logger, callout, nullptr),
        m_remote_url(push ? dest_url : source_url),
        m_transfer_headers(push ? dest_hdrs : source_hdrs), m_push(push)
    {
        m_minimum_rate = 1;
        m_operation_expiry = GetHeaderExpiry();
        for (const auto &info : (push ? source_hdrs : dest_hdrs))
            m_headers_list.emplace_back(info.first, info.second);
    }
    
    bool
    CurlCopyOp::Setup(CURL *curl, CurlWorker &worker)
    {
        if (!m_transfer_prepared) {
            auto status = HttpTransferHeaders(m_remote_url, m_transfer_headers, m_logger);
            if (!status.IsOK()) {
                Fail(status.code, status.errNo, status.GetErrorMessage());
                return false;
            }
            for (const auto &header : m_transfer_headers)
                m_headers_list.emplace_back("TransferHeader" + header.first, header.second);
            m_transfer_prepared = true;
        }
        auto rv = CurlOperation::Setup(curl, worker);
        if (!rv) return false;

        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, CurlCopyOp::WriteCallback);
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, this);
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, "COPY");
        m_headers_list.emplace_back(m_push ? "Destination" : "Source", m_remote_url);

        return true;
    }
    
    void
    CurlCopyOp::Success()
    {
        const auto result = m_response.Finish();
        if (!result.IsOK()) {
            Fail(result.code, result.errNo, result.GetErrorMessage());
            return;
        }
        SetDone(false);
        if (m_handler == nullptr) {return;}
        auto status = new XrdCl::XRootDStatus();
        auto obj = new XrdCl::AnyObject();
        auto handle = m_handler;
        m_handler = nullptr;
        handle->HandleResponse(status, obj);
    }
    
    void
    CurlCopyOp::ReleaseHandle()
    {
        if (m_curl == nullptr) return;
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_HTTPHEADER, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_XFERINFOFUNCTION, nullptr);
        CurlOperation::ReleaseHandle();
    }
    
    size_t
    CurlCopyOp::WriteCallback(char *buffer, size_t size, size_t nitems, void *this_ptr)
    {
        auto me = reinterpret_cast<CurlCopyOp*>(this_ptr);
        me->UpdateBytes(size * nitems);
        // Redirect and error bodies are not the TPC control channel. Preserve
        // the HTTP status and let the worker handle those responses.
        const auto status_code = me->m_headers.GetStatusCode();
        if (status_code < 200 || status_code >= 300) return size * nitems;
        if (!me->m_response.Feed(std::string_view(buffer, size * nitems))) {
            const auto &status = me->m_response.Status();
            return me->FailCallback(static_cast<XErrorCode>(status.errNo), status.GetErrorMessage());
        }
        return size * nitems;
    }

    void
    CurlCopyOp::SetCallback(std::unique_ptr<CurlProgressCallback> callback)
    {
        m_callback = std::move(callback);
        m_response.SetCallback([this](off_t bytes) {
            if (m_callback) m_callback->Progress(bytes);
        });
    }
