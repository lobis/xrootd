/******************************************************************************/
/*                                                                            */
/*                    X r d C l T a p e R e s t . c c                         */
/*                                                                            */
/* (c) 2026 by the XRootD Collaboration                                       */
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
/*                                                                            */
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with XRootD in a file called COPYING.LESSER (LGPL license) and file  */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/*                                                                            */
/******************************************************************************/

#include "XrdVersion.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClCurlUtil.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClUtils.hh"
#include "XrdCl/XrdClTapeRest.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdOuc/XrdOucJson.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#include <curl/curl.h>

namespace
{
using Json = nlohmann::json;

struct HttpResponse
{
  long statusCode = 0;
  std::string body;
  std::string error;
};

std::string ToLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
    [](unsigned char c) { return std::tolower(c); });
  return value;
}

std::string TrimCopy(std::string value)
{
  XrdCl::Utils::Trim(value);
  return value;
}

std::string GetEnvString(const std::string &key,
                         const std::string &shellKey)
{
  XrdCl::Env *env = XrdCl::DefaultEnv::GetEnv();
  if(!env) return "";

  std::string value;
  if(!env->GetString(key, value) || value.empty())
  {
    env->ImportString(key, shellKey);
    env->GetString(key, value);
  }
  return value;
}

std::string CollapseSlashes(const std::string &path)
{
  std::string result;
  result.reserve(path.size());
  bool previousSlash = false;
  for(const char c : path)
  {
    const bool currentSlash = c == '/';
    if(currentSlash && previousSlash) continue;
    result += c;
    previousSlash = currentSlash;
  }
  return result.empty() ? "/" : result;
}

std::string JoinUrl(const std::string &base, const std::string &path)
{
  if(base.empty()) return path;
  if(path.empty()) return base;
  if(base.back() == '/' && path.front() == '/') return base + path.substr(1);
  if(base.back() != '/' && path.front() != '/') return base + "/" + path;
  return base + path;
}

bool ParseVersion(const std::string &version, int &parsed)
{
  std::string value = ToLower(version);
  if(!value.empty() && value.front() == 'v') value.erase(value.begin());
  if(value.empty() || !std::isdigit(static_cast<unsigned char>(value.front())))
  {
    return false;
  }

  char *end = nullptr;
  const long number = std::strtol(value.c_str(), &end, 10);
  if(end == value.c_str()) return false;
  parsed = static_cast<int>(number);
  return true;
}

bool UrlEndpointAndPath(const std::string &input, std::string &endpoint,
                        std::string &path, std::string &error)
{
  XrdCl::URL url(input);
  if(!url.IsValid() || url.GetHostName().empty())
  {
    error = "invalid URL '" + input + "'";
    return false;
  }

  std::string protocol = ToLower(url.GetProtocol());
  bool useUrlPort = true;
  if(protocol == "davs") protocol = "https";
  else if(protocol == "dav") protocol = "http";
  else if(protocol == "root" || protocol == "xroot")
  {
    protocol = "https";
    useUrlPort = false;
  }

  if(protocol != "http" && protocol != "https")
  {
    error = "unsupported URL protocol '" + url.GetProtocol()
      + "' for Tape REST API";
    return false;
  }

  std::ostringstream out;
  out << protocol << "://" << url.GetHostName();
  if(useUrlPort && url.GetPort() > 0) out << ":" << url.GetPort();
  endpoint = out.str();
  path = CollapseSlashes(url.GetPath());
  return true;
}

std::string ReadBearerToken()
{
  std::string token = GetEnvString("BearerToken", "BEARER_TOKEN");
  if(!token.empty()) return TrimCopy(token);

  const std::string tokenFile =
    GetEnvString("BearerTokenFile", "BEARER_TOKEN_FILE");
  if(tokenFile.empty()) return "";

  std::ifstream in(tokenFile);
  if(!in) return "";

  std::string value;
  std::getline(in, value);
  return TrimCopy(value);
}

size_t CurlWriteCallback(char *data, size_t size, size_t nmemb, void *userp)
{
  const size_t bytes = size * nmemb;
  auto *output = static_cast<std::string *>(userp);
  output->append(data, bytes);
  return bytes;
}

