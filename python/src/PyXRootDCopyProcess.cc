//------------------------------------------------------------------------------
// Copyright (c) 2012-2014 by European Organization for Nuclear Research (CERN)
// Author: Justin Salmon <jsalmon@cern.ch>
// Author: Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------
// This file is part of the XRootD software suite.
//
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
//
// In applying this licence, CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
//------------------------------------------------------------------------------

#include "PyXRootDCopyProcess.hh"
#include "PyXRootDCopyProgressHandler.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClDefaultEnv.hh"

#include "Conversions.hh"
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClConstants.hh>

#include <memory>
#include <thread>
#include <mutex>
#include <chrono>
#include <functional>

namespace PyXRootD
{
  namespace
  {
    struct Worker
    {
      std::thread thread;
      CopyAsyncState *state;
      std::atomic<bool> finished{false};
    };
    std::mutex workersMutex;
    std::vector<std::shared_ptr<Worker>> workers;
    bool stopping = false;

    void LaunchWorker( CopyAsyncState *state,
                       std::function<void(std::shared_ptr<Worker>)> run )
    {
      std::lock_guard<std::mutex> lock( workersMutex );
      if( stopping ) throw std::runtime_error( "XRootD is shutting down" );
      // Reap completed workers during submission so long-lived clients do not
      // retain one thread handle for every completed transfer.
      for( auto it = workers.begin(); it != workers.end(); )
      {
        if( (*it)->finished.load() )
        {
          (*it)->thread.join();
          it = workers.erase( it );
        }
        else ++it;
      }
      auto worker = std::make_shared<Worker>();
      worker->state = state;
      workers.push_back( worker );
      try { worker->thread = std::thread( [worker, run]() { run( worker ); } ); }
      catch( ... ) { workers.pop_back(); throw; }
    }
  }

  void StopCopyWorkers()
  {
    std::vector<std::shared_ptr<Worker>> pending;
    {
      std::lock_guard<std::mutex> lock( workersMutex );
      stopping = true;
      pending.swap( workers );
      for( auto worker : pending )
        if( worker->state ) worker->state->cancelled.store( true );
    }
    for( auto worker : pending ) worker->thread.join();
  }

