/******************************************************************************/
/*                                                                            */
/*                    X r d C l T a p e R e s t . h h                         */
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

#ifndef __XRD_CL_TAPE_REST_HH__
#define __XRD_CL_TAPE_REST_HH__

#include "XrdCl/XrdClXRootDResponses.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace XrdCl
{
  //----------------------------------------------------------------------------
  //! Locality values returned by the WLCG Tape REST API archiveinfo endpoint.
  //----------------------------------------------------------------------------
  enum class TapeRestLocality
  {
    Disk,
    Tape,
    DiskAndTape,
    Lost,
    None,
    Unavailable,
    Unknown
  };

  //----------------------------------------------------------------------------
  //! Client options used by TapeRestClient.
  //----------------------------------------------------------------------------
  struct TapeRestOptions
  {
    int timeout = -1;
    std::string cert;
    std::string key;
    unsigned int verbosity = 0;
  };

  //----------------------------------------------------------------------------
  //! Endpoint selected from the Tape REST API discovery document.
  //----------------------------------------------------------------------------
  struct TapeRestEndpoint
  {
    std::string uri;
    std::string version;
    std::string sitename;
  };

  //----------------------------------------------------------------------------
  //! Per-path archiveinfo response.
  //----------------------------------------------------------------------------
  struct TapeRestArchiveInfo
  {
    std::string url;
    std::string path;
    TapeRestLocality locality = TapeRestLocality::Unknown;
    std::string error;
  };

  //----------------------------------------------------------------------------
  //! File entry submitted to the Tape REST API stage endpoint.
  //----------------------------------------------------------------------------
  struct TapeRestStageFile
  {
    std::string url;
    std::string path;
    std::string diskLifetime;
    std::string targetedMetadata;
  };

  //----------------------------------------------------------------------------
  //! Response returned when a stage request is submitted.
  //----------------------------------------------------------------------------
  struct TapeRestStageResponse
  {
    std::string requestId;
  };

  //----------------------------------------------------------------------------
  //! Per-path status returned while polling a stage request.
  //----------------------------------------------------------------------------
  struct TapeRestStageFileStatus
  {
    std::string path;
    bool onDisk = false;
    bool hasOnDisk = false;
    std::string state;
    std::string error;
    std::uint64_t startedAt = 0;
    bool hasStartedAt = false;
    std::uint64_t finishedAt = 0;
    bool hasFinishedAt = false;
  };

  //----------------------------------------------------------------------------
  //! Status returned while polling a stage request.
  //----------------------------------------------------------------------------
  struct TapeRestStageStatus
  {
    std::string id;
    std::uint64_t createdAt = 0;
    bool hasCreatedAt = false;
    std::uint64_t startedAt = 0;
    bool hasStartedAt = false;
    std::uint64_t completedAt = 0;
    bool hasCompletedAt = false;
    std::vector<TapeRestStageFileStatus> files;
  };

  //----------------------------------------------------------------------------
  //! Small synchronous client for the WLCG Tape REST API.
  //----------------------------------------------------------------------------
  class TapeRestClient
  {
    public:
      explicit TapeRestClient( const TapeRestOptions &options = TapeRestOptions() );
      ~TapeRestClient();

      XRootDStatus Discover( const std::string &url,
                             TapeRestEndpoint &endpoint ) const;

      XRootDStatus Stage( const std::string &url,
                          const std::vector<TapeRestStageFile> &files,
                          TapeRestStageResponse &response ) const;

      XRootDStatus StageStatus( const std::string &url,
                                const std::string &requestId,
                                TapeRestStageStatus &response ) const;

      XRootDStatus StageCancel( const std::string &url,
                                const std::string &requestId,
                                const std::vector<std::string> &paths ) const;

      XRootDStatus StageDelete( const std::string &url,
                                const std::string &requestId ) const;

      XRootDStatus Release( const std::string &url,
                            const std::string &requestId,
                            const std::vector<std::string> &paths ) const;

      XRootDStatus ArchiveInfo( const std::vector<std::string> &urls,
                                std::vector<TapeRestArchiveInfo> &results ) const;

      static std::string LocalityToString( TapeRestLocality locality );
      static TapeRestLocality LocalityFromString( const std::string &locality );

    private:
      TapeRestOptions pOptions;
  };
}

#endif // __XRD_CL_TAPE_REST_HH__
