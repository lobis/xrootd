/******************************************************************************/
/*                                                                            */
/*                    X r d C l H t t p T a p e . h h                         */
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

#ifndef XRDCLHTTP_TAPE_HH
#define XRDCLHTTP_TAPE_HH

#include "XrdCl/XrdClXRootDResponses.hh"

#include <array>
#include <string>
#include <vector>

namespace XrdClHttp
{
  class HeaderCallout;

  // Options shared by all Tape REST helper calls.
  //
  // Storage URLs passed to the helpers are translated into HTTP(S) discovery
  // endpoints; root:// and xroot:// URLs are assumed to serve the discovery
  // document over HTTPS on the default port (443), so any xroot port present
  // in the URL is dropped.
  struct TapeOptions
  {
    int timeout = -1;                        //!< Total per-request timeout in
                                             //!< seconds (-1 for no timeout)
    HeaderCallout *headerCallout = nullptr;  //!< Optional callout used to
                                             //!< amend outgoing HTTP headers
  };

  XrdCl::XRootDStatus TapeDiscover( const std::string &url,
                                    const TapeOptions &options,
                                    std::string &uri,
                                    std::string &version,
                                    std::string &sitename );

  // File entries are ordered as: url, path, diskLifetime, targetedMetadata.
  XrdCl::XRootDStatus TapeStage(
    const std::string &url,
    const std::vector<std::array<std::string, 4>> &files,
    const TapeOptions &options,
    std::string &requestId );

  XrdCl::XRootDStatus TapeStageStatus( const std::string &url,
                                       const std::string &requestId,
                                       const TapeOptions &options,
                                       std::string &responseJson );

  XrdCl::XRootDStatus TapeStageCancel(
      const std::string &url,
      const std::string &requestId,
      const std::vector<std::string> &paths,
      const TapeOptions &options );

  XrdCl::XRootDStatus TapeStageDelete( const std::string &url,
                                       const std::string &requestId,
                                       const TapeOptions &options );

  XrdCl::XRootDStatus TapeRelease( const std::string &url,
                                   const std::string &requestId,
                                   const std::vector<std::string> &paths,
                                   const TapeOptions &options );

  XrdCl::XRootDStatus TapeArchiveInfo(
    const std::vector<std::string> &urls,
    const TapeOptions &options,
    std::string &responseJson );

  // Drop all cached Tape REST discovery results, forcing the next helper
  // call to repeat endpoint discovery.
  void TapeClearDiscoveryCache();
}

#endif // XRDCLHTTP_TAPE_HH