  static bool CopyIdle( CopyProcess *self )
  {
    if( !self->asyncState->running.load() ) return true;
    PyErr_SetString( PyExc_RuntimeError, "copy process is already running" );
    return false;
  }
  //----------------------------------------------------------------------------
  // Set the number of parallel jobs
  //----------------------------------------------------------------------------
  PyObject* CopyProcess::Parallel( CopyProcess *self, PyObject *args, PyObject *kwds )
  {
    if( !CopyIdle( self ) ) return nullptr;
    static const char *kwlist[]
      = { "parallel", NULL };

    // we cannot submit a config job now because it needs to be the last one,
    // otherwise it will segv
    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "I:parallel",
         (char**) kwlist, &self->parallel ) )
      return NULL;

    XrdCl::XRootDStatus st;
    return ConvertType( &st );
  }

  //----------------------------------------------------------------------------
  // Add a job to the copy process
  //----------------------------------------------------------------------------
  PyObject* CopyProcess::AddJob( CopyProcess *self, PyObject *args, PyObject *kwds )
  {
    if( !CopyIdle( self ) ) return nullptr;
    //--------------------------------------------------------------------------
    // Initialize default parameters
    //--------------------------------------------------------------------------
    XrdCl::Env *env = XrdCl::DefaultEnv::GetEnv();

    static const char *kwlist[]
      = { "source", "target", "sourcelimit", "force", "posc",
          "coerce", "mkdir", "thirdparty", "checksummode", "checksumtype",
          "checksumpreset", "dynamicsource", "chunksize", "parallelchunks", "inittimeout",
          "tpctimeout", "rmBadCksum", "cptimeout", "xratethreshold", "xrate",
          "retry", "cont", "rtrplc", NULL };

    const char  *source;
    const char  *target;
    uint16_t     sourceLimit       = 1;
    bool         force             = false;
    bool         posc              = false;
    bool         coerce            = false;
    bool         mkdir             = false;
    const char  *thirdParty        = "none";
    const char  *checkSumMode      = "none";
    const char  *checkSumType      = "";
    const char  *checkSumPreset    = "";
    bool         dynamicSource     = false;
    bool         rmBadCksum        = false;
    long long    xRateThreshold    = 0;
    long long    xRate             = 0;
    long long    retry             = 0;
    bool         cont              = false;
    const char  *rtrplc            = "force";


    int val = XrdCl::DefaultCPChunkSize;
    env->GetInt( "CPChunkSize", val );
    uint32_t chunkSize = val;

    val = XrdCl::DefaultCPParallelChunks;
    env->GetInt( "CPParallelChunks", val );
    uint16_t parallelChunks = val;

    val = XrdCl::DefaultCPInitTimeout;
    env->GetInt( "CPInitTimeout", val );
    time_t initTimeout = val;

    val = XrdCl::DefaultCPTPCTimeout;
    env->GetInt( "CPTPCTimeout", val );
    time_t tpcTimeout = val;

    val = XrdCl::DefaultCPTimeout;
    env->GetInt( "CPTimeout", val );
    time_t cpTimeout = val;

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "ss|HbbbbssssbIHHHbHLLLbs:add_job",
         (char**) kwlist,
         &source, &target, &sourceLimit, &force, &posc,
         &coerce, &mkdir, &thirdParty, &checkSumMode, &checkSumType,
         &checkSumPreset, &dynamicSource, &chunkSize, &parallelChunks, &initTimeout,
         &tpcTimeout, &rmBadCksum, &cpTimeout, &xRateThreshold, &xRate,
         &retry, &cont, &rtrplc ) )
      return NULL;

    XrdCl::PropertyList properties;
    self->results->push_back(XrdCl::PropertyList());

    properties.Set( "source",          source          );
    properties.Set( "target",          target          );
    properties.Set( "force",           force           );
    properties.Set( "posc",            posc            );
    properties.Set( "coerce",          coerce          );
    properties.Set( "makeDir",         mkdir           );
    properties.Set( "dynamicSource",   dynamicSource   );
    properties.Set( "thirdParty",      thirdParty      );
    properties.Set( "checkSumMode",    checkSumMode    );
    properties.Set( "checkSumType",    checkSumType    );
    properties.Set( "checkSumPreset",  checkSumPreset  );
    properties.Set( "chunkSize",       chunkSize       );
    properties.Set( "parallelChunks",  parallelChunks  );
    properties.Set( "initTimeout",     initTimeout     );
    properties.Set( "tpcTimeout",      tpcTimeout      );
    properties.Set( "rmOnBadCksum",    rmBadCksum      );
    properties.Set( "cpTimeout",       cpTimeout       );
    properties.Set( "xrateThreshold",  xRateThreshold  );
    properties.Set( "xrate",           xRate           );
    properties.Set( "continue",        cont );

    env->PutInt( "CpRetry", retry );
    env->PutString( "CpRetryPolicy", rtrplc );

    if( sourceLimit > 1 )
    {
      int blockSize = XrdCl::DefaultXCpBlockSize;
      env->GetInt( "XCpBlockSize", blockSize );
      properties.Set( "xcp",          true        );
      properties.Set( "xcpBlockSize", blockSize   );
      properties.Set( "nbXcpSources", sourceLimit );
    }

    XrdCl::XRootDStatus status = self->process->AddJob(properties,
                                                       &self->results->back());

    return ConvertType( &status );
  }

  //----------------------------------------------------------------------------
  // Prepare the copy jobs
  //----------------------------------------------------------------------------
  PyObject* CopyProcess::Prepare( CopyProcess *self, PyObject *args, PyObject *kwds )
  {
    if( !CopyIdle( self ) ) return nullptr;
    // add a config job that sets the number of parallel copy jobs
    XrdCl::PropertyList processConfig;
    processConfig.Set( "jobType", "configuration" );
    processConfig.Set( "parallel", self->parallel );
    XrdCl::XRootDStatus status = self->process->AddJob(processConfig, 0);
    if( !status.IsOK() )
      return ConvertType( &status );

    status = self->process->Prepare();
    return ConvertType( &status );
  }

  //----------------------------------------------------------------------------
  // Run the copy jobs
  //----------------------------------------------------------------------------
  PyObject* CopyProcess::Run( CopyProcess *self, PyObject *args, PyObject *kwds )
  {
    if( !CopyIdle( self ) ) return nullptr;
    (void) CopyProcessType;   // Suppress unused variable warning
    static const char          *kwlist[]   = { "handler", NULL };
    PyObject                   *pyhandler  = 0;
    std::unique_ptr<XrdCl::CopyProgressHandler> handler;

    if( !PyArg_ParseTupleAndKeywords( args, kwds, "|O", (char**) kwlist,
        &pyhandler ) ) return NULL;

    handler = std::make_unique<CopyProgressHandler>( pyhandler );
    XrdCl::XRootDStatus status;

    //--------------------------------------------------------------------------
    //! Allow other threads to acquire the GIL while the copy jobs are running
    //--------------------------------------------------------------------------
    Py_BEGIN_ALLOW_THREADS
    status = self->process->Run( handler.get() );
    Py_END_ALLOW_THREADS

    PyObject *tuple = PyTuple_New(2);
    PyTuple_SetItem(tuple, 0, ConvertType(&status));
    PyTuple_SetItem(tuple, 1, ConvertType(self->results));

    return tuple;
  }

  PyObject* CopyProcess::Cancel( CopyProcess *self, PyObject *, PyObject * )
  {
    self->asyncState->cancelled.store( true );
    Py_RETURN_NONE;
  }

  PyObject* CopyProcess::RunAsync( CopyProcess *self, PyObject *args, PyObject *kwds )
  {
    static const char *keys[] = { "callback", "handler", nullptr };
    PyObject *callback = nullptr, *progress = Py_None;
    if( !PyArg_ParseTupleAndKeywords( args, kwds, "O|O:run_async", (char **)keys,
                                     &callback, &progress ) ) return nullptr;
    if( !PyCallable_Check( callback ) )
    {
      PyErr_SetString( PyExc_TypeError, "callback must be callable" );
      return nullptr;
    }
    bool idle = false;
    if( !self->asyncState->running.compare_exchange_strong( idle, true ) )
    { CopyIdle( self ); return nullptr; }
    self->asyncState->cancelled.store( false );
    Py_INCREF( self );
    Py_INCREF( callback );
    Py_INCREF( progress );
    try
    {
      LaunchWorker( self->asyncState, [self, callback, progress]( std::shared_ptr<Worker> worker )
      {
        class Handler : public CopyProgressHandler
        {
          public:
            Handler( PyObject *progress, CopyAsyncState *state ):
              CopyProgressHandler( progress == Py_None ? nullptr : progress ), state( state ) {}
            bool ShouldCancel( uint32_t job ) override
            { return state->cancelled.load() || CopyProgressHandler::ShouldCancel( job ); }
            void JobProgress( uint32_t job, uint64_t done, uint64_t total ) override
            {
              std::lock_guard<std::mutex> lock( mutex );
              auto now = std::chrono::steady_clock::now();
              if( done == total || now - last >= std::chrono::milliseconds( 50 ) )
              {
                last = now;
                CopyProgressHandler::JobProgress( job, done, total );
              }
            }
            CopyAsyncState *state;
            std::mutex mutex;
            std::chrono::steady_clock::time_point last;
        } handler( progress, self->asyncState );
        XrdCl::XRootDStatus status;
        try
        {
          XrdCl::PropertyList config;
          config.Set( "jobType", "configuration" );
          config.Set( "parallel", self->parallel );
          status = self->process->AddJob( config, nullptr );
          if( status.IsOK() ) status = self->process->Prepare();
          if( status.IsOK() && self->asyncState->cancelled.load() )
            status = XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errOperationInterrupted );
          if( status.IsOK() ) status = self->process->Run( &handler );
        }
        catch( const std::exception &error )
        { status = XrdCl::XRootDStatus( XrdCl::stError, XrdCl::errInternal, 0, error.what() ); }
        // The worker owns process, progress and callback until completion.
        // Awaitable callers drain it before shutting down their event loop.
        auto gil = PyGILState_Ensure();
        PyObject *pystatus = ConvertType( &status );
        PyObject *results = ConvertType( self->results );
        self->asyncState->running.store( false );
        PyObject *called = pystatus && results ? PyObject_CallFunctionObjArgs(
          callback, pystatus, results, nullptr ) : nullptr;
        if( !called ) PyErr_WriteUnraisable( callback );
        Py_XDECREF( called );
        Py_XDECREF( pystatus );
        Py_XDECREF( results );
        Py_DECREF( callback );
        Py_DECREF( progress );
        {
          std::lock_guard<std::mutex> lock( workersMutex );
          worker->state = nullptr;
        }
        Py_DECREF( self );
        PyGILState_Release( gil );
        worker->finished.store( true );
      } );
    }
    catch( const std::exception &error )
    {
      self->asyncState->running.store( false );
      Py_DECREF( callback );
      Py_DECREF( progress );
      Py_DECREF( self );
      PyErr_SetString( PyExc_RuntimeError, error.what() );
      return nullptr;
    }
    XrdCl::XRootDStatus status;
    return ConvertType( &status );
  }
}
