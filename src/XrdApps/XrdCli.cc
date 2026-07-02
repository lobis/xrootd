/******************************************************************************/
/*                                                                            */
/*                         X r d C l i . c c                                  */
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
#include "XrdCl/XrdClBuffer.hh"
#include "XrdCl/XrdClCheckSumManager.hh"
#include "XrdCl/XrdClCopy.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdOuc/XrdOucJson.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCks/XrdCksCalc.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif

#include <CLI/CLI.hpp>
#include <zlib.h>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{
struct Command
{
  std::string_view name;
  std::string_view description;
};

struct StatOptions
{
  std::string path;
  int timeout = -1;
};

struct LsOptions
{
  std::string path;
  bool all = false;
  bool longFormat = false;
  bool directory = false;
  bool humanReadable = false;
  std::string timeStyle = "locale";
};

struct SumOptions
{
  std::string path;
  std::string checkSumType;
  int timeout = -1;
};

struct CatOptions
{
  std::vector<std::string> paths;
};

struct XAttrOptions
{
  std::string path;
  std::string attribute;
};

struct ArchivePollOptions
{
  std::vector<std::string> urls;
  int timeout = -1;
  int pollingTimeout = 0;
};

enum class ArchivePollState
{
  Ready,
  Queued,
  Failed
};

constexpr Command kCommands[] = {
  {"archivepoll", "Perform an archive polling operation on the given URL"},
  {"bringonline", "Perform a staging operation on the given URL"},
  {"cat", "Concatenate a file and print it on the standard output"},
  {"chmod", "Change file permissions"},
  {"copy", "Copy files"},
  {"evict", "Evict a file from a disk buffer"},
  {"ls", "List directory contents or file information"},
  {"mkdir", "Make directories"},
  {"rename", "Rename files or directories"},
  {"rm", "Remove files or directories"},
  {"save", "Read from standard input and write to a file"},
  {"stat", "Display extended information about a file or directory"},
  {"sum", "Calculate a file checksum"},
  {"token", "Retrieve an SE-issued token for a path"},
  {"xattr", "Show or set file attributes"},
};

int NotImplemented(std::string_view name)
{
  std::cerr << "xrd " << name
            << ": command is not implemented yet\n";
  return 2;
}

bool IsCopyCommand(const char *command)
{
  return command && std::string_view(command) == "copy";
}

int RunCopyCommand(int argc, char **argv)
{
  std::string programName = "xrd copy";
  std::vector<char *> copyArgs;
  copyArgs.reserve(static_cast<std::size_t>(argc) + 1);
  copyArgs.push_back(programName.data());
  for(int i = 2; i < argc; ++i)
  {
    copyArgs.push_back(argv[i]);
  }
  copyArgs.push_back(nullptr);

#ifndef _WIN32
  optind = 1;
#endif

  return XrdCl::RunXrdCp(static_cast<int>(copyArgs.size() - 1),
                         copyArgs.data());
}

std::string ToLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
    [](unsigned char c) { return std::tolower(c); });
  return value;
}

