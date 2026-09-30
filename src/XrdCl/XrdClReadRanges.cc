// Copyright (c) 2026 by the XRootD developers.
// Distributed under the GNU Lesser General Public License, version 3 or later.

#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClMessageUtils.hh"
#include "XrdCl/XrdClXRootDResponses.hh"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace XrdCl
{
  namespace
  {
    using Clock = std::chrono::steady_clock;
    struct Limits
    {
      uint32_t count = 1024;
      uint32_t size = 2097136;
      Clock::time_point expires;
    };
    std::mutex limitsMutex;
    std::map<std::string, Limits> limitsCache;

    class RangeReader;
    class RangeHandler : public ResponseHandler
    {
      public:
        RangeHandler( std::shared_ptr<RangeReader> reader, ChunkList chunks ):
          reader( std::move( reader ) ), chunks( std::move( chunks ) ) {}
        void HandleResponse( XRootDStatus *, AnyObject * ) override;
      private:
        std::shared_ptr<RangeReader> reader;
        ChunkList chunks;
    };

    class RangeReader : public std::enable_shared_from_this<RangeReader>
    {
      public:
        RangeReader( File &file, const ChunkList &chunks,
                     ResponseHandler *handler, uint16_t parallel, time_t timeout ):
          file( file ), chunks( chunks ), handler( handler ), parallel( parallel ),
          timeout( timeout ), deadline( Clock::now() + std::chrono::seconds( timeout ) ) {}

        time_t Remaining() const
        {
          if( !timeout ) return 0;
          auto remaining = deadline - Clock::now();
          if( remaining <= Clock::duration::zero() ) return -1;
          return std::chrono::duration_cast<std::chrono::seconds>( remaining ).count() + 1;
        }

        void Start()
        {
          std::string server;
          if( file.GetProperty( "DataServer", server ) && !server.empty() )
          {
            URL url( server );
            endpoint = url.GetProtocol() + "://" + url.GetHostId() + "/";
            {
              std::lock_guard<std::mutex> lock( limitsMutex );
              auto it = limitsCache.find( endpoint );
              if( it != limitsCache.end() && it->second.expires > Clock::now() )
              {
                limits = it->second;
                endpoint.clear(); // No discovery needed.
              }
            }
          }
          if( endpoint.empty() ) return Pump();
          filesystem.reset( new FileSystem( URL( endpoint ) ) );
          struct QueryHandler : ResponseHandler
          {
            explicit QueryHandler( std::shared_ptr<RangeReader> reader ): reader( reader ) {}
            void HandleResponse( XRootDStatus *status, AnyObject *response ) override
            {
              auto keep = reader;
              keep->Discovered( status, response );
              delete this;
            }
            std::shared_ptr<RangeReader> reader;
          };
          auto query = new QueryHandler( shared_from_this() );
          Buffer arg;
          arg.FromString( "readv_iov_max readv_ior_max" );
          auto status = filesystem->Query( QueryCode::Config, arg, query, Remaining() );
          if( !status.IsOK() ) query->HandleResponse( new XRootDStatus( status ), nullptr );
        }

        void Discovered( XRootDStatus *status, AnyObject *response )
        {
          std::unique_ptr<XRootDStatus> ownStatus( status );
          std::unique_ptr<AnyObject> ownResponse( response );
          Buffer *buffer = nullptr;
          if( response ) response->Get( buffer );
          if( status->IsOK() && buffer )
          {
            std::string text( buffer->GetBuffer(), buffer->GetSize() );
            if( !text.empty() && text.back() == '\0' ) text.pop_back();
            std::istringstream input( text );
            uint64_t count = 0, size = 0;
            std::string extra;
            if( (input >> count >> size) && !(input >> extra) && count && size &&
                count <= 1024 && size <= std::numeric_limits<uint32_t>::max() )
            {
              limits.count = count;
              limits.size = size;
              limits.expires = Clock::now() + std::chrono::seconds( 60 );
              std::lock_guard<std::mutex> lock( limitsMutex );
              if( limitsCache.size() >= 256 ) limitsCache.clear();
              limitsCache[endpoint] = limits;
            }
          }
          Pump();
        }

        void Completed( const XRootDStatus &status )
        {
          {
            std::lock_guard<std::mutex> lock( mutex );
            --active;
            if( result.IsOK() && !status.IsOK() ) result = status;
            if( result.IsOK() && Remaining() < 0 )
              result = XRootDStatus( stError, errOperationExpired );
          }
          Pump();
        }

        void Pump()
        {
          // Submission may complete inline. One pump owns the submission loop;
          // callbacks only update its state until it releases that ownership.
          auto keep = shared_from_this();
          std::unique_lock<std::mutex> lock( mutex );
          if( pumping || finished ) return;
          pumping = true;
          while( result.IsOK() && active < parallel && index < chunks.size() )
          {
            time_t remaining = Remaining();
            if( remaining < 0 )
            {
              result = XRootDStatus( stError, errOperationExpired );
              break;
            }
            ChunkList batch;
            uint64_t bytes = 0;
            while( batch.size() < limits.count && index < chunks.size() )
            {
              const auto &chunk = chunks[index];
              if( !chunk.length ) { ++index; continue; }
              uint32_t length = std::min( chunk.length - offset, limits.size );
              // VectorReadInfo has a 32-bit aggregate size.
              if( bytes + length > std::numeric_limits<uint32_t>::max() ) break;
              batch.emplace_back( chunk.offset + offset, length,
                                  static_cast<char *>( chunk.buffer ) + offset );
              bytes += length;
              offset += length;
              if( offset == chunk.length ) { ++index; offset = 0; }
            }
            if( batch.empty() ) continue;
            ++active;
            lock.unlock();
            auto completion = new RangeHandler( keep, batch );
            auto status = file.VectorRead( batch, nullptr, completion, remaining );
            if( !status.IsOK() )
              completion->HandleResponse( new XRootDStatus( status ), nullptr );
            lock.lock();
          }
          pumping = false;
          if( active || (result.IsOK() && index < chunks.size()) ) return;
          finished = true;
          auto status = new XRootDStatus( result );
          lock.unlock();
          handler->HandleResponse( status, nullptr );
        }

      private:
        File &file;
        ChunkList chunks;
        ResponseHandler *handler;
        uint16_t parallel;
        time_t timeout;
        Clock::time_point deadline;
        Limits limits;
        std::string endpoint;
        std::unique_ptr<FileSystem> filesystem;
        std::mutex mutex;
        size_t index = 0;
        uint32_t offset = 0, active = 0;
        bool pumping = false, finished = false;
        XRootDStatus result;
    };

    void RangeHandler::HandleResponse( XRootDStatus *status, AnyObject *response )
    {
      std::unique_ptr<XRootDStatus> ownStatus( status );
      std::unique_ptr<AnyObject> ownResponse( response );
      if( status->IsOK() && status->code == suContinue ) return;
      VectorReadInfo *info = nullptr;
      if( response ) response->Get( info );
      if( status->IsOK() )
      {
        bool valid = info && info->GetChunks().size() == chunks.size();
        if( valid )
          for( size_t i = 0; i < chunks.size(); ++i )
          {
            const auto &actual = info->GetChunks()[i];
            valid = actual.offset == chunks[i].offset && actual.length == chunks[i].length;
            if( !valid ) break;
          }
        if( !valid ) *status = XRootDStatus( stError, errDataError, 0, "Incomplete range read" );
      }
      reader->Completed( *status );
      delete this;
    }
  }

  XRootDStatus File::ReadRanges( const ChunkList &chunks, ResponseHandler *handler,
                                uint16_t parallel, time_t timeout )
  {
    if( !handler || !parallel || timeout < 0 ) return XRootDStatus( stError, errInvalidArgs );
    for( const auto &chunk : chunks )
      if( (chunk.length && !chunk.buffer) ||
          chunk.offset > std::numeric_limits<uint64_t>::max() - chunk.length )
        return XRootDStatus( stError, errInvalidArgs );
    if( !IsOpen() ) return XRootDStatus( stError, errInvalidOp );
    std::make_shared<RangeReader>( *this, chunks, handler, parallel, timeout )->Start();
    return XRootDStatus();
  }

  XRootDStatus File::ReadRanges( const ChunkList &chunks, uint16_t parallel, time_t timeout )
  {
    SyncResponseHandler handler;
    auto status = ReadRanges( chunks, &handler, parallel, timeout );
    return status.IsOK() ? MessageUtils::WaitForStatus( &handler ) : status;
  }
}
