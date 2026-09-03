//------------------------------------------------------------------------------
// This file is part of XrdHTTP: A pragmatic implementation of the
// HTTP/WebDAV protocol for the Xrootd framework
//
// Copyright (c) 2017 by European Organization for Nuclear Research (CERN)
// Author: Fabrizio Furano <furano@cern.ch>
// File Date: May 2017
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------


#include "Xrd/XrdLink.hh"
#include "XrdHttpExtHandler.hh"
#include "XrdHttpReq.hh"
#include "XrdHttpProtocol.hh"
#include "XrdHttpUtils.hh"
#include "XrdOuc/XrdOucEnv.hh"

#include <cstring>

int XrdHttpExtReq::SendSimpleResp(int code, const char* desc, const char* header_to_add, const char* body, long long bodylen)
{
  if (!prot) return -1;

  return prot->SendSimpleResp(code, desc, header_to_add, body, bodylen, true);
}

int XrdHttpExtReq::StartSimpleResp(int code, const char *desc, const char *header_to_add, long long bodylen, bool keepalive)
{
  if (!prot) return -1;

  return prot->StartSimpleResp(code, desc, header_to_add, bodylen, true);
}

int XrdHttpExtReq::SendData(const char *body, int bodylen)
{
  if (!prot) return -1;

  return prot->SendData(body, bodylen);
}

int XrdHttpExtReq::StartChunkedResp(int code, const char *desc, const char *header_to_add)
{
  if (!prot) return -1;

  return prot->StartChunkedResp(code, desc, header_to_add, -1, true);
}

int XrdHttpExtReq::ChunkResp(const char *body, long long bodylen)
{
  if (!prot) return -1;

  return prot->ChunkResp(body, bodylen);
}

int XrdHttpExtReq::BuffgetData(int blen, char **data, bool wait) {

  if (!prot) return -1;
  int nb = prot->BuffgetData(blen, data, wait);
  
  return nb;
}

void XrdHttpExtReq::GetClientID(std::string &clid)
{
   char buff[512];
   prot->Link->Client(buff, sizeof(buff));
   clid = buff;
}

const XrdSecEntity &XrdHttpExtReq::GetSecEntity() const
{
  return prot->SecEntity;
}

bool XrdHttpExtReq::RunBridge(const void *request, const char *data,
                              int dataLength)
{
  if (!prot || !request || dataLength < 0 || (dataLength && !data))
    return false;

  XrdHttpReq &httpReq = prot->CurrentReq;
  if (httpReq.m_extBridgeState != XrdHttpReq::ExtBridgeState::None)
    return false;

  std::memcpy(&httpReq.m_extBridgeRequest, request,
              sizeof(httpReq.m_extBridgeRequest));
  if (dataLength)
    httpReq.m_extBridgePayload.assign(data, data + dataLength);
  else
    httpReq.m_extBridgePayload.clear();
  httpReq.m_extBridgeState = XrdHttpReq::ExtBridgeState::Queued;

  if (!prot->Bridge) {
    const char *name = prot->SecEntity.name ? prot->SecEntity.name : "unknown";
    prot->Bridge = XrdXrootd::Bridge::Login(
      &httpReq, prot->Link, &prot->SecEntity, name,
      prot->ishttps ? "https" : "http");
    if (!prot->Bridge) {
      httpReq.m_extBridgeState = XrdHttpReq::ExtBridgeState::None;
      httpReq.m_extBridgePayload.clear();
      return false;
    }
    if (prot->m_maxdelay > 0) prot->Bridge->SetWait(prot->m_maxdelay, false);
    prot->DoingLogin = true;
    return true;
  }

  return httpReq.DispatchExtBridge();
}

XrdHttpExtReq::BridgeResult XrdHttpExtReq::GetBridgeResult() const
{
  BridgeResult result;
  if (!prot) return result;

  const XrdHttpReq &httpReq = prot->CurrentReq;
  switch (httpReq.m_extBridgeState) {
    case XrdHttpReq::ExtBridgeState::Data:
      result.type = BridgeResult::Data;
      break;
    case XrdHttpReq::ExtBridgeState::Done:
      result.type = BridgeResult::Done;
      break;
    case XrdHttpReq::ExtBridgeState::Error:
      result.type = BridgeResult::Error;
      break;
    case XrdHttpReq::ExtBridgeState::Redirect:
      result.type = BridgeResult::Redirect;
      break;
    default:
      return result;
  }
  result.data = httpReq.m_extBridgeData;
  result.code = httpReq.m_extBridgeCode;
  result.httpStatus = httpReq.m_extBridgeHttpStatus;
  result.message = httpReq.m_extBridgeMessage;
  result.host = httpReq.m_extBridgeHost;
  result.port = httpReq.m_extBridgePort;
  return result;
}


XrdHttpExtReq::XrdHttpExtReq(XrdHttpReq *req, XrdHttpProtocol *pr): prot(pr),
verb(req->requestverb), headers(req->allheaders) {
  // Here we fill the request summary with all the fields we can
  resource = req->resource.c_str();
  int envlen = 0;
  
  const char *p = nullptr;
  if (req->opaque)
    p = req->opaque->Env(envlen);
  headers["xrd-http-query"] = p ? p:"";
  p = req->resourceplusopaque.c_str();
  headers["xrd-http-fullresource"] = p ? p:"";
  headers["xrd-http-prot"] = prot->isHTTPS()?"https":"http";
  
  // These fields usually identify the client that connected

  
  if (prot->SecEntity.moninfo) {
    clientdn = prot->SecEntity.moninfo;
    trim(clientdn);
  }
  if (prot->SecEntity.host) {
    clienthost = prot->SecEntity.host;
    trim(clienthost);
  }
  if (prot->SecEntity.vorg) {
    clientgroups = prot->SecEntity.vorg;
    trim(clientgroups);
  }

  // Get the packet marking handle and the client scitag from the XrdHttp layer
  pmark = prot->pmarkHandle;
  mSciTag = req->mScitag;
  mReprDigest = req->m_repr_digest;
  mWantReprDigest = req->m_want_repr_digest;
  tpcForwardCreds = prot->tpcForwardCreds;

  length = req->length;
}