HttpResponse HttpRequest(const std::string &method, const std::string &url,
                         const std::string &body,
                         const XrdCl::TapeRestOptions &options)
{
  HttpResponse response;
  XrdCl::Env *env = XrdCl::DefaultEnv::GetEnv();
  CURL *curl = XrdCl::CurlUtil::CreateCurlHandle(
    "XrdCl/" XrdVERSION, options.verbosity >= 3, env);
  if(!curl)
  {
    response.error = "unable to initialize curl";
    return response;
  }

  char errorBuffer[CURL_ERROR_SIZE];
  errorBuffer[0] = '\0';

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");

  const std::string token = ReadBearerToken();
  if(!token.empty())
  {
    const std::string header = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, header.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  if(options.timeout >= 0)
  {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.timeout);
  }

  XrdCl::CurlUtil::X509Credentials credentials;
  if(!options.cert.empty())
  {
    credentials.cert = options.cert;
    credentials.key = options.key.empty() ? options.cert : options.key;
  }
  else if(XrdCl::CurlUtil::UseClientX509(env))
  {
    credentials = XrdCl::CurlUtil::GetClientX509Credentials(env);
  }
  XrdCl::CurlUtil::ApplyClientX509Credentials(
    curl, credentials, XrdCl::DefaultEnv::GetLog(), XrdCl::TlsMsg);

  if(method == "POST")
  {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
      static_cast<long>(body.size()));
  }
  else if(method == "DELETE")
  {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  }

  const CURLcode result = curl_easy_perform(curl);
  if(result != CURLE_OK)
  {
    response.error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(result);
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

std::string FormatProblemResponse(long statusCode, const std::string &body)
{
  std::ostringstream out;
  out << "HTTP " << statusCode;
  if(body.empty()) return out.str();

  try
  {
    const Json json = Json::parse(body);
    if(json.is_object())
    {
      const auto title = json.find("title");
      const auto detail = json.find("detail");

      if(title != json.end() && title->is_string())
      {
        out << ": " << title->get<std::string>();
        if(detail != json.end() && detail->is_string())
        {
          out << " - " << detail->get<std::string>();
        }
        return out.str();
      }
    }
  }
  catch(const std::exception &)
  {
  }

  out << ": " << body;
  return out.str();
}

XrdCl::XRootDStatus ErrorStatus(uint16_t code, const std::string &message)
{
  return XrdCl::XRootDStatus(XrdCl::stError, code, 0, message);
}

bool PathFromInput(const std::string &input, std::string &path,
                   std::string &error)
{
  if(input.empty())
  {
    error = "empty path";
    return false;
  }
  if(input.front() == '/')
  {
    path = CollapseSlashes(input);
    return true;
  }

  std::string endpoint;
  return UrlEndpointAndPath(input, endpoint, path, error);
}

XrdCl::XRootDStatus DiscoverEndpoint(const XrdCl::TapeRestClient &client,
                                     const std::string &url,
                                     XrdCl::TapeRestEndpoint &endpoint)
{
  if(url.empty())
  {
    return ErrorStatus(XrdCl::errInvalidArgs, "missing URL");
  }
  return client.Discover(url, endpoint);
}

Json PathsRequestBody(const std::vector<std::string> &inputs)
{
  Json body;
  body["paths"] = Json::array();
  for(const auto &input : inputs)
  {
    std::string path;
    std::string error;
    if(PathFromInput(input, path, error)) body["paths"].push_back(path);
  }
  return body;
}

XrdCl::XRootDStatus PathsFromInputs(const std::vector<std::string> &inputs,
                                    std::vector<std::string> &paths)
{
  paths.clear();
  paths.reserve(inputs.size());
  for(const auto &input : inputs)
  {
    std::string path;
    std::string error;
    if(!PathFromInput(input, path, error))
    {
      return ErrorStatus(XrdCl::errInvalidArgs, error);
    }
    paths.push_back(path);
  }
  return XrdCl::XRootDStatus();
}

Json StageRequestBody(const std::vector<XrdCl::TapeRestStageFile> &files,
                      std::string &error)
{
  Json body;
  body["files"] = Json::array();
  for(const auto &file : files)
  {
    const std::string input = file.path.empty() ? file.url : file.path;
    if(input.empty())
    {
      error = "stage file is missing both URL and path";
      return Json();
    }

    std::string path;
    if(!PathFromInput(input, path, error)) return Json();

    Json item;
    item["path"] = path;
    if(!file.diskLifetime.empty()) item["diskLifetime"] = file.diskLifetime;
    if(!file.targetedMetadata.empty())
    {
      Json metadata;
      try
      {
        metadata = Json::parse(file.targetedMetadata);
      }
      catch(const std::exception &ex)
      {
        error = "invalid targetedMetadata JSON: " + std::string(ex.what());
        return Json();
      }
      if(!metadata.is_object())
      {
        error = "targetedMetadata must be a JSON object";
        return Json();
      }
      item["targetedMetadata"] = metadata;
    }
    body["files"].push_back(item);
  }
  return body;
}

bool JsonUInt64(const Json &json, const char *key, std::uint64_t &value,
                bool &hasValue)
{
  hasValue = false;
  const auto it = json.find(key);
  if(it == json.end()) return true;
  if(it->is_number_unsigned())
  {
    value = it->get<std::uint64_t>();
  }
  else if(it->is_number_integer() && it->get<std::int64_t>() >= 0)
  {
    value = static_cast<std::uint64_t>(it->get<std::int64_t>());
  }
  else
  {
    return false;
  }
  hasValue = true;
  return true;
}

XrdCl::XRootDStatus StageFileStatusFromJson(
  const Json &json, XrdCl::TapeRestStageFileStatus &result)
{
  result = XrdCl::TapeRestStageFileStatus();
  if(json.contains("path") && json["path"].is_string())
  {
    result.path = CollapseSlashes(json["path"].get<std::string>());
  }
  else
  {
    return ErrorStatus(XrdCl::errInvalidResponse,
      "stage request response contains a file entry without a string path");
  }
  if(json.contains("onDisk") && json["onDisk"].is_boolean())
  {
    result.onDisk = json["onDisk"].get<bool>();
    result.hasOnDisk = true;
  }
  if(json.contains("state") && json["state"].is_string())
  {
    result.state = json["state"].get<std::string>();
  }
  if(json.contains("error") && json["error"].is_string())
  {
    result.error = json["error"].get<std::string>();
  }
  if(!JsonUInt64(json, "startedAt", result.startedAt, result.hasStartedAt)
     || !JsonUInt64(json, "finishedAt", result.finishedAt,
                    result.hasFinishedAt))
  {
    return ErrorStatus(XrdCl::errInvalidResponse,
      "stage request response contains a non-integer file timestamp");
  }
  return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus StageStatusFromJson(const Json &json,
                                        XrdCl::TapeRestStageStatus &status)
{
  status = XrdCl::TapeRestStageStatus();
  if(!json.is_object())
  {
    return ErrorStatus(XrdCl::errInvalidResponse,
      "stage request response is not a JSON object");
  }
  if(!json.contains("id") || !json["id"].is_string())
  {
    return ErrorStatus(XrdCl::errInvalidResponse,
      "stage request response does not contain a string id");
  }
  if(!json.contains("files") || !json["files"].is_array())
  {
    return ErrorStatus(XrdCl::errInvalidResponse,
      "stage request response does not contain a files array");
  }

  status.id = json["id"].get<std::string>();
  if(!JsonUInt64(json, "createdAt", status.createdAt, status.hasCreatedAt)
     || !JsonUInt64(json, "startedAt", status.startedAt, status.hasStartedAt)
     || !JsonUInt64(json, "completedAt", status.completedAt,
                    status.hasCompletedAt))
  {
    return ErrorStatus(XrdCl::errInvalidResponse,
      "stage request response contains a non-integer timestamp");
  }

  status.files.reserve(json["files"].size());
  for(const auto &file : json["files"])
  {
    if(!file.is_object())
    {
      return ErrorStatus(XrdCl::errInvalidResponse,
        "stage request response contains a non-object file entry");
    }
    XrdCl::TapeRestStageFileStatus fileStatus;
    XrdCl::XRootDStatus fileStatusResult =
      StageFileStatusFromJson(file, fileStatus);
    if(!fileStatusResult.IsOK()) return fileStatusResult;
    status.files.push_back(fileStatus);
  }
  return XrdCl::XRootDStatus();
}

XrdCl::XRootDStatus EmptyResponseStatus(const HttpResponse &response,
                                        const std::string &operation)
{
  if(!response.error.empty())
  {
    return ErrorStatus(XrdCl::errConnectionError,
      operation + " failed: " + response.error);
  }
  if(response.statusCode != 200 && response.statusCode != 204)
  {
    return ErrorStatus(XrdCl::errErrorResponse,
      operation + " failed: "
      + FormatProblemResponse(response.statusCode, response.body));
  }
  return XrdCl::XRootDStatus();
}

Json ArchiveInfoRequestBody(const std::vector<std::string> &paths)
{
  Json body;
  body["paths"] = Json::array();
  for(const auto &path : paths)
  {
    body["paths"].push_back(path);
  }
  return body;
}

const Json *FindArchiveInfoItem(const Json &response, const std::string &path)
{
  if(!response.is_array()) return nullptr;

  for(const auto &item : response)
  {
    if(!item.is_object() || !item.contains("path") || !item["path"].is_string())
    {
      continue;
    }
    if(CollapseSlashes(item["path"].get<std::string>()) == path)
    {
      return &item;
    }
  }
  return nullptr;
}

XrdCl::TapeRestArchiveInfo ArchiveInfoFromJson(const Json *item,
                                               const std::string &url,
                                               const std::string &path)
{
  XrdCl::TapeRestArchiveInfo result;
  result.url = url;
  result.path = path;

  if(!item)
  {
    result.error = "missing response item for path=" + path;
    return result;
  }

  if(item->contains("error"))
  {
    if((*item)["error"].is_string())
    {
      result.error = (*item)["error"].get<std::string>();
    }
    else
    {
      result.error = "error field is not a string";
    }
    return result;
  }

  if(!item->contains("locality") || !(*item)["locality"].is_string())
  {
    result.error = "locality attribute missing";
    return result;
  }

  const std::string locality = (*item)["locality"].get<std::string>();
  result.locality = XrdCl::TapeRestClient::LocalityFromString(locality);
  if(result.locality == XrdCl::TapeRestLocality::Unknown)
  {
    result.error = "file locality reported as \"" + locality + "\"";
  }
  return result;
}

}

namespace XrdCl
{
  TapeRestClient::TapeRestClient( const TapeRestOptions &options ):
    pOptions( options )
  {
    XrdCl::Log *log = XrdCl::DefaultEnv::GetLog();
    if(log)
    {
      XrdCl::CurlUtil::ConfigureX509Env(
        XrdCl::DefaultEnv::GetEnv(), *log, XrdCl::TlsMsg);
    }
  }

  TapeRestClient::~TapeRestClient()
  {
  }

  XRootDStatus TapeRestClient::Discover( const std::string &url,
                                         TapeRestEndpoint &endpoint ) const
  {
    std::string storageEndpoint;
    std::string path;
    std::string error;
    if(!UrlEndpointAndPath(url, storageEndpoint, path, error))
    {
      return ErrorStatus(errInvalidArgs, error);
    }

    const std::string discoveryUrl =
      JoinUrl(storageEndpoint, "/.well-known/wlcg-tape-rest-api");
    const HttpResponse response =
      HttpRequest("GET", discoveryUrl, "", pOptions);

    if(!response.error.empty())
    {
      return ErrorStatus(errConnectionError,
        "failed to query " + discoveryUrl + ": " + response.error);
    }

    if(response.statusCode != 200)
    {
      return ErrorStatus(errErrorResponse,
        "failed to query " + discoveryUrl + ": "
        + FormatProblemResponse(response.statusCode, response.body));
    }

    Json json;
    try
    {
      json = Json::parse(response.body);
    }
    catch(const std::exception &ex)
    {
      return ErrorStatus(errInvalidResponse,
        "malformed discovery response: " + std::string(ex.what()));
    }

    if(!json.contains("sitename") || !json["sitename"].is_string())
    {
      return ErrorStatus(errInvalidResponse,
        "discovery response does not contain a string sitename");
    }
    if(!json.contains("endpoints") || !json["endpoints"].is_array())
    {
      return ErrorStatus(errInvalidResponse,
        "discovery response does not contain an endpoints array");
    }

    TapeRestEndpoint selected;
    int selectedVersion = -1;
    for(const auto &candidate : json["endpoints"])
    {
      if(!candidate.contains("uri") || !candidate["uri"].is_string()
         || !candidate.contains("version")
         || !candidate["version"].is_string())
      {
        continue;
      }

      int parsedVersion = -1;
      const auto version = candidate["version"].get<std::string>();
      if(!ParseVersion(version, parsedVersion)) continue;
      if(parsedVersion > 1 || parsedVersion < selectedVersion) continue;

      selectedVersion = parsedVersion;
      selected.uri = candidate["uri"].get<std::string>();
      selected.version = version;
    }

    if(selected.uri.empty())
    {
      return ErrorStatus(errNotSupported,
        "discovery response does not advertise a supported v0/v1 endpoint");
    }

    selected.sitename = json["sitename"].get<std::string>();
    endpoint = selected;
    return XRootDStatus();
  }

  XRootDStatus TapeRestClient::Stage(
    const std::string &url, const std::vector<TapeRestStageFile> &files,
    TapeRestStageResponse &stageResponse ) const
  {
    stageResponse = TapeRestStageResponse();
    if(files.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing stage files");
    }

    TapeRestEndpoint endpoint;
    XRootDStatus status = DiscoverEndpoint(*this, url, endpoint);
    if(!status.IsOK()) return status;

    std::string error;
    const Json body = StageRequestBody(files, error);
    if(!error.empty()) return ErrorStatus(errInvalidArgs, error);

    const std::string stageUrl = JoinUrl(endpoint.uri, "/stage");
    const HttpResponse response =
      HttpRequest("POST", stageUrl, body.dump(), pOptions);

    if(!response.error.empty())
    {
      return ErrorStatus(errConnectionError,
        "stage request submission failed: " + response.error);
    }

    if(response.statusCode != 201)
    {
      return ErrorStatus(errErrorResponse,
        "stage request submission failed: "
        + FormatProblemResponse(response.statusCode, response.body));
    }

    Json json;
    try
    {
      json = Json::parse(response.body);
    }
    catch(const std::exception &ex)
    {
      return ErrorStatus(errInvalidResponse,
        "malformed stage submission response: " + std::string(ex.what()));
    }

    if(!json.contains("requestId") || !json["requestId"].is_string())
    {
      return ErrorStatus(errInvalidResponse,
        "stage submission response does not contain a string requestId");
    }

    stageResponse.requestId = json["requestId"].get<std::string>();
    return XRootDStatus();
  }

  XRootDStatus TapeRestClient::StageStatus(
    const std::string &url, const std::string &requestId,
    TapeRestStageStatus &stageStatus ) const
  {
    stageStatus = TapeRestStageStatus();
    if(requestId.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing stage request id");
    }

    TapeRestEndpoint endpoint;
    XRootDStatus status = DiscoverEndpoint(*this, url, endpoint);
    if(!status.IsOK()) return status;

    const std::string stageUrl = JoinUrl(endpoint.uri, "/stage/" + requestId);
    const HttpResponse response = HttpRequest("GET", stageUrl, "", pOptions);

    if(!response.error.empty())
    {
      return ErrorStatus(errConnectionError,
        "stage request polling failed: " + response.error);
    }

    if(response.statusCode != 200)
    {
      return ErrorStatus(errErrorResponse,
        "stage request polling failed: "
        + FormatProblemResponse(response.statusCode, response.body));
    }

    Json json;
    try
    {
      json = Json::parse(response.body);
    }
    catch(const std::exception &ex)
    {
      return ErrorStatus(errInvalidResponse,
        "malformed stage polling response: " + std::string(ex.what()));
    }
    return StageStatusFromJson(json, stageStatus);
  }

  XRootDStatus TapeRestClient::StageCancel(
    const std::string &url, const std::string &requestId,
    const std::vector<std::string> &inputs ) const
  {
    if(requestId.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing stage request id");
    }
    if(inputs.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing paths to cancel");
    }

    TapeRestEndpoint endpoint;
    XRootDStatus status = DiscoverEndpoint(*this, url, endpoint);
    if(!status.IsOK()) return status;

    std::vector<std::string> paths;
    status = PathsFromInputs(inputs, paths);
    if(!status.IsOK()) return status;

    const std::string cancelUrl =
      JoinUrl(endpoint.uri, "/stage/" + requestId + "/cancel");
    const HttpResponse response = HttpRequest(
      "POST", cancelUrl, PathsRequestBody(paths).dump(), pOptions);
    return EmptyResponseStatus(response, "stage request cancellation");
  }

  XRootDStatus TapeRestClient::StageDelete(
    const std::string &url, const std::string &requestId ) const
  {
    if(requestId.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing stage request id");
    }

    TapeRestEndpoint endpoint;
    XRootDStatus status = DiscoverEndpoint(*this, url, endpoint);
    if(!status.IsOK()) return status;

    const std::string stageUrl = JoinUrl(endpoint.uri, "/stage/" + requestId);
    const HttpResponse response = HttpRequest("DELETE", stageUrl, "", pOptions);
    return EmptyResponseStatus(response, "stage request deletion");
  }

  XRootDStatus TapeRestClient::Release(
    const std::string &url, const std::string &requestId,
    const std::vector<std::string> &inputs ) const
  {
    if(requestId.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing stage request id");
    }
    if(inputs.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing paths to release");
    }

    TapeRestEndpoint endpoint;
    XRootDStatus status = DiscoverEndpoint(*this, url, endpoint);
    if(!status.IsOK()) return status;

    std::vector<std::string> paths;
    status = PathsFromInputs(inputs, paths);
    if(!status.IsOK()) return status;

    const std::string releaseUrl =
      JoinUrl(endpoint.uri, "/release/" + requestId);
    const HttpResponse response = HttpRequest(
      "POST", releaseUrl, PathsRequestBody(paths).dump(), pOptions);
    return EmptyResponseStatus(response, "stage request release");
  }

  XRootDStatus TapeRestClient::ArchiveInfo(
    const std::vector<std::string> &urls,
    std::vector<TapeRestArchiveInfo> &results ) const
  {
    results.clear();
    if(urls.empty())
    {
      return ErrorStatus(errInvalidArgs, "missing URL");
    }

    TapeRestEndpoint endpoint;
    XRootDStatus status = Discover(urls.front(), endpoint);
    if(!status.IsOK()) return status;

    std::vector<std::string> paths;
    paths.reserve(urls.size());
    for(const auto &url : urls)
    {
      std::string storageEndpoint;
      std::string path;
      std::string error;
      if(!UrlEndpointAndPath(url, storageEndpoint, path, error))
      {
        return ErrorStatus(errInvalidArgs, error);
      }
      paths.push_back(path);
    }

    const std::string archiveInfoUrl = JoinUrl(endpoint.uri, "/archiveinfo");
    const std::string requestBody = ArchiveInfoRequestBody(paths).dump();
    const HttpResponse response =
      HttpRequest("POST", archiveInfoUrl, requestBody, pOptions);

    if(!response.error.empty())
    {
      return ErrorStatus(errConnectionError,
        "archive polling call failed: " + response.error);
    }

    if(response.statusCode != 200)
    {
      return ErrorStatus(errErrorResponse,
        "archive polling call failed: "
        + FormatProblemResponse(response.statusCode, response.body));
    }

    Json json;
    try
    {
      json = Json::parse(response.body);
    }
    catch(const std::exception &ex)
    {
      return ErrorStatus(errInvalidResponse,
        "malformed archiveinfo response: " + std::string(ex.what()));
    }

    if(!json.is_array())
    {
      return ErrorStatus(errInvalidResponse,
        "archiveinfo response is not a JSON array");
    }

    results.reserve(paths.size());
    for(std::size_t i = 0; i < paths.size(); ++i)
    {
      results.push_back(ArchiveInfoFromJson(
        FindArchiveInfoItem(json, paths[i]), urls[i], paths[i]));
    }
    return XRootDStatus();
  }

  std::string TapeRestClient::LocalityToString( TapeRestLocality locality )
  {
    switch(locality)
    {
      case TapeRestLocality::Disk: return "DISK";
      case TapeRestLocality::Tape: return "TAPE";
      case TapeRestLocality::DiskAndTape: return "DISK_AND_TAPE";
      case TapeRestLocality::Lost: return "LOST";
      case TapeRestLocality::None: return "NONE";
      case TapeRestLocality::Unavailable: return "UNAVAILABLE";
      case TapeRestLocality::Unknown: break;
    }
    return "UNKNOWN";
  }

  TapeRestLocality TapeRestClient::LocalityFromString(
    const std::string &locality )
  {
    const std::string value = ToLower(locality);
    if(value == "disk") return TapeRestLocality::Disk;
    if(value == "tape") return TapeRestLocality::Tape;
    if(value == "disk_and_tape") return TapeRestLocality::DiskAndTape;
    if(value == "lost") return TapeRestLocality::Lost;
    if(value == "none") return TapeRestLocality::None;
    if(value == "unavailable") return TapeRestLocality::Unavailable;
    return TapeRestLocality::Unknown;
  }
}
