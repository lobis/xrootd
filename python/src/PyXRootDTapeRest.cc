/******************************************************************************/
/*                                                                            */
/*                    P y X R o o t D T a p e R e s t . c c                   */
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

#include "PyXRootDTapeRest.hh"
#include "Conversions.hh"
#include "XrdCl/XrdClTapeRest.hh"

#include <cstdint>
#include <vector>

namespace
{
XrdCl::TapeRestOptions OptionsFromArgs(int timeout, const char *cert,
                                       const char *key, unsigned int verbosity)
{
  XrdCl::TapeRestOptions options;
  options.timeout = timeout;
  options.cert = cert ? cert : "";
  options.key = key ? key : "";
  options.verbosity = verbosity;
  return options;
}

PyObject *EndpointToDict(const XrdCl::TapeRestEndpoint &endpoint)
{
  return Py_BuildValue("{ssssss}",
    "uri", endpoint.uri.c_str(),
    "version", endpoint.version.c_str(),
    "sitename", endpoint.sitename.c_str());
}

PyObject *ArchiveInfoToDict(const XrdCl::TapeRestArchiveInfo &info)
{
  return Py_BuildValue("{ssssssss}",
    "url", info.url.c_str(),
    "path", info.path.c_str(),
    "locality", XrdCl::TapeRestClient::LocalityToString(info.locality).c_str(),
    "error", info.error.c_str());
}

void DictSetString(PyObject *dict, const char *key, const std::string &value)
{
  PyObject *pyvalue = PyUnicode_FromString(value.c_str());
  PyDict_SetItemString(dict, key, pyvalue);
  Py_DECREF(pyvalue);
}

void DictSetBool(PyObject *dict, const char *key, bool value)
{
  PyObject *pyvalue = PyBool_FromLong(value ? 1 : 0);
  PyDict_SetItemString(dict, key, pyvalue);
  Py_DECREF(pyvalue);
}

void DictSetUInt64(PyObject *dict, const char *key, std::uint64_t value)
{
  PyObject *pyvalue = PyLong_FromUnsignedLongLong(value);
  PyDict_SetItemString(dict, key, pyvalue);
  Py_DECREF(pyvalue);
}

PyObject *StageResponseToDict(const XrdCl::TapeRestStageResponse &response)
{
  return Py_BuildValue("{ss}", "requestId", response.requestId.c_str());
}

PyObject *StageFileStatusToDict(
  const XrdCl::TapeRestStageFileStatus &status)
{
  PyObject *dict = PyDict_New();
  DictSetString(dict, "path", status.path);
  if(status.hasOnDisk) DictSetBool(dict, "onDisk", status.onDisk);
  if(!status.state.empty()) DictSetString(dict, "state", status.state);
  if(!status.error.empty()) DictSetString(dict, "error", status.error);
  if(status.hasStartedAt) DictSetUInt64(dict, "startedAt", status.startedAt);
  if(status.hasFinishedAt) DictSetUInt64(dict, "finishedAt", status.finishedAt);
  return dict;
}

PyObject *StageStatusToDict(const XrdCl::TapeRestStageStatus &status)
{
  PyObject *dict = PyDict_New();
  DictSetString(dict, "id", status.id);
  if(status.hasCreatedAt) DictSetUInt64(dict, "createdAt", status.createdAt);
  if(status.hasStartedAt) DictSetUInt64(dict, "startedAt", status.startedAt);
  if(status.hasCompletedAt)
  {
    DictSetUInt64(dict, "completedAt", status.completedAt);
  }

  PyObject *files = PyList_New(status.files.size());
  for(std::size_t i = 0; i < status.files.size(); ++i)
  {
    PyList_SET_ITEM(files, i, StageFileStatusToDict(status.files[i]));
  }
  PyDict_SetItemString(dict, "files", files);
  Py_DECREF(files);
  return dict;
}

bool PyStringValue(PyObject *object, std::string &value)
{
  if(object == Py_None)
  {
    value.clear();
    return true;
  }
  const char *text = PyUnicode_AsUTF8(object);
  if(!text) return false;
  value = text;
  return true;
}

bool SequenceToStageFiles(PyObject *sequence,
                          std::vector<XrdCl::TapeRestStageFile> &files)
{
  PyObject *fast = PySequence_Fast(sequence, "files must be a sequence");
  if(!fast) return false;

  const Py_ssize_t size = PySequence_Fast_GET_SIZE(fast);
  files.reserve(static_cast<std::size_t>(size));
  for(Py_ssize_t i = 0; i < size; ++i)
  {
    PyObject *item = PySequence_Fast_GET_ITEM(fast, i);
    XrdCl::TapeRestStageFile file;
    if(PyUnicode_Check(item))
    {
      if(!PyStringValue(item, file.url))
      {
        Py_DECREF(fast);
        return false;
      }
      files.push_back(file);
      continue;
    }

    PyObject *entry = PySequence_Fast(
      item, "stage file entries must be strings or normalized tuples");
    if(!entry)
    {
      Py_DECREF(fast);
      return false;
    }
    if(PySequence_Fast_GET_SIZE(entry) != 4)
    {
      PyErr_SetString(PyExc_ValueError,
        "stage file tuples must contain url, path, diskLifetime, targetedMetadata");
      Py_DECREF(entry);
      Py_DECREF(fast);
      return false;
    }

    if(!PyStringValue(PySequence_Fast_GET_ITEM(entry, 0), file.url)
       || !PyStringValue(PySequence_Fast_GET_ITEM(entry, 1), file.path)
       || !PyStringValue(PySequence_Fast_GET_ITEM(entry, 2), file.diskLifetime)
       || !PyStringValue(PySequence_Fast_GET_ITEM(entry, 3),
                         file.targetedMetadata))
    {
      Py_DECREF(entry);
      Py_DECREF(fast);
      return false;
    }

    Py_DECREF(entry);
    files.push_back(file);
  }

  Py_DECREF(fast);
  return true;
}

bool SequenceToUrls(PyObject *sequence, std::vector<std::string> &urls)
{
  PyObject *fast = PySequence_Fast(sequence, "urls must be a sequence");
  if(!fast) return false;

  const Py_ssize_t size = PySequence_Fast_GET_SIZE(fast);
  urls.reserve(static_cast<std::size_t>(size));
  for(Py_ssize_t i = 0; i < size; ++i)
  {
    PyObject *item = PySequence_Fast_GET_ITEM(fast, i);
    const char *url = PyUnicode_AsUTF8(item);
    if(!url)
    {
      Py_DECREF(fast);
      return false;
    }
    urls.emplace_back(url);
  }

  Py_DECREF(fast);
  return true;
}

}

