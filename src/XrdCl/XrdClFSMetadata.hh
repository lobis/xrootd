#ifndef __XRD_CL_FS_METADATA_HH__
#define __XRD_CL_FS_METADATA_HH__

#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdOuc/XrdOucJson.hh"

#include <iostream>

namespace XrdCl
{
  namespace FSMetadata
  {
    inline std::string PathOnly( const std::string &path )
    {
      return path.substr( 0, path.find( '?' ) );
    }

    inline nlohmann::json Entry( const std::string &path, const StatInfo &info )
    {
      const bool extended = info.ExtendedFormat();
      const char *type = info.TestFlags( StatInfo::IsDir ) ? "directory" :
                         info.TestFlags( StatInfo::Other ) ? "other" : "file";
      nlohmann::json names = nlohmann::json::array();
      const std::pair<StatInfo::Flags, const char *> flags[] = {
        { StatInfo::XBitSet, "XBitSet" },
        { StatInfo::IsDir, "IsDir" },
        { StatInfo::Other, "Other" },
        { StatInfo::Offline, "Offline" },
        { StatInfo::POSCPending, "POSCPending" },
        { StatInfo::IsReadable, "IsReadable" },
        { StatInfo::IsWritable, "IsWritable" },
        { StatInfo::BackUpExists, "BackUpExists" }
      };
      for( const auto &flag : flags )
        if( info.TestFlags( flag.first ) ) names.push_back( flag.second );

      auto optional = []( bool present, const std::string &value ) -> nlohmann::json
      {
        if( present ) return value;
        return nullptr;
      };
      return {
        { "path", path },
        { "type", type },
        { "size", info.GetSize() },
        { "mtime", info.GetModTime() },
        { "atime", extended ? info.GetAccessTime() : info.GetModTime() },
        { "ctime", extended ? info.GetChangeTime() : info.GetModTime() },
        { "flags", info.GetFlags() },
        { "flag_names", std::move( names ) },
        { "extended", extended },
        { "mode", optional( extended, info.GetModeAsString() ) },
        { "permissions", optional( extended, info.GetModeAsOctString() ) },
        { "owner", optional( extended, info.GetOwner() ) },
        { "group", optional( extended, info.GetGroup() ) },
        { "checksum", optional( info.HasChecksum(), info.GetChecksum() ) },
        { "xattrs", nlohmann::json::array() }
      };
    }

    inline XRootDStatus Print( const XRootDStatus &status,
                               nlohmann::json entries = nlohmann::json::array() )
    {
      const nlohmann::json result = {
        { "version", 1 },
        { "status", {
            { "ok", status.IsOK() },
            { "code", status.code },
            { "number", status.errNo },
            { "partial", status.IsOK() && status.code == suPartial }
        } },
        { "entries", std::move( entries ) }
      };
      try
      {
        std::cout << result.dump() << std::endl;
      }
      catch( const nlohmann::json::exception & )
      {
        // A text path may contain bytes that JSON cannot represent.
        return Print( XRootDStatus( stError, errDataError ) );
      }
      return std::cout ? status : XRootDStatus( stError, errOSError );
    }
  }
}

#endif