void SetEnvironment(const char *name, const std::string &value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

bool HasScheme(const std::string &path)
{
  return path.find("://") != std::string::npos;
}

std::string Trim(const std::string &value)
{
  const auto begin = std::find_if_not(value.begin(), value.end(),
    [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(value.rbegin(), value.rend(),
    [](unsigned char c) { return std::isspace(c); }).base();
  if(begin >= end) return "";
  return std::string(begin, end);
}

std::string LocalDisplayPath(const std::string &localPath)
{
  char resolved[PATH_MAX];
  if(::realpath(localPath.c_str(), resolved) != nullptr)
  {
    return std::string("file://") + resolved;
  }
  return std::string("file://") + localPath;
}

std::string FileType(const XrdCl::StatInfo &info)
{
  if(info.TestFlags(XrdCl::StatInfo::IsDir)) return "directory";
  if(info.TestFlags(XrdCl::StatInfo::Other)) return "unknown";
  return "regular file";
}

char FileTypeChar(const XrdCl::StatInfo &info)
{
  return info.TestFlags(XrdCl::StatInfo::IsDir) ? 'd' : '-';
}

std::string FileType(mode_t mode)
{
  if(S_ISBLK(mode)) return "block device";
  if(S_ISCHR(mode)) return "character device";
  if(S_ISDIR(mode)) return "directory";
  if(S_ISFIFO(mode)) return "fifo";
  if(S_ISLNK(mode)) return "symbolic link";
  if(S_ISREG(mode)) return "regular file";
  if(S_ISSOCK(mode)) return "socket";
  return "unknown";
}

char FileTypeChar(mode_t mode)
{
  if(S_ISBLK(mode)) return 'b';
  if(S_ISCHR(mode)) return 'c';
  if(S_ISDIR(mode)) return 'd';
  if(S_ISFIFO(mode)) return 'p';
  if(S_ISLNK(mode)) return 'l';
  if(S_ISSOCK(mode)) return 's';
  return '-';
}

std::string ModeTriplet(mode_t mode, mode_t read, mode_t write, mode_t execute)
{
  std::string result;
  result += (mode & read) ? "r" : "-";
  result += (mode & write) ? "w" : "-";
  result += (mode & execute) ? "x" : "-";
  return result;
}

std::string ModeString(mode_t mode)
{
  std::string result;
  result += FileTypeChar(mode);
  result += ModeTriplet(mode, S_IRUSR, S_IWUSR, S_IXUSR);
  result += ModeTriplet(mode, S_IRGRP, S_IWGRP, S_IXGRP);
  result += ModeTriplet(mode, S_IROTH, S_IWOTH, S_IXOTH);
  return result;
}

std::string ModeOctal(mode_t mode)
{
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << std::oct
      << (mode & 07777);
  return out.str();
}

std::string BasicModeString(const XrdCl::StatInfo &info)
{
  std::string mode;
  mode += FileTypeChar(info);
  mode += info.TestFlags(XrdCl::StatInfo::IsReadable) ? "r" : "-";
  mode += info.TestFlags(XrdCl::StatInfo::IsWritable) ? "w" : "-";
  mode += info.TestFlags(XrdCl::StatInfo::XBitSet) ? "x" : "-";
  mode += "------";
  return mode;
}

std::string ModeString(const XrdCl::StatInfo &info)
{
  if(info.ExtendedFormat())
  {
    return std::string(1, FileTypeChar(info)) + info.GetModeAsOctString();
  }
  return BasicModeString(info);
}

std::string ModeOctal(const XrdCl::StatInfo &info)
{
  if(info.ExtendedFormat()) return info.GetModeAsString();

  unsigned int mode = 0;
  if(info.TestFlags(XrdCl::StatInfo::IsReadable)) mode |= 0400;
  if(info.TestFlags(XrdCl::StatInfo::IsWritable)) mode |= 0200;
  if(info.TestFlags(XrdCl::StatInfo::XBitSet)) mode |= 0100;

  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << std::oct << mode;
  return out.str();
}

std::string FormatTimestamp(uint64_t timestamp)
{
  std::time_t seconds = static_cast<std::time_t>(timestamp);
  std::tm tm;

#ifdef _WIN32
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%F %T", &tm);
  return std::string(buffer) + ".000000";
}

XrdCl::URL FileSystemURL(XrdCl::URL url)
{
  url.SetPath("");
  url.SetParams(XrdCl::URL::ParamsMap{});
  return url;
}

// One entry of the JSON array returned by the XrdClHttp Tape REST
// "tape.archiveinfo" opaque query.
struct ArchivePollResult
{
  std::string url;
  std::string path;
  std::string locality;
  std::string error;
};

ArchivePollState ArchivePollStateFromLocality(const std::string &locality)
{
  if(locality == "TAPE" || locality == "DISK_AND_TAPE")
  {
    return ArchivePollState::Ready;
  }
  if(locality == "DISK" || locality == "UNAVAILABLE")
  {
    return ArchivePollState::Queued;
  }
  return ArchivePollState::Failed;
}

std::string ArchivePollFailureMessage(const ArchivePollResult &result)
{
  if(!result.error.empty())
  {
    return "[Tape REST API] " + result.error;
  }

  return "[Tape REST API] File locality reported as " + result.locality
    + " (path=" + result.path + ")";
}

int PrintArchivePollResults(
  const std::vector<ArchivePollResult> &results)
{
  int terminal = 0;
  for(const auto &result : results)
  {
    const auto state = ArchivePollStateFromLocality(result.locality);
    if(state == ArchivePollState::Ready)
    {
      ++terminal;
      std::cout << result.url << " READY\n";
    }
    else if(state == ArchivePollState::Queued)
    {
      std::cout << result.url << " QUEUED\n";
    }
    else
    {
      ++terminal;
      std::cout << result.url << " => FAILED: "
                << ArchivePollFailureMessage(result) << '\n';
    }
  }
  return terminal;
}

int RunLocalStat(const std::string &originalPath, const std::string &localPath)
{
  struct stat statBuffer;
  if(::stat(localPath.c_str(), &statBuffer) != 0)
  {
    const int err = errno;
    std::cerr << "xrd stat error: " << err << " (" << std::strerror(err)
              << ") - errno reported by local system call "
              << std::strerror(err) << '\n';
    return err;
  }

  std::cout << "  File: '" << LocalDisplayPath(localPath) << "'\n";
  std::cout << "  Size: " << statBuffer.st_size << '\t'
            << FileType(statBuffer.st_mode) << '\n';
  std::cout << "Access: (" << ModeOctal(statBuffer.st_mode) << "/"
            << ModeString(statBuffer.st_mode) << ")\tUid: "
            << statBuffer.st_uid << "\tGid: " << statBuffer.st_gid << "\t\n";
  std::cout << "Access: " << FormatTimestamp(statBuffer.st_atime) << '\n';
  std::cout << "Modify: " << FormatTimestamp(statBuffer.st_mtime) << '\n';
  std::cout << "Change: " << FormatTimestamp(statBuffer.st_ctime) << '\n';
  return 0;
}

int RunRemoteStat(const std::string &path)
{
  XrdCl::URL url(path);
  if(!url.IsValid())
  {
    std::cerr << "xrd stat: invalid URL '" << path << "'\n";
    return 64;
  }

  if(url.IsLocalFile())
  {
    return RunLocalStat(path, url.GetPath());
  }

  XrdCl::FileSystem fs(FileSystemURL(url));
  XrdCl::StatInfo *rawInfo = nullptr;
  XrdCl::XRootDStatus status = fs.Stat(url.GetPathWithParams(), rawInfo);
  std::unique_ptr<XrdCl::StatInfo> info(rawInfo);

  if(!status.IsOK())
  {
    std::cerr << "xrd stat: unable to stat '" << path
              << "': " << status.ToStr() << '\n';
    return status.GetShellCode();
  }

  std::cout << "  File: '" << path << "'\n";
  std::cout << "  Size: " << info->GetSize() << '\t' << FileType(*info) << '\n';
  std::cout << "Access: (" << ModeOctal(*info) << "/" << ModeString(*info)
            << ")\tUid: " << info->GetOwner()
            << "\tGid: " << info->GetGroup() << "\t\n";

  const auto accessTime = info->ExtendedFormat()
    ? info->GetAccessTime()
    : info->GetModTime();
  const auto changeTime = info->ExtendedFormat()
    ? info->GetChangeTime()
    : info->GetModTime();

  std::cout << "Access: " << FormatTimestamp(accessTime) << '\n';
  std::cout << "Modify: " << FormatTimestamp(info->GetModTime()) << '\n';
  std::cout << "Change: " << FormatTimestamp(changeTime) << '\n';
  return 0;
}

std::string CheckSumQueryPath(const XrdCl::URL &url,
                              const std::string &checkSumType)
{
  const auto path = url.GetPathWithParams();
  return path + (path.find('?') == std::string::npos ? '?' : '&')
    + "cks.type=" + checkSumType;
}

bool ParseCheckSumResponse(const std::string &response,
                           std::string &checkSumType,
                           std::string &checkSumValue)
{
  std::istringstream in(response);
  std::string extra;
  if(!(in >> checkSumType >> checkSumValue)) return false;
  return !(in >> extra);
}

bool IsHexValue(const std::string &value)
{
  return !value.empty()
    && std::all_of(value.begin(), value.end(), [](unsigned char c) {
         return std::isxdigit(c);
       });
}

std::string FormatCheckSumValue(const std::string &checkSumType,
                                const std::string &checkSumValue)
{
  if(checkSumType == "crc32" && IsHexValue(checkSumValue)
     && checkSumValue.size() <= 8)
  {
    return std::to_string(std::stoul(checkSumValue, nullptr, 16));
  }
  return checkSumValue;
}

int CalculateLocalCrc32(const std::string &localPath, uLong &checkSum)
{
  std::FILE *file = std::fopen(localPath.c_str(), "rb");
  if(!file) return errno;

  checkSum = crc32(0L, Z_NULL, 0);
  char buffer[64 * 1024];
  while(true)
  {
    const auto bytesRead = std::fread(buffer, 1, sizeof(buffer), file);
    if(bytesRead > 0)
    {
      checkSum = crc32(checkSum,
        reinterpret_cast<const Bytef *>(buffer), bytesRead);
    }
    if(bytesRead < sizeof(buffer))
    {
      if(std::ferror(file))
      {
        const int err = errno;
        std::fclose(file);
        return err ? err : EIO;
      }
      break;
    }
  }

  std::fclose(file);
  return 0;
}

int RunLocalSum(const std::string &localPath, const std::string &checkSumType,
                const std::string &requestedCheckSumType)
{
  struct stat statBuffer;
  if(::stat(localPath.c_str(), &statBuffer) != 0)
  {
    const int err = errno;
    std::cerr << "xrd sum error: " << err << " (" << std::strerror(err)
              << ") - errno reported by local system call "
              << std::strerror(err) << '\n';
    return err;
  }

  if(checkSumType == "crc32")
  {
    uLong crcValue = 0;
    const int err = CalculateLocalCrc32(localPath, crcValue);
    if(err != 0)
    {
      std::cerr << "xrd sum error: " << err << " (" << std::strerror(err)
                << ") - errno reported by local system call "
                << std::strerror(err) << '\n';
      return err;
    }
    std::cout << LocalDisplayPath(localPath) << ' ' << crcValue << '\n';
    return 0;
  }

  XrdCl::CheckSumManager *checkSumManager =
    XrdCl::DefaultEnv::GetCheckSumManager();
  if(!checkSumManager)
  {
    std::cerr << "xrd sum error: unable to initialize checksum processing\n";
    return 13;
  }

  std::unique_ptr<XrdCksCalc> calculator(
    checkSumManager->GetCalculator(checkSumType));
  if(!calculator)
  {
    std::cerr << "xrd sum error: 38 (Function not implemented) - "
              << "Checksum type " << requestedCheckSumType
              << " not supported for local files\n";
    return 38;
  }

  XrdCksData checkSum;
  checkSum.Set(checkSumType.c_str());
  errno = 0;
  if(!checkSumManager->Calculate(checkSum, checkSumType, localPath))
  {
    if(errno != 0)
    {
      const int err = errno;
      std::cerr << "xrd sum error: " << err << " (" << std::strerror(err)
                << ") - errno reported by local system call "
                << std::strerror(err) << '\n';
      return err;
    }
    std::cerr << "xrd sum error: 38 (Function not implemented) - "
              << "Checksum type " << requestedCheckSumType
              << " not supported for local files\n";
    return 38;
  }

  char checkSumBuffer[265];
  if(checkSum.Get(checkSumBuffer, sizeof(checkSumBuffer)) == 0)
  {
    std::cerr << "xrd sum error: unable to format checksum\n";
    return 13;
  }

  std::cout << LocalDisplayPath(localPath) << ' ' << checkSumBuffer << '\n';
  return 0;
}

int RunRemoteSum(const std::string &path, const std::string &checkSumType)
{
  XrdCl::URL url(path);
  if(!url.IsValid())
  {
    std::cerr << "xrd sum: invalid URL '" << path << "'\n";
    return 64;
  }

  if(url.IsLocalFile())
  {
    return RunLocalSum(url.GetPath(), checkSumType, checkSumType);
  }

  XrdCl::FileSystem fs(FileSystemURL(url));
  XrdCl::Buffer arg;
  arg.FromString(CheckSumQueryPath(url, checkSumType));
  XrdCl::Buffer *rawResponse = nullptr;
  XrdCl::XRootDStatus status =
    fs.Query(XrdCl::QueryCode::Checksum, arg, rawResponse);
  std::unique_ptr<XrdCl::Buffer> response(rawResponse);

  if(!status.IsOK())
  {
    std::cerr << "xrd sum: unable to checksum '" << path
              << "': " << status.ToStr() << '\n';
    return status.GetShellCode();
  }

  if(!response)
  {
    std::cerr << "xrd sum: invalid checksum response for '" << path << "'\n";
    return 1;
  }

  std::string responseType;
  std::string responseValue;
  if(!ParseCheckSumResponse(response->ToString(), responseType, responseValue))
  {
    std::cerr << "xrd sum: invalid checksum response for '" << path << "'\n";
    return 1;
  }

  if(ToLower(responseType) != checkSumType)
  {
    std::cerr << "xrd sum: server returned checksum type " << responseType
              << " instead of " << checkSumType << '\n';
    return 1;
  }

  std::cout << path << ' '
            << FormatCheckSumValue(checkSumType, responseValue) << '\n';
  return 0;
}

void SetVerbose(unsigned int verbosity)
{
  if(verbosity == 0) return;
  if(verbosity == 1) XrdCl::DefaultEnv::SetLogLevel("Warning");
  else if(verbosity == 2) XrdCl::DefaultEnv::SetLogLevel("Info");
  else XrdCl::DefaultEnv::SetLogLevel("Debug");
}

void ApplyClientOptions(int timeout, const std::string &cert,
                        const std::string &key, bool ipv4, bool ipv6)
{
  if(timeout >= 0)
  {
    XrdCl::DefaultEnv::GetEnv()->PutInt("RequestTimeout", timeout);
  }

  if(!cert.empty())
  {
    if(key.empty())
    {
      SetEnvironment("X509_USER_PROXY", cert);
      XrdCl::DefaultEnv::GetEnv()->PutString("HttpClientCertFile", cert);
      XrdCl::DefaultEnv::GetEnv()->PutString("HttpClientKeyFile", cert);
    }
    else
    {
      SetEnvironment("X509_USER_CERT", cert);
      XrdCl::DefaultEnv::GetEnv()->PutString("HttpClientCertFile", cert);
    }
  }

  if(!key.empty())
  {
    SetEnvironment("X509_USER_KEY", key);
    XrdCl::DefaultEnv::GetEnv()->PutString("HttpClientKeyFile", key);
  }

  if(ipv4 && !ipv6)
  {
    XrdCl::DefaultEnv::GetEnv()->PutString("NetworkStack", "IPv4");
    XrdCl::DefaultEnv::GetEnv()->PutInt("PreferIPv4", 1);
  }
  else if(ipv6 && !ipv4)
  {
    XrdCl::DefaultEnv::GetEnv()->PutString("NetworkStack", "IPv6");
    XrdCl::DefaultEnv::GetEnv()->PutInt("PreferIPv4", 0);
  }
}

std::string RightJustify(const std::string &value, std::size_t width)
{
  if(value.size() >= width) return value;
  return std::string(width - value.size(), ' ') + value;
}

std::string LeftJustify(const std::string &value, std::size_t width)
{
  if(value.size() >= width) return value;
  return value + std::string(width - value.size(), ' ');
}

// gfal2-util prints sizes GNU-ls style: one decimal below 10, rounded up.
std::string HumanSize(uint64_t size)
{
  static const char *symbols[] = {"", "K", "M", "G", "T", "P"};
  int degree = 0;
  double value = static_cast<double>(size);
  while(value >= 1024.0 && degree < 5)
  {
    value /= 1024.0;
    ++degree;
  }

  char buffer[32];
  if(value < 10.0)
  {
    std::snprintf(buffer, sizeof(buffer), "%.1f%s",
                  std::ceil(value * 10.0) / 10.0, symbols[degree]);
  }
  else
  {
    std::snprintf(buffer, sizeof(buffer), "%.0f%s",
                  std::ceil(value), symbols[degree]);
  }
  return buffer;
}

// Timestamp formats offered by gfal2-util's --time-style option. All styles
// use local time; full-iso appends "+0000" regardless of the timezone, which
// matches the gfal2-util output captured on lxplus.
std::string LsFormatTime(std::time_t seconds, const std::string &style)
{
  std::tm tm;
  ::localtime_r(&seconds, &tm);
  const bool recent =
    std::time(nullptr) - seconds < 180ll * 24 * 60 * 60;

  char buffer[64];
  if(style == "full-iso")
  {
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buffer) + ".000000 +0000";
  }
  if(style == "long-iso")
  {
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
    return buffer;
  }
  if(style == "iso")
  {
    std::strftime(buffer, sizeof(buffer),
                  recent ? "%m-%d %H:%M" : "%Y-%m-%d", &tm);
    return buffer;
  }
  std::strftime(buffer, sizeof(buffer),
                recent ? "%b %e %H:%M" : "%b %e  %Y", &tm);
  return buffer;
}

struct LsEntry
{
  std::string name;
  std::string mode;
  uint64_t nlink = 1;
  std::string uid = "0";
  std::string gid = "0";
  uint64_t size = 0;
  std::time_t mtime = 0;
};

// Long listing format used by gfal2-util:
// mode nlink uid gid size date name, with a trailing tab.
void PrintLsEntry(const LsEntry &entry, const LsOptions &options)
{
  if(!options.longFormat)
  {
    std::cout << entry.name << '\n';
    return;
  }

  const std::string sizeField = options.humanReadable
    ? RightJustify(HumanSize(entry.size), 5)
    : RightJustify(std::to_string(entry.size), 8);
  const std::string date = LsFormatTime(entry.mtime, options.timeStyle);

  std::cout << entry.mode << ' '
            << RightJustify(std::to_string(entry.nlink), 3) << ' '
            << RightJustify(entry.uid, 5) << ' '
            << RightJustify(entry.gid, 5) << ' '
            << sizeField << ' '
            << LeftJustify(date, 11) << ' '
            << entry.name << "\t\n";
}

bool LsEntryVisible(const std::string &name, const LsOptions &options)
{
  if(name.empty() || name == "." || name == "..") return false;
  return options.all || name.front() != '.';
}

LsEntry LsEntryFromStat(const std::string &name, const struct stat &statBuffer)
{
  LsEntry entry;
  entry.name = name;
  entry.mode = ModeString(statBuffer.st_mode);
  entry.nlink = statBuffer.st_nlink;
  entry.uid = std::to_string(statBuffer.st_uid);
  entry.gid = std::to_string(statBuffer.st_gid);
  entry.size = static_cast<uint64_t>(statBuffer.st_size);
  entry.mtime = statBuffer.st_mtime;
  return entry;
}

LsEntry LsEntryFromStatInfo(const std::string &name,
                            const XrdCl::StatInfo &info)
{
  LsEntry entry;
  entry.name = name;
  entry.mode = ModeString(info);
  if(info.ExtendedFormat())
  {
    if(!info.GetOwner().empty()) entry.uid = info.GetOwner();
    if(!info.GetGroup().empty()) entry.gid = info.GetGroup();
  }
  entry.size = info.GetSize();
  entry.mtime = static_cast<std::time_t>(info.GetModTime());
  return entry;
}

int RunLocalLs(const std::string &originalPath, const std::string &localPath,
               const LsOptions &options)
{
  struct stat statBuffer;
  if(::stat(localPath.c_str(), &statBuffer) != 0)
  {
    const int err = errno;
    std::cerr << "xrd ls error: " << err << " (" << std::strerror(err)
              << ") - errno reported by local system call "
              << std::strerror(err) << '\n';
    return err;
  }

  if(!S_ISDIR(statBuffer.st_mode) || options.directory)
  {
    PrintLsEntry(LsEntryFromStat(originalPath, statBuffer), options);
    return 0;
  }

  DIR *dir = ::opendir(localPath.c_str());
  if(!dir)
  {
    const int err = errno;
    std::cerr << "xrd ls error: " << err << " (" << std::strerror(err)
              << ") - errno reported by local system call "
              << std::strerror(err) << '\n';
    return err;
  }

  std::vector<std::string> names;
  while(struct dirent *item = ::readdir(dir))
  {
    const std::string name = item->d_name;
    if(LsEntryVisible(name, options)) names.push_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());

  for(const auto &name : names)
  {
    LsEntry entry;
    entry.name = name;
    struct stat entryBuffer;
    if(::stat((localPath + "/" + name).c_str(), &entryBuffer) == 0)
    {
      entry = LsEntryFromStat(name, entryBuffer);
    }
    PrintLsEntry(entry, options);
  }
  return 0;
}

int RunRemoteLs(const std::string &path, const LsOptions &options)
{
  XrdCl::URL url(path);
  if(!url.IsValid())
  {
    std::cerr << "xrd ls: invalid URL '" << path << "'\n";
    return 64;
  }

  if(url.IsLocalFile())
  {
    return RunLocalLs(path, url.GetPath(), options);
  }

  XrdCl::FileSystem fs(FileSystemURL(url));
  XrdCl::StatInfo *rawInfo = nullptr;
  XrdCl::XRootDStatus status = fs.Stat(url.GetPathWithParams(), rawInfo);
  std::unique_ptr<XrdCl::StatInfo> info(rawInfo);

  if(!status.IsOK())
  {
    std::cerr << "xrd ls: unable to list '" << path
              << "': " << status.ToStr() << '\n';
    return status.GetShellCode();
  }

  if(!info->TestFlags(XrdCl::StatInfo::IsDir) || options.directory)
  {
    PrintLsEntry(LsEntryFromStatInfo(path, *info), options);
    return 0;
  }

  XrdCl::DirectoryList *rawList = nullptr;
  status = fs.DirList(url.GetPathWithParams(), XrdCl::DirListFlags::Stat,
                      rawList);
  std::unique_ptr<XrdCl::DirectoryList> list(rawList);

  if(!status.IsOK())
  {
    std::cerr << "xrd ls: unable to list '" << path
              << "': " << status.ToStr() << '\n';
    return status.GetShellCode();
  }

  std::vector<LsEntry> entries;
  entries.reserve(list->GetSize());
  for(auto it = list->Begin(); it != list->End(); ++it)
  {
    const XrdCl::DirectoryList::ListEntry *item = *it;
    if(!item || !LsEntryVisible(item->GetName(), options)) continue;

    if(const XrdCl::StatInfo *entryInfo = item->GetStatInfo())
    {
      entries.push_back(LsEntryFromStatInfo(item->GetName(), *entryInfo));
    }
    else
    {
      LsEntry entry;
      entry.name = item->GetName();
      entries.push_back(entry);
    }
  }

  std::sort(entries.begin(), entries.end(),
    [](const LsEntry &a, const LsEntry &b) { return a.name < b.name; });
  for(const auto &entry : entries)
  {
    PrintLsEntry(entry, options);
  }
  return 0;
}

int RunLs(const LsOptions &options)
{
  if(HasScheme(options.path)) return RunRemoteLs(options.path, options);
  return RunLocalLs(options.path, options.path, options);
}

void PrintXAttrError(int err)
{
  std::cerr << "xrd xattr error: " << err << " (" << std::strerror(err)
            << ") - errno reported by local system call "
            << std::strerror(err) << '\n';
}

#if defined(__APPLE__) || defined(__linux__)
int LocalListXAttrNames(const std::string &path,
                        std::vector<std::string> &names)
{
#ifdef __APPLE__
  ssize_t size = ::listxattr(path.c_str(), nullptr, 0, 0);
#else
  ssize_t size = ::listxattr(path.c_str(), nullptr, 0);
#endif
  if(size < 0) return errno;
  if(size == 0) return 0;

  std::vector<char> buffer(static_cast<std::size_t>(size));
#ifdef __APPLE__
  size = ::listxattr(path.c_str(), buffer.data(), buffer.size(), 0);
#else
  size = ::listxattr(path.c_str(), buffer.data(), buffer.size());
#endif
  if(size < 0) return errno;

  std::size_t start = 0;
  for(std::size_t i = 0; i < static_cast<std::size_t>(size); ++i)
  {
    if(buffer[i] == '\0')
    {
      if(i > start) names.emplace_back(&buffer[start], i - start);
      start = i + 1;
    }
  }
  return 0;
}

int LocalGetXAttr(const std::string &path, const std::string &name,
                  std::string &value)
{
#ifdef __APPLE__
  ssize_t size = ::getxattr(path.c_str(), name.c_str(), nullptr, 0, 0, 0);
#else
  ssize_t size = ::getxattr(path.c_str(), name.c_str(), nullptr, 0);
#endif
  if(size < 0) return errno;
  if(size == 0)
  {
    value.clear();
    return 0;
  }

  std::vector<char> buffer(static_cast<std::size_t>(size));
#ifdef __APPLE__
  size = ::getxattr(path.c_str(), name.c_str(), buffer.data(),
                    buffer.size(), 0, 0);
#else
  size = ::getxattr(path.c_str(), name.c_str(), buffer.data(),
                    buffer.size());
#endif
  if(size < 0) return errno;
  value.assign(buffer.data(), static_cast<std::size_t>(size));
  return 0;
}

int RunLocalXAttr(const std::string &localPath, const XAttrOptions &options)
{
  struct stat statBuffer;
  if(::stat(localPath.c_str(), &statBuffer) != 0)
  {
    const int err = errno;
    PrintXAttrError(err);
    return err;
  }

  if(!options.attribute.empty())
  {
    std::string value;
    const int err = LocalGetXAttr(localPath, options.attribute, value);
    if(err != 0)
    {
      PrintXAttrError(err);
      return err;
    }
    std::cout << value << '\n';
    return 0;
  }

  std::vector<std::string> names;
  const int err = LocalListXAttrNames(localPath, names);
  if(err != 0)
  {
    PrintXAttrError(err);
    return err;
  }

  for(const auto &name : names)
  {
    std::string value;
    if(LocalGetXAttr(localPath, name, value) == 0)
    {
      std::cout << name << " = " << value << "\n\n";
    }
    else
    {
      std::cout << name << " FAILED: " << std::strerror(errno) << "\n\n";
    }
  }
  return 0;
}
#else
int RunLocalXAttr(const std::string &, const XAttrOptions &)
{
  std::cerr << "xrd xattr: extended attributes are not supported "
            << "on this platform\n";
  return ENOTSUP;
}
#endif

int RunRemoteXAttr(const std::string &path, const XAttrOptions &options)
{
  XrdCl::URL url(path);
  if(!url.IsValid())
  {
    std::cerr << "xrd xattr: invalid URL '" << path << "'\n";
    return 64;
  }

  if(url.IsLocalFile())
  {
    return RunLocalXAttr(url.GetPath(), options);
  }

  XrdCl::FileSystem fs(FileSystemURL(url));
  const std::string fsPath = url.GetPathWithParams();

  if(!options.attribute.empty())
  {
    std::vector<XrdCl::XAttr> result;
    XrdCl::XRootDStatus status =
      fs.GetXAttr(fsPath, {options.attribute}, result);
    if(!status.IsOK() || result.empty())
    {
      std::cerr << "xrd xattr: unable to get attribute '"
                << options.attribute << "' of '" << path
                << "': " << status.ToStr() << '\n';
      return status.GetShellCode();
    }
    if(!result.front().status.IsOK())
    {
      std::cerr << "xrd xattr: unable to get attribute '"
                << options.attribute << "' of '" << path
                << "': " << result.front().status.ToStr() << '\n';
      return result.front().status.GetShellCode();
    }
    std::cout << result.front().value << '\n';
    return 0;
  }

  std::vector<XrdCl::XAttr> attrs;
  XrdCl::XRootDStatus status = fs.ListXAttr(fsPath, attrs);
  if(!status.IsOK())
  {
    std::cerr << "xrd xattr: unable to list attributes of '" << path
              << "': " << status.ToStr() << '\n';
    return status.GetShellCode();
  }

  for(const auto &attr : attrs)
  {
    if(attr.status.IsOK())
    {
      std::cout << attr.name << " = " << attr.value << "\n\n";
    }
    else
    {
      std::cout << attr.name << " FAILED: " << attr.status.ToStr() << "\n\n";
    }
  }
  return 0;
}

int RunXAttr(const XAttrOptions &options)
{
  if(options.attribute.find('=') != std::string::npos)
  {
    std::cerr << "xrd xattr: setting attributes is not implemented yet\n";
    return 2;
  }

  if(HasScheme(options.path)) return RunRemoteXAttr(options.path, options);
  return RunLocalXAttr(options.path, options);
}

int WriteToStdout(const char *data, std::size_t size)
{
  if(std::fwrite(data, 1, size, stdout) != size)
  {
    const int err = errno ? errno : EIO;
    if(err != EPIPE)
    {
      std::cerr << "xrd cat error: " << err << " (" << std::strerror(err)
                << ") - unable to write to standard output\n";
    }
    return err;
  }
  return 0;
}

int CatLocal(const std::string &localPath)
{
  std::FILE *file = std::fopen(localPath.c_str(), "rb");
  if(!file)
  {
    const int err = errno;
    std::cerr << "xrd cat error: " << err << " (" << std::strerror(err)
              << ") - errno reported by local system call "
              << std::strerror(err) << '\n';
    return err;
  }

  char buffer[1 << 20];
  int result = 0;
  while(result == 0)
  {
    const std::size_t bytesRead = std::fread(buffer, 1, sizeof(buffer), file);
    if(bytesRead > 0) result = WriteToStdout(buffer, bytesRead);
    if(bytesRead < sizeof(buffer))
    {
      if(result == 0 && std::ferror(file))
      {
        result = errno ? errno : EIO;
        std::cerr << "xrd cat error: " << result << " ("
                  << std::strerror(result)
                  << ") - errno reported by local system call "
                  << std::strerror(result) << '\n';
      }
      break;
    }
  }

  std::fclose(file);
  return result;
}

int CatRemote(const std::string &path)
{
  XrdCl::File file;
  XrdCl::XRootDStatus status = file.Open(path, XrdCl::OpenFlags::Read);
  if(!status.IsOK())
  {
    std::cerr << "xrd cat: unable to open '" << path
              << "': " << status.ToStr() << '\n';
    return status.GetShellCode();
  }

  std::vector<char> buffer(1 << 20);
  uint64_t offset = 0;
  int result = 0;
  while(result == 0)
  {
    uint32_t bytesRead = 0;
    status = file.Read(offset, buffer.size(), buffer.data(), bytesRead);
    if(!status.IsOK())
    {
      std::cerr << "xrd cat: unable to read '" << path
                << "': " << status.ToStr() << '\n';
      result = status.GetShellCode();
      break;
    }
    if(bytesRead == 0) break;
    result = WriteToStdout(buffer.data(), bytesRead);
    offset += bytesRead;
  }

  file.Close();
  return result;
}

int RunCat(const CatOptions &options)
{
#ifndef _WIN32
  // Match standard cat semantics for consumers that stop reading early:
  // detect EPIPE on write instead of dying on the signal.
  std::signal(SIGPIPE, SIG_IGN);
#endif

  for(const auto &path : options.paths)
  {
    int result = 0;
    if(HasScheme(path))
    {
      XrdCl::URL url(path);
      if(!url.IsValid())
      {
        std::cerr << "xrd cat: invalid URL '" << path << "'\n";
        return 64;
      }
      result = url.IsLocalFile() ? CatLocal(url.GetPath()) : CatRemote(path);
    }
    else
    {
      result = CatLocal(path);
    }
    if(result != 0) return result;
  }

  std::fflush(stdout);
  return 0;
}

// Options accepted by every xrd subcommand, mirroring the gfal2-util common
// flag set. gfal2/GridFTP-specific flags are accepted for compatibility and
// otherwise ignored.
struct CommonOptions
{
  int timeout = -1;
  unsigned int verbosity = 0;
  bool version = false;
  bool ipv4 = false;
  bool ipv6 = false;
  std::string definition;
  std::string cert;
  std::string key;
  std::string clientInfo;
  std::string logFile;
};

void AddCommonOptions(CLI::App *subcommand, CommonOptions &options)
{
  subcommand->add_flag("-V,--version", options.version,
    "Output version information and exit");
  subcommand->add_flag("-v,--verbose", options.verbosity,
    "Enable verbose client logging");
  subcommand->add_option("-D,--definition", options.definition,
    "Accept a GFAL parameter override");
  subcommand->add_option("-t,--timeout", options.timeout,
    "Maximum operation time in seconds");
  subcommand->add_option("-E,--cert", options.cert,
    "Accept a user certificate path");
  subcommand->add_option("--key", options.key,
    "Accept a user private key path");
  subcommand->add_flag("-4", options.ipv4,
    "Accept the GFAL IPv4-only flag");
  subcommand->add_flag("-6", options.ipv6,
    "Accept the GFAL IPv6-only flag");
  subcommand->add_option("-C,--client-info", options.clientInfo,
    "Accept custom client information");
  subcommand->add_option("--log-file", options.logFile,
    "Write XRootD client logs to a file");
}

// Handle --version and apply the common client options. Returns false when
// the command was fully handled and exitCode is already set.
bool BeginCommand(const std::string &name, const CommonOptions &options,
                  int &exitCode)
{
  if(options.version)
  {
    std::cout << "xrd " << XrdVERSION << '\n';
    exitCode = 0;
    return false;
  }

  if(!options.logFile.empty()
     && !XrdCl::DefaultEnv::SetLogFile(options.logFile))
  {
    std::cerr << "xrd " << name << ": unable to open log file '"
              << options.logFile << "'\n";
    exitCode = 1;
    return false;
  }

  SetVerbose(options.verbosity);
  ApplyClientOptions(options.timeout, options.cert, options.key,
                     options.ipv4, options.ipv6);
  return true;
}

int RunStat(const StatOptions &options)
{
  if(HasScheme(options.path)) return RunRemoteStat(options.path);
  return RunLocalStat(options.path, options.path);
}

int RunSum(const SumOptions &options)
{
  const auto checkSumType = ToLower(options.checkSumType);
  if(HasScheme(options.path)) return RunRemoteSum(options.path, checkSumType);
  return RunLocalSum(options.path, checkSumType, options.checkSumType);
}

int LoadArchivePollUrls(const std::string &url, const std::string &fromFile,
                        std::vector<std::string> &urls)
{
  if(!fromFile.empty() && !url.empty())
  {
    std::cerr << "xrd archivepoll: could not combine --from-file with a URL "
              << "in the positional arguments\n";
    return 1;
  }

  if(!fromFile.empty())
  {
    std::ifstream input(fromFile);
    if(!input)
    {
      std::cerr << "xrd archivepoll: unable to open '" << fromFile << "'\n";
      return 1;
    }

    std::string line;
    while(std::getline(input, line))
    {
      line = Trim(line);
      if(!line.empty()) urls.push_back(line);
    }
  }
  else if(!url.empty())
  {
    urls.push_back(url);
  }

  if(urls.empty())
  {
    std::cerr << "xrd archivepoll: missing URL\n";
    return 1;
  }

  return 0;
}

// Storage URLs are translated to the HTTP(S) endpoint serving the WLCG Tape
// REST API; the operations themselves are provided by the XrdClHttp client
// plugin through FileSystem::Query. root:// and xroot:// URLs are assumed to
// serve the API over HTTPS on the default port.
bool TapeFileSystemURL(const std::string &input, std::string &fsUrl,
                       std::string &error)
{
  XrdCl::URL url(input);
  if(!url.IsValid() || url.GetHostName().empty())
  {
    error = "invalid URL '" + input + "'";
    return false;
  }

  std::string protocol = ToLower(url.GetProtocol());
  bool useUrlPort = true;
  if(protocol == "root" || protocol == "xroot")
  {
    protocol = "https";
    useUrlPort = false;
  }
  else if(protocol == "davs") protocol = "https";
  else if(protocol == "dav") protocol = "http";

  if(protocol != "http" && protocol != "https")
  {
    error = "unsupported URL protocol '" + url.GetProtocol()
      + "' for the Tape REST API";
    return false;
  }

  std::ostringstream out;
  out << protocol << "://" << url.GetHostName();
  if(useUrlPort && url.GetPort() > 0) out << ":" << url.GetPort();
  fsUrl = out.str();
  return true;
}

bool ParseArchiveInfoResponse(const std::string &body,
                              std::vector<ArchivePollResult> &results,
                              std::string &error)
{
  results.clear();
  try
  {
    const nlohmann::json json = nlohmann::json::parse(body);
    if(!json.is_array())
    {
      error = "archiveinfo response is not a JSON array";
      return false;
    }

    results.reserve(json.size());
    for(const auto &item : json)
    {
      if(!item.is_object())
      {
        error = "archiveinfo response contains a non-object entry";
        return false;
      }

      ArchivePollResult result;
      if(item.contains("url") && item["url"].is_string())
      {
        result.url = item["url"].get<std::string>();
      }
      if(item.contains("path") && item["path"].is_string())
      {
        result.path = item["path"].get<std::string>();
      }
      if(item.contains("locality") && item["locality"].is_string())
      {
        result.locality = item["locality"].get<std::string>();
      }
      if(item.contains("error") && item["error"].is_string())
      {
        result.error = item["error"].get<std::string>();
      }
      results.push_back(result);
    }
    return true;
  }
  catch(const std::exception &ex)
  {
    error = "malformed archiveinfo response: " + std::string(ex.what());
    return false;
  }
}

int RunArchivePoll(const ArchivePollOptions &options)
{
  std::string fsUrl;
  std::string error;
  if(!TapeFileSystemURL(options.urls.front(), fsUrl, error))
  {
    std::cerr << "xrd archivepoll: " << error << '\n';
    return 1;
  }

  std::string queryArg = "tape.archiveinfo";
  for(const auto &url : options.urls)
  {
    queryArg += "\n" + url;
  }

  int terminal = 0;
  int wait = options.pollingTimeout;
  int sleep = 1;

  while(true)
  {
    XrdCl::FileSystem filesystem{XrdCl::URL(fsUrl)};
    XrdCl::Buffer arg;
    arg.FromString(queryArg);
    XrdCl::Buffer *responseBuffer = nullptr;
    XrdCl::XRootDStatus status = filesystem.Query(
      XrdCl::QueryCode::Opaque, arg, responseBuffer,
      static_cast<time_t>(options.timeout));
    std::unique_ptr<XrdCl::Buffer> response(responseBuffer);

    if(!status.IsOK())
    {
      std::cerr << "xrd archivepoll: " << status.GetErrorMessage() << '\n';
      return 1;
    }

    std::vector<ArchivePollResult> results;
    if(!ParseArchiveInfoResponse(response ? response->ToString() : "",
                                 results, error))
    {
      std::cerr << "xrd archivepoll: " << error << '\n';
      return 1;
    }

    terminal = PrintArchivePollResults(results);
    if(terminal == static_cast<int>(options.urls.size()) || wait <= 0)
    {
      break;
    }

    std::cout << "Archiving ongoing, sleep " << sleep << " seconds...\n";
    wait -= sleep;
    std::this_thread::sleep_for(std::chrono::seconds(sleep));
    sleep = std::min(sleep * 2, 300);
  }

  return 0;
}

}

int main(int argc, char **argv)
{
  if(argc > 1 && IsCopyCommand(argv[1]))
  {
    return RunCopyCommand(argc, argv);
  }

  CLI::App app{"XRootD command-line client."};
  app.name("xrd");
  app.set_help_flag("-h,--help", "Show this message and exit");
  bool showVersion = false;
  app.add_flag("--version", showVersion, "Show version information and exit");

  int exitCode = 0;
  CommonOptions xattrCommon;
  XAttrOptions xattrOptions;
  CommonOptions catCommon;
  CatOptions catOptions;
  bool catBytes = false;
  CommonOptions lsCommon;
  LsOptions lsOptions;
  bool lsFullTime = false;
  std::vector<std::string> lsXAttrs;
  std::string lsColor = "auto";
  CommonOptions statCommon;
  std::string statPath;
  CommonOptions sumCommon;
  std::string sumPath;
  std::string sumCheckSumType;
  CommonOptions archivePollCommon;
  std::string archivePollUrl;
  int archivePollPollingTimeout = 0;
  std::string archivePollFromFile;

  for(const auto &command : kCommands)
  {
    auto *subcommand = app.add_subcommand(
      std::string(command.name), std::string(command.description));

    if(command.name == "archivepoll")
    {
      subcommand->add_option("surl", archivePollUrl,
        "Site URL to query for archival status");
      AddCommonOptions(subcommand, archivePollCommon);
      subcommand->add_option("--polling-timeout",
        archivePollPollingTimeout, "Timeout for the polling operation");
      subcommand->add_option("--from-file", archivePollFromFile,
        "Read site URLs from a file");
      subcommand->callback([&] {
        if(!BeginCommand("archivepoll", archivePollCommon, exitCode)) return;

        ArchivePollOptions options;
        options.timeout = archivePollCommon.timeout;
        options.pollingTimeout = archivePollPollingTimeout;
        exitCode = LoadArchivePollUrls(archivePollUrl, archivePollFromFile,
                                       options.urls);
        if(exitCode != 0) return;

        exitCode = RunArchivePoll(options);
      });
    }
    else if(command.name == "xattr")
    {
      subcommand->add_option("file", xattrOptions.path,
        "URL of the file or directory");
      subcommand->add_option("attribute", xattrOptions.attribute,
        "Attribute to retrieve; use key=value to set");
      AddCommonOptions(subcommand, xattrCommon);
      subcommand->callback([&] {
        if(!BeginCommand("xattr", xattrCommon, exitCode)) return;
        if(xattrOptions.path.empty())
        {
          std::cerr << "xrd xattr: expected one file URL\n";
          exitCode = 64;
          return;
        }
        exitCode = RunXAttr(xattrOptions);
      });
    }
    else if(command.name == "cat")
    {
      subcommand->add_option("file", catOptions.paths,
        "URLs of the files to print");
      AddCommonOptions(subcommand, catCommon);
      subcommand->add_flag("-b,--bytes", catBytes,
        "Handle file contents as bytes (compatibility flag)");
      subcommand->callback([&] {
        if(!BeginCommand("cat", catCommon, exitCode)) return;
        if(catOptions.paths.empty())
        {
          std::cerr << "xrd cat: expected at least one file URL\n";
          exitCode = 64;
          return;
        }
        exitCode = RunCat(catOptions);
      });
    }
    else if(command.name == "ls")
    {
      subcommand->add_option("file", lsOptions.path,
        "URL of the file or directory to list");
      AddCommonOptions(subcommand, lsCommon);
      subcommand->add_flag("-a,--all", lsOptions.all,
        "Display hidden files");
      subcommand->add_flag("-l,--long", lsOptions.longFormat,
        "Use a long listing format");
      subcommand->add_flag("-d,--directory", lsOptions.directory,
        "List directory entries instead of contents");
      subcommand->add_flag("-H,--human-readable", lsOptions.humanReadable,
        "With -l, print sizes in human readable format");
      subcommand->add_option("--xattr", lsXAttrs,
        "Query additional attributes (accepted, not implemented yet)");
      subcommand->add_option("--time-style", lsOptions.timeStyle,
        "Time style")
        ->check(CLI::IsMember({"full-iso", "long-iso", "iso", "locale"}));
      subcommand->add_flag("--full-time", lsFullTime,
        "Same as --time-style=full-iso");
      subcommand->add_option("--color", lsColor,
        "Print colored entries with -l (accepted, not implemented yet)")
        ->check(CLI::IsMember({"always", "never", "auto"}));
      subcommand->callback([&] {
        if(!BeginCommand("ls", lsCommon, exitCode)) return;
        if(lsOptions.path.empty())
        {
          std::cerr << "xrd ls: expected one file URL\n";
          exitCode = 64;
          return;
        }
        if(!lsXAttrs.empty())
        {
          std::cerr << "xrd ls: --xattr is not implemented yet, "
                    << "the attributes are ignored\n";
        }
        // gfal2-util's --full-time output uses the long-iso format.
        if(lsFullTime) lsOptions.timeStyle = "long-iso";
        exitCode = RunLs(lsOptions);
      });
    }
    else if(command.name == "stat")
    {
      subcommand->add_option("file", statPath,
        "URL of the file or directory to stat");
      AddCommonOptions(subcommand, statCommon);
      subcommand->callback([&] {
        if(!BeginCommand("stat", statCommon, exitCode)) return;
        if(statPath.empty())
        {
          std::cerr << "xrd stat: expected one file URL\n";
          exitCode = 64;
          return;
        }
        StatOptions options;
        options.path = statPath;
        exitCode = RunStat(options);
      });
    }
    else if(command.name == "sum")
    {
      subcommand->add_option("file", sumPath,
        "File URL to use for checksum calculation");
      subcommand->add_option("checksum_type", sumCheckSumType,
        "Checksum algorithm to use");
      AddCommonOptions(subcommand, sumCommon);
      subcommand->callback([&] {
        if(!BeginCommand("sum", sumCommon, exitCode)) return;
        if(sumPath.empty() || sumCheckSumType.empty())
        {
          std::cerr << "xrd sum: expected one file URL and checksum type\n";
          exitCode = 64;
          return;
        }
        SumOptions options;
        options.path = sumPath;
        options.checkSumType = sumCheckSumType;
        exitCode = RunSum(options);
      });
    }
    else
    {
      subcommand->allow_extras();
      subcommand->callback([&exitCode, name = command.name] {
        exitCode = NotImplemented(name);
      });
    }
  }

  CLI11_PARSE(app, argc, argv);

  if(showVersion)
  {
    std::cout << "xrd " << XrdVERSION << '\n';
    return 0;
  }

  if(app.get_subcommands().empty())
  {
    std::cout << app.help();
  }
  return exitCode;
}