namespace PyXRootD
{
  PyObject* TapeRestDiscover_cpp( PyObject *self, PyObject *args )
  {
    char *url = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "s|issI", &url, &timeout, &cert, &key,
                         &verbosity))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    XrdCl::TapeRestEndpoint endpoint;
    XrdCl::XRootDStatus status = client.Discover(url, endpoint);

    PyObject *pystatus = ConvertType<XrdCl::XRootDStatus>(&status);
    PyObject *pyendpoint = status.IsOK() ? EndpointToDict(endpoint)
                                         : Py_BuildValue("");
    return Py_BuildValue("NN", pystatus, pyendpoint);
  }

  PyObject* TapeRestStage_cpp( PyObject *self, PyObject *args )
  {
    char *url = nullptr;
    PyObject *pyfiles = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "sO|issI", &url, &pyfiles, &timeout, &cert,
                         &key, &verbosity))
    {
      return nullptr;
    }

    std::vector<XrdCl::TapeRestStageFile> files;
    if(!SequenceToStageFiles(pyfiles, files))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    XrdCl::TapeRestStageResponse response;
    XrdCl::XRootDStatus status = client.Stage(url, files, response);

    PyObject *pystatus = ConvertType<XrdCl::XRootDStatus>(&status);
    PyObject *pyresponse = status.IsOK() ? StageResponseToDict(response)
                                         : Py_BuildValue("");
    return Py_BuildValue("NN", pystatus, pyresponse);
  }

  PyObject* TapeRestStageStatus_cpp( PyObject *self, PyObject *args )
  {
    char *url = nullptr;
    char *requestId = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "ss|issI", &url, &requestId, &timeout, &cert,
                         &key, &verbosity))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    XrdCl::TapeRestStageStatus response;
    XrdCl::XRootDStatus status = client.StageStatus(url, requestId, response);

    PyObject *pystatus = ConvertType<XrdCl::XRootDStatus>(&status);
    PyObject *pyresponse = status.IsOK() ? StageStatusToDict(response)
                                         : Py_BuildValue("");
    return Py_BuildValue("NN", pystatus, pyresponse);
  }

  PyObject* TapeRestStageCancel_cpp( PyObject *self, PyObject *args )
  {
    char *url = nullptr;
    char *requestId = nullptr;
    PyObject *pypaths = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "ssO|issI", &url, &requestId, &pypaths,
                         &timeout, &cert, &key, &verbosity))
    {
      return nullptr;
    }

    std::vector<std::string> paths;
    if(!SequenceToUrls(pypaths, paths))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    XrdCl::XRootDStatus status = client.StageCancel(url, requestId, paths);
    return ConvertType<XrdCl::XRootDStatus>(&status);
  }

  PyObject* TapeRestStageDelete_cpp( PyObject *self, PyObject *args )
  {
    char *url = nullptr;
    char *requestId = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "ss|issI", &url, &requestId, &timeout, &cert,
                         &key, &verbosity))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    XrdCl::XRootDStatus status = client.StageDelete(url, requestId);
    return ConvertType<XrdCl::XRootDStatus>(&status);
  }

  PyObject* TapeRestRelease_cpp( PyObject *self, PyObject *args )
  {
    char *url = nullptr;
    char *requestId = nullptr;
    PyObject *pypaths = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "ssO|issI", &url, &requestId, &pypaths,
                         &timeout, &cert, &key, &verbosity))
    {
      return nullptr;
    }

    std::vector<std::string> paths;
    if(!SequenceToUrls(pypaths, paths))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    XrdCl::XRootDStatus status = client.Release(url, requestId, paths);
    return ConvertType<XrdCl::XRootDStatus>(&status);
  }

  PyObject* TapeRestArchiveInfo_cpp( PyObject *self, PyObject *args )
  {
    PyObject *pyurls = nullptr;
    int timeout = -1;
    char *cert = const_cast<char *>("");
    char *key = const_cast<char *>("");
    unsigned int verbosity = 0;

    if(!PyArg_ParseTuple(args, "O|issI", &pyurls, &timeout, &cert, &key,
                         &verbosity))
    {
      return nullptr;
    }

    std::vector<std::string> urls;
    if(!SequenceToUrls(pyurls, urls))
    {
      return nullptr;
    }

    XrdCl::TapeRestClient client(
      OptionsFromArgs(timeout, cert, key, verbosity));
    std::vector<XrdCl::TapeRestArchiveInfo> results;
    XrdCl::XRootDStatus status = client.ArchiveInfo(urls, results);

    PyObject *pyresults = PyList_New(results.size());
    for(std::size_t i = 0; i < results.size(); ++i)
    {
      PyList_SET_ITEM(pyresults, i, ArchiveInfoToDict(results[i]));
    }

    PyObject *pystatus = ConvertType<XrdCl::XRootDStatus>(&status);
    return Py_BuildValue("NN", pystatus, pyresults);
  }
}
