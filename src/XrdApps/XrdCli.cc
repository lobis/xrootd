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
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClTapeRest.hh"
#include "XrdCl/XrdClURL.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCks/XrdCksCalc.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <limits.h>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <getopt.h>
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

struct SumOptions
{
  std::string path;
  std::string checkSumType;
  int timeout = -1;
};

struct ArchivePollOptions
{
  std::vector<std::string> urls;
  int timeout = -1;
  int pollingTimeout = 0;
};

struct ClientOptions
{
  int timeout = -1;
  unsigned int verbosity = 0;
  std::string cert;
  std::string key;
  std::string logFile;
  bool ipv4 = false;
  bool ipv6 = false;
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

const Command *FindCommand(std::string_view name)
{
  for(const auto &command : kCommands)
  {
    if(command.name == name) return &command;
  }
  return nullptr;
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

ArchivePollState ArchivePollStateFromLocality(
  XrdCl::TapeRestLocality locality )
{
  if(locality == XrdCl::TapeRestLocality::Tape
     || locality == XrdCl::TapeRestLocality::DiskAndTape)
  {
    return ArchivePollState::Ready;
  }
  if(locality == XrdCl::TapeRestLocality::Disk
     || locality == XrdCl::TapeRestLocality::Unavailable)
  {
    return ArchivePollState::Queued;
  }
  return ArchivePollState::Failed;
}

std::string ArchivePollFailureMessage(
  const XrdCl::TapeRestArchiveInfo &result )
{
  if(!result.error.empty())
  {
    return "[Tape REST API] " + result.error;
  }

  return "[Tape REST API] File locality reported as "
    + XrdCl::TapeRestClient::LocalityToString(result.locality)
    + " (path=" + result.path + ")";
}

int PrintArchivePollResults(
  const std::vector<XrdCl::TapeRestArchiveInfo> &results)
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

int RunStat(const StatOptions &options, unsigned int verbosity = 0,
            const std::string &logFile = "", const std::string &cert = "",
            const std::string &key = "", bool ipv4 = false,
            bool ipv6 = false)
{
  if(!logFile.empty() && !XrdCl::DefaultEnv::SetLogFile(logFile))
  {
    std::cerr << "xrd stat: unable to open log file '" << logFile << "'\n";
    return 1;
  }

  SetVerbose(verbosity);
  ApplyClientOptions(options.timeout, cert, key, ipv4, ipv6);

  if(HasScheme(options.path)) return RunRemoteStat(options.path);
  return RunLocalStat(options.path, options.path);
}

int RunSum(const SumOptions &options, unsigned int verbosity = 0,
           const std::string &logFile = "", const std::string &cert = "",
           const std::string &key = "", bool ipv4 = false,
           bool ipv6 = false)
{
  if(!logFile.empty() && !XrdCl::DefaultEnv::SetLogFile(logFile))
  {
    std::cerr << "xrd sum: unable to open log file '" << logFile << "'\n";
    return 1;
  }

  SetVerbose(verbosity);
  ApplyClientOptions(options.timeout, cert, key, ipv4, ipv6);

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

int RunArchivePoll(const ArchivePollOptions &options,
                   unsigned int verbosity = 0,
                   const std::string &logFile = "",
                   const std::string &cert = "",
                   const std::string &key = "",
                   bool ipv4 = false,
                   bool ipv6 = false)
{
  if(!logFile.empty() && !XrdCl::DefaultEnv::SetLogFile(logFile))
  {
    std::cerr << "xrd archivepoll: unable to open log file '" << logFile
              << "'\n";
    return 1;
  }

  SetVerbose(verbosity);
  ApplyClientOptions(options.timeout, cert, key, ipv4, ipv6);

  XrdCl::TapeRestOptions tapeRestOptions;
  tapeRestOptions.timeout = options.timeout;
  tapeRestOptions.cert = cert;
  tapeRestOptions.key = key;
  tapeRestOptions.verbosity = verbosity;
  XrdCl::TapeRestClient tapeRest(tapeRestOptions);

  int terminal = 0;
  int wait = options.pollingTimeout;
  int sleep = 1;

  while(true)
  {
    std::vector<XrdCl::TapeRestArchiveInfo> results;
    XrdCl::XRootDStatus status = tapeRest.ArchiveInfo(options.urls, results);
    if(!status.IsOK())
    {
      std::cerr << "xrd archivepoll: " << status.GetErrorMessage() << '\n';
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

void PrintVersion()
{
  std::cout << "xrd " << XrdVERSION << '\n';
}

void PrintMainHelp()
{
  std::cout
    << "XRootD command-line client.\n\n"
    << "Usage: xrd [OPTIONS] COMMAND [ARGS]...\n\n"
    << "Options:\n"
    << "  -h, --help       Show this message and exit\n"
    << "      --version    Show version information and exit\n\n"
    << "Commands:\n";

  for(const auto &command : kCommands)
  {
    std::cout << "  " << std::left << std::setw(13) << command.name
              << command.description << '\n';
  }
}

void PrintCommonOptionsHelp()
{
  std::cout
    << "  -h, --help                 Show this message and exit\n"
    << "  -V, --version              Output version information and exit\n"
    << "  -v, --verbose              Enable verbose client logging\n"
    << "  -D, --definition VALUE     Accept a GFAL parameter override\n"
    << "  -t, --timeout SECONDS      Maximum operation time in seconds\n"
    << "  -E, --cert PATH            Accept a user certificate path\n"
    << "      --key PATH             Accept a user private key path\n"
    << "  -4                         Accept the GFAL IPv4-only flag\n"
    << "  -6                         Accept the GFAL IPv6-only flag\n"
    << "  -C, --client-info INFO     Accept custom client information\n"
    << "      --log-file PATH        Write XRootD client logs to a file\n";
}

void PrintArchivePollHelp()
{
  std::cout
    << "Perform an archive polling operation on the given URL\n\n"
    << "Usage: xrd archivepoll [OPTIONS] [surl]\n\n"
    << "Options:\n";
  PrintCommonOptionsHelp();
  std::cout
    << "      --polling-timeout SECONDS  Timeout for the polling operation\n"
    << "      --from-file PATH           Read site URLs from a file\n";
}

void PrintStatHelp()
{
  std::cout
    << "Display extended information about a file or directory\n\n"
    << "Usage: xrd stat [OPTIONS] file\n\n"
    << "Options:\n";
  PrintCommonOptionsHelp();
}

void PrintSumHelp()
{
  std::cout
    << "Calculate a file checksum\n\n"
    << "Usage: xrd sum [OPTIONS] file checksum_type\n\n"
    << "Options:\n";
  PrintCommonOptionsHelp();
  std::cout
    << "\nArguments:\n"
    << "  file              File URL to use for checksum calculation\n"
    << "  checksum_type     Checksum algorithm to use\n";
}

void PrintCommandHelp(std::string_view name)
{
  if(name == "archivepoll") PrintArchivePollHelp();
  else if(name == "stat") PrintStatHelp();
  else if(name == "sum") PrintSumHelp();
  else if(const auto *command = FindCommand(name))
  {
    std::cout << command->description << "\n\n"
              << "Usage: xrd " << command->name << " [ARGS]...\n";
  }
}

bool ParseIntegerOption(std::string_view command, const char *optionName,
                        const char *value, int &target)
{
  if(value == nullptr || *value == '\0')
  {
    std::cerr << "xrd " << command << ": option " << optionName
              << " requires an argument\n";
    return false;
  }

  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value, &end, 10);
  if(errno != 0 || end == value || *end != '\0'
     || parsed < std::numeric_limits<int>::min()
     || parsed > std::numeric_limits<int>::max())
  {
    std::cerr << "xrd " << command << ": invalid value for "
              << optionName << ": '" << value << "'\n";
    return false;
  }

  target = static_cast<int>(parsed);
  return true;
}

enum LongOptionCode
{
  KeyOption = 1000,
  LogFileOption,
  PollingTimeoutOption,
  FromFileOption
};

enum class ParseStatus
{
  Continue,
  Exit,
  Error
};

ParseStatus HandleCommonOption(int option, int currentArg,
                               std::string_view command,
                               ClientOptions &options, char **argv)
{
  switch(option)
  {
  case 'h':
    PrintCommandHelp(command);
    return ParseStatus::Exit;
  case 'V':
    PrintVersion();
    return ParseStatus::Exit;
  case 'v':
    ++options.verbosity;
    return ParseStatus::Continue;
  case 'D':
  case 'C':
    return ParseStatus::Continue;
  case 't':
    return ParseIntegerOption(command, "--timeout", optarg, options.timeout)
      ? ParseStatus::Continue : ParseStatus::Error;
  case 'E':
    options.cert = optarg ? optarg : "";
    return ParseStatus::Continue;
  case '4':
    options.ipv4 = true;
    return ParseStatus::Continue;
  case '6':
    options.ipv6 = true;
    return ParseStatus::Continue;
  case KeyOption:
    options.key = optarg ? optarg : "";
    return ParseStatus::Continue;
  case LogFileOption:
    options.logFile = optarg ? optarg : "";
    return ParseStatus::Continue;
  case ':':
    std::cerr << "xrd " << command << ": option requires an argument: "
              << argv[currentArg] << '\n';
    return ParseStatus::Error;
  case '?':
  default:
    std::cerr << "xrd " << command << ": unrecognized option: "
              << argv[currentArg] << '\n';
    return ParseStatus::Error;
  }
}

int ParseArchivePollCommand(int argc, char **argv)
{
  ClientOptions clientOptions;
  ArchivePollOptions options;
  std::string url;
  std::string fromFile;

  static const option longOptions[] = {
    {"help", no_argument, nullptr, 'h'},
    {"version", no_argument, nullptr, 'V'},
    {"verbose", no_argument, nullptr, 'v'},
    {"definition", required_argument, nullptr, 'D'},
    {"timeout", required_argument, nullptr, 't'},
    {"cert", required_argument, nullptr, 'E'},
    {"key", required_argument, nullptr, KeyOption},
    {"client-info", required_argument, nullptr, 'C'},
    {"log-file", required_argument, nullptr, LogFileOption},
    {"polling-timeout", required_argument, nullptr, PollingTimeoutOption},
    {"from-file", required_argument, nullptr, FromFileOption},
    {nullptr, 0, nullptr, 0}
  };

  optind = 2;
  opterr = 0;
  while(true)
  {
    const int option =
      getopt_long(argc, argv, ":hVvD:t:E:46C:", longOptions, nullptr);
    if(option == -1) break;

    if(option == PollingTimeoutOption)
    {
      if(!ParseIntegerOption("archivepoll", "--polling-timeout", optarg,
                             options.pollingTimeout))
      {
        return 64;
      }
      continue;
    }
    if(option == FromFileOption)
    {
      fromFile = optarg ? optarg : "";
      continue;
    }

    const auto status =
      HandleCommonOption(option, optind - 1, "archivepoll",
                         clientOptions, argv);
    if(status == ParseStatus::Exit) return 0;
    if(status == ParseStatus::Error) return 64;
  }

  if(argc - optind > 1)
  {
    std::cerr << "xrd archivepoll: expected at most one URL\n";
    return 64;
  }
  if(optind < argc) url = argv[optind];

  options.timeout = clientOptions.timeout;
  const int loadResult = LoadArchivePollUrls(url, fromFile, options.urls);
  if(loadResult != 0) return loadResult;

  return RunArchivePoll(options, clientOptions.verbosity,
                        clientOptions.logFile, clientOptions.cert,
                        clientOptions.key, clientOptions.ipv4,
                        clientOptions.ipv6);
}

int ParseStatCommand(int argc, char **argv)
{
  ClientOptions clientOptions;

  static const option longOptions[] = {
    {"help", no_argument, nullptr, 'h'},
    {"version", no_argument, nullptr, 'V'},
    {"verbose", no_argument, nullptr, 'v'},
    {"definition", required_argument, nullptr, 'D'},
    {"timeout", required_argument, nullptr, 't'},
    {"cert", required_argument, nullptr, 'E'},
    {"key", required_argument, nullptr, KeyOption},
    {"client-info", required_argument, nullptr, 'C'},
    {"log-file", required_argument, nullptr, LogFileOption},
    {nullptr, 0, nullptr, 0}
  };

  optind = 2;
  opterr = 0;
  while(true)
  {
    const int option =
      getopt_long(argc, argv, ":hVvD:t:E:46C:", longOptions, nullptr);
    if(option == -1) break;

    const auto status =
      HandleCommonOption(option, optind - 1, "stat", clientOptions, argv);
    if(status == ParseStatus::Exit) return 0;
    if(status == ParseStatus::Error) return 64;
  }

  if(argc - optind != 1)
  {
    std::cerr << "xrd stat: expected one file URL\n";
    return 64;
  }

  StatOptions options;
  options.path = argv[optind];
  options.timeout = clientOptions.timeout;
  return RunStat(options, clientOptions.verbosity, clientOptions.logFile,
                 clientOptions.cert, clientOptions.key, clientOptions.ipv4,
                 clientOptions.ipv6);
}

int ParseSumCommand(int argc, char **argv)
{
  ClientOptions clientOptions;

  static const option longOptions[] = {
    {"help", no_argument, nullptr, 'h'},
    {"version", no_argument, nullptr, 'V'},
    {"verbose", no_argument, nullptr, 'v'},
    {"definition", required_argument, nullptr, 'D'},
    {"timeout", required_argument, nullptr, 't'},
    {"cert", required_argument, nullptr, 'E'},
    {"key", required_argument, nullptr, KeyOption},
    {"client-info", required_argument, nullptr, 'C'},
    {"log-file", required_argument, nullptr, LogFileOption},
    {nullptr, 0, nullptr, 0}
  };

  optind = 2;
  opterr = 0;
  while(true)
  {
    const int option =
      getopt_long(argc, argv, ":hVvD:t:E:46C:", longOptions, nullptr);
    if(option == -1) break;

    const auto status =
      HandleCommonOption(option, optind - 1, "sum", clientOptions, argv);
    if(status == ParseStatus::Exit) return 0;
    if(status == ParseStatus::Error) return 64;
  }

  if(argc - optind != 2)
  {
    std::cerr << "xrd sum: expected one file URL and checksum type\n";
    return 64;
  }

  SumOptions options;
  options.path = argv[optind];
  options.checkSumType = argv[optind + 1];
  options.timeout = clientOptions.timeout;
  return RunSum(options, clientOptions.verbosity, clientOptions.logFile,
                clientOptions.cert, clientOptions.key, clientOptions.ipv4,
                clientOptions.ipv6);
}

}

int main(int argc, char **argv)
{
  if(argc > 1 && IsCopyCommand(argv[1]))
  {
    return RunCopyCommand(argc, argv);
  }

  if(argc == 1)
  {
    PrintMainHelp();
    return 0;
  }

  const std::string_view firstArg = argv[1];
  if(firstArg == "-h" || firstArg == "--help")
  {
    PrintMainHelp();
    return 0;
  }
  if(firstArg == "--version")
  {
    PrintVersion();
    return 0;
  }

  const Command *command = FindCommand(firstArg);
  if(!command)
  {
    std::cerr << "xrd: unknown command '" << firstArg << "'\n";
    return 64;
  }

  if(firstArg == "archivepoll") return ParseArchivePollCommand(argc, argv);
  if(firstArg == "stat") return ParseStatCommand(argc, argv);
  if(firstArg == "sum") return ParseSumCommand(argc, argv);

  if(argc > 2)
  {
    const std::string_view secondArg = argv[2];
    if(secondArg == "-h" || secondArg == "--help")
    {
      PrintCommandHelp(firstArg);
      return 0;
    }
  }

  return NotImplemented(command->name);
}
