//------------------------------------------------------------------------------
// Copyright (c) 2012-2025 by European Organization for Nuclear Research (CERN)
// Author: Justin Salmon <jsalmon@cern.ch>
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

#include "PyXRootD.hh"
#include "PyXRootDFile.hh"
#include "AsyncResponseHandler.hh"
#include "ChunkIterator.hh"
#include "Utils.hh"

#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include <limits>
#include <mutex>
#include <vector>

namespace PyXRootD
{
  struct FileIOState
  {
    std::mutex mutex;
    size_t pending = 0;
    std::vector<PyObject *> waiters;
  };

  // Own the file and its buffers in the extension, independently of the
  // Python callback's closure. In particular, cancellation cannot unpin a
  // readinto target or destroy a file still used by a native request.
  class OwnedIO : public XrdCl::ResponseHandler
  {
    public:
      enum Kind { Read, ReadInto, Write, Ranges, Vector };
      OwnedIO( File *file, PyObject *callback, Kind kind, PyObject *data ):
        file( file ), callback( callback ), kind( kind ), data( data )
      {
        Py_INCREF( file );
        Py_XINCREF( callback );
        Py_XINCREF( data );
        std::lock_guard<std::mutex> lock( file->io->mutex );
        ++file->io->pending;
      }
      ~OwnedIO()
      {
        if( view.obj ) PyBuffer_Release( &view );
        Py_XDECREF( data );
        Py_XDECREF( callback );
        std::vector<PyObject *> waiters;
        {
          std::lock_guard<std::mutex> lock( file->io->mutex );
          if( !--file->io->pending ) waiters.swap( file->io->waiters );
        }
        for( auto waiter : waiters )
        {
          XrdCl::XRootDStatus status;
          PyObject *pystatus = ConvertType( &status );
          PyObject *result = pystatus ? PyObject_CallFunctionObjArgs(
            waiter, pystatus, Py_None, nullptr ) : nullptr;
          if( !result ) PyErr_WriteUnraisable( waiter );
          Py_XDECREF( result );
          Py_XDECREF( pystatus );
          Py_DECREF( waiter );
          Py_DECREF( file ); // Reference retained by Drain.
        }
        Py_DECREF( file );
      }

      PyObject *Result( XrdCl::AnyObject *response )
      {
        if( kind == Write ) { Py_RETURN_NONE; }
        if( kind == Ranges ) { Py_INCREF( data ); return data; }
        if( kind == Vector )
        {
          XrdCl::VectorReadInfo *info = nullptr;
          if( response ) response->Get( info );
          if( !info ) { Py_RETURN_NONE; }
          const auto &chunks = info->GetChunks();
          PyObject *list = PyList_New( chunks.size() );
          if( !list ) return nullptr;
          for( size_t i = 0; i < chunks.size(); ++i )
          {
            if( i >= static_cast<size_t>( PyList_Size( data ) ) ||
                chunks[i].length > PyBytes_Size( PyList_GET_ITEM( data, i ) ) )
            {
              Py_DECREF( list );
              PyErr_SetString( PyExc_RuntimeError, "Invalid vector read response" );
              return nullptr;
            }
            PyObject *buffer = PyList_GET_ITEM( data, i );
            if( chunks[i].length != PyBytes_Size( buffer ) )
              buffer = PyBytes_FromStringAndSize( PyBytes_AS_STRING( buffer ), chunks[i].length );
            else Py_INCREF( buffer );
            if( !buffer ) { Py_DECREF( list ); return nullptr; }
            PyObject *item = Py_BuildValue( "{sKsIsN}", "offset", chunks[i].offset,
                                           "length", chunks[i].length, "buffer", buffer );
            if( !item ) { Py_DECREF( list ); return nullptr; }
            PyList_SET_ITEM( list, i, item );
          }
          return Py_BuildValue( "{sIsN}", "size", info->GetSize(), "chunks", list );
        }
        XrdCl::ChunkInfo *chunk = nullptr;
        if( response ) response->Get( chunk );
        uint32_t count = chunk ? chunk->length : 0;
        Py_ssize_t capacity = kind == ReadInto ? view.len : PyBytes_Size( data );
        if( count > capacity )
        {
          PyErr_SetString( PyExc_RuntimeError, "Invalid read response" );
          return nullptr;
        }
        if( kind == ReadInto ) return PyLong_FromUnsignedLong( count );
        if( count == capacity ) { Py_INCREF( data ); return data; }
        return PyBytes_FromStringAndSize( PyBytes_AS_STRING( data ), count );
      }

      void HandleResponse( XrdCl::XRootDStatus *status, XrdCl::AnyObject *response ) override
      {
        if( !Py_IsInitialized() ) return;
        auto gil = PyGILState_Ensure();
        if( status->IsOK() && status->code == XrdCl::suContinue )
        {
          delete status;
          delete response;
          PyGILState_Release( gil );
          return;
        }
        PyObject *pystatus = ConvertType( status );
        PyObject *result = status->IsOK() ? Result( response ) : nullptr;
        if( !result && PyErr_Occurred() )
        {
          // Conversion failures must complete the waiter, not only print an
          // exception on the native callback thread.
          PyErr_Clear();
          Py_XDECREF( pystatus );
          XrdCl::XRootDStatus failed( XrdCl::stError, XrdCl::errInternal );
          pystatus = ConvertType( &failed );
        }
        if( !result ) { result = Py_None; Py_INCREF( result ); }
        PyObject *called = pystatus ? PyObject_CallFunctionObjArgs(
          callback, pystatus, result, nullptr ) : nullptr;
        if( !called ) PyErr_WriteUnraisable( callback );
        Py_XDECREF( called );
        Py_XDECREF( pystatus );
        Py_DECREF( result );
        delete status;
        delete response;
        delete this;
        PyGILState_Release( gil );
      }
      File *file;
      PyObject *callback;
      Kind kind;
      PyObject *data;
      Py_buffer view = {};
  };

  static bool CheckCallback( PyObject *callback )
  {
    if( !callback || callback == Py_None || PyCallable_Check( callback ) ) return true;
    PyErr_SetString( PyExc_TypeError, "callback must be callable" );
    return false;
  }

  template<typename T> class FileResponseHandler : public AsyncResponseHandler<T>
  {
    public:
      FileResponseHandler( File *file, PyObject *callback, PyObject *arguments ):
        AsyncResponseHandler<T>( callback ),
        lifetime( new OwnedIO( file, nullptr, OwnedIO::Write, arguments ) ) {}
      ~FileResponseHandler() override { delete lifetime; }
    private:
      OwnedIO *lifetime;
  };

  template<typename T> static XrdCl::ResponseHandler *GetFileHandler(
    File *file, PyObject *callback, PyObject *arguments )
  {
    if( !IsCallable( callback ) ) return nullptr;
    return new FileResponseHandler<T>( file, callback, arguments );
  }

  static bool IONumbers( PyObject *pyoffset, PyObject *pysize, PyObject *pytimeout,
                         unsigned long long &offset, unsigned int &size,
                         unsigned short &timeout )
  {
    unsigned long long value = 0;
    if( pyoffset && PyObjToUllong( pyoffset, &offset, "offset" ) ) return false;
    if( pysize && PyObjToUint( pysize, &size, "size" ) ) return false;
    if( pytimeout && PyObjToUllong( pytimeout, &value, "timeout" ) ) return false;
    if( value > 65535 )
    {
      PyErr_SetString( PyExc_OverflowError, "timeout exceeds 16 bits" );
      return false;
    }
    timeout = value;
    return true;
  }

  static PyObject *IOResult( const XrdCl::XRootDStatus &status,
                            PyObject *callback, PyObject *response = nullptr )
  {
    PyObject *pystatus = ConvertType( const_cast<XrdCl::XRootDStatus *>( &status ) );
    if( !pystatus ) { Py_XDECREF( response ); return nullptr; }
    if( callback && callback != Py_None ) return pystatus;
    if( !response ) { response = Py_None; Py_INCREF( response ); }
    return Py_BuildValue( "NN", pystatus, response );
  }

  //----------------------------------------------------------------------------
  //! Set exception and return null if I/O op on closed file is attempted
  //----------------------------------------------------------------------------
  static PyObject* FileClosedError()
  {
    PyErr_SetString( PyExc_ValueError, "I/O operation on closed file" );
    return NULL;
  }

  //----------------------------------------------------------------------------
  //! __init__() equivalent
  //----------------------------------------------------------------------------
  static int File_init( File *self, PyObject *args )
  {
    self->file    = new XrdCl::File();
    self->currentOffset = 0;
    self->io = new FileIOState();
    return 0;
  }

  //----------------------------------------------------------------------------
  //! Deallocation function, called when object is deleted
  //----------------------------------------------------------------------------
  static void File_dealloc( File *self )
  {
    // Destruction may close an open handle. Native callback workers must be
    // able to acquire the GIL while that close drains.
    async( delete self->file );
    delete self->io;
    Py_TYPE(self)->tp_free( (PyObject*) self );
  }

  //----------------------------------------------------------------------------
  //! __iter__
  //----------------------------------------------------------------------------
  static PyObject* File_iter( File *self )
  {
    if ( !self->file->IsOpen() ) return FileClosedError();

    //--------------------------------------------------------------------------
    // Return ourselves for iteration
    //--------------------------------------------------------------------------
    Py_INCREF( self );
    return (PyObject*) self;
  }

  //----------------------------------------------------------------------------
  //! __iternext__
  //----------------------------------------------------------------------------
  static PyObject* File_iternext( File *self )
  {
    if ( !self->file->IsOpen() ) return FileClosedError();

    PyObject *line = PyObject_CallMethod( (PyObject*) self,
                                          const_cast<char*>("readline"), NULL );
    if( !line ) return NULL;
    //--------------------------------------------------------------------------
    // Raise StopIteration if the line we just read is empty
    //--------------------------------------------------------------------------
    if ( PyUnicode_GET_LENGTH( line ) == 0 ) {
      PyErr_SetNone( PyExc_StopIteration );
      return NULL;
    }

    return line;
  }

  //----------------------------------------------------------------------------
  //! __enter__
  //----------------------------------------------------------------------------
  static PyObject* File_enter( File *self )
  {
    Py_INCREF( self );
    return (PyObject*) self;
  }

  //----------------------------------------------------------------------------
  //! __exit__
  //----------------------------------------------------------------------------
  static PyObject* File_exit( File *self )
  {
    PyObject *ret = PyObject_CallMethod( (PyObject*) self,
                                         const_cast<char*>("close"), NULL );
    if ( !ret ) return NULL;
    Py_DECREF( ret );
    Py_RETURN_NONE ;
  }

  //----------------------------------------------------------------------------
  //! Visible method definition
  //----------------------------------------------------------------------------
  static PyMethodDef FileMethods[] =
  {
    { "open",
       (PyCFunction) PyXRootD::File::Open,                METH_VARARGS | METH_KEYWORDS, NULL },
    { "close",
       (PyCFunction) PyXRootD::File::Close,               METH_VARARGS | METH_KEYWORDS, NULL },
    { "stat",
       (PyCFunction) PyXRootD::File::Stat,                METH_VARARGS | METH_KEYWORDS, NULL },
    { "read",
       (PyCFunction) PyXRootD::File::Read,                METH_VARARGS | METH_KEYWORDS, NULL },
    { "readinto",
       (PyCFunction) PyXRootD::File::ReadInto, METH_VARARGS | METH_KEYWORDS, NULL },
    { "read_ranges",
       (PyCFunction) PyXRootD::File::ReadRanges, METH_VARARGS | METH_KEYWORDS, NULL },
    { "drain",
       (PyCFunction) PyXRootD::File::Drain, METH_VARARGS | METH_KEYWORDS, NULL },
    { "readline",
       (PyCFunction) PyXRootD::File::ReadLine,            METH_VARARGS | METH_KEYWORDS, NULL },
    { "readlines",
       (PyCFunction) PyXRootD::File::ReadLines,           METH_VARARGS | METH_KEYWORDS, NULL },
    { "readchunks",
       (PyCFunction) PyXRootD::File::ReadChunks,          METH_VARARGS | METH_KEYWORDS, NULL },
    { "write",
       (PyCFunction) PyXRootD::File::Write,               METH_VARARGS | METH_KEYWORDS, NULL },
    { "sync",
       (PyCFunction) PyXRootD::File::Sync,                METH_VARARGS | METH_KEYWORDS, NULL },
    { "truncate",
       (PyCFunction) PyXRootD::File::Truncate,            METH_VARARGS | METH_KEYWORDS, NULL },
    { "vector_read",
       (PyCFunction) PyXRootD::File::VectorRead,          METH_VARARGS | METH_KEYWORDS, NULL },
    { "fcntl",
       (PyCFunction) PyXRootD::File::Fcntl,               METH_VARARGS | METH_KEYWORDS, NULL },
    { "visa",
       (PyCFunction) PyXRootD::File::Visa,                METH_VARARGS | METH_KEYWORDS, NULL },
    { "is_open",
       (PyCFunction) PyXRootD::File::IsOpen,              METH_VARARGS | METH_KEYWORDS, NULL },
    { "get_property",
       (PyCFunction) PyXRootD::File::GetProperty,         METH_VARARGS | METH_KEYWORDS, NULL },
    { "set_property",
       (PyCFunction) PyXRootD::File::SetProperty,         METH_VARARGS | METH_KEYWORDS, NULL },
    { "set_xattr",
       (PyCFunction) PyXRootD::File::SetXAttr,            METH_VARARGS | METH_KEYWORDS, NULL },
    { "get_xattr",
       (PyCFunction) PyXRootD::File::GetXAttr,            METH_VARARGS | METH_KEYWORDS, NULL },
    { "del_xattr",
       (PyCFunction) PyXRootD::File::DelXAttr,            METH_VARARGS | METH_KEYWORDS, NULL },
    { "list_xattr",
       (PyCFunction) PyXRootD::File::ListXAttr,           METH_VARARGS | METH_KEYWORDS, NULL },
    { "openusingtemplate",
       (PyCFunction) PyXRootD::File::OpenUsingTemplate,   METH_VARARGS | METH_KEYWORDS, NULL },
    { "clone",
       (PyCFunction) PyXRootD::File::Clone,               METH_VARARGS | METH_KEYWORDS, NULL },
    {"__enter__",
       (PyCFunction) File_enter,                          METH_NOARGS,   NULL},
    {"__exit__",
       (PyCFunction) File_exit,                           METH_VARARGS,  NULL},

    { NULL } /* Sentinel */
  };

  //----------------------------------------------------------------------------
  //! Visible member definition
  //----------------------------------------------------------------------------
  static PyMemberDef FileMembers[] =
  {
    { NULL } /* Sentinel */
  };

  //----------------------------------------------------------------------------
  //! Docstring definition
  //----------------------------------------------------------------------------
  PyDoc_STRVAR(file_type_doc, "File object (internal)");

  //----------------------------------------------------------------------------
  //! File binding type object definition (with external linkage)
  //----------------------------------------------------------------------------
  PyTypeObject FileType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyxrootd.File",                            /* tp_name */
    sizeof(File),                               /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor) File_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
    file_type_doc,                              /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)  File_iter,                   /* tp_iter */
    (iternextfunc) File_iternext,               /* tp_iternext */
    FileMethods,                                /* tp_methods */
    FileMembers,                                /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc) File_init,                       /* tp_init */
  };

  //----------------------------------------------------------------------------
  //! Open the file pointed to by the given URL
  //----------------------------------------------------------------------------
  PyObject* File::Open( File *self, PyObject *args, PyObject *kwds )
  {
    static const char      *kwlist[] = { "url", "flags", "mode",
                                         "timeout", "callback", NULL };
    const  char            *url;
    XrdCl::OpenFlags::Flags flags    = XrdCl::OpenFlags::None;
    XrdCl::Access::Mode     mode     = XrdCl::Access::None;
    time_t                  timeout  = 0;
    PyObject               *callback = NULL, *pystatus = NULL;
    XrdCl::XRootDStatus     status;

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "s|HHHO:open",
         (char**) kwlist, &url, &flags, &mode, &timeout, &callback ) )
      return NULL;

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::AnyObject>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->Open( url, flags, mode, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      async( status = self->file->Open( url, flags, mode, timeout ) );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "ON", pystatus, Py_BuildValue( "" ) );
    Py_DECREF( pystatus );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Close the file
  //----------------------------------------------------------------------------
  PyObject* File::Close( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "timeout", "callback", NULL };
    time_t              timeout  = 0;
    PyObject           *callback = NULL, *pystatus = NULL;
    XrdCl::XRootDStatus status;

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|HO:close", (char**) kwlist,
        &timeout, &callback ) ) return NULL;

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::AnyObject>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->Close( handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      async( status = self->file->Close( timeout ) )
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "ON", pystatus, Py_BuildValue( "" ) );
    Py_DECREF( pystatus );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Obtain status information for this file
  //----------------------------------------------------------------------------
  PyObject* File::Stat( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "force", "timeout", "callback", NULL };
    int                 force    = 0;
    time_t              timeout  = 0;
    PyObject           *callback = NULL, *pyresponse = NULL, *pystatus = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|iHO:stat", (char**) kwlist,
        &force, &timeout, &callback ) ) return NULL;

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::StatInfo>( self, callback, args );
      async( status = self->file->Stat( force, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      XrdCl::StatInfo *response = 0;
      async( status = self->file->Stat( force, response, timeout ) );
      pyresponse = ConvertType<XrdCl::StatInfo>( response );
      delete response;
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Read a data chunk at a given offset
  //----------------------------------------------------------------------------
  PyObject* File::Read( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *keys[] = { "offset", "size", "timeout", "callback", nullptr };
    unsigned long long offset = 0;
    unsigned int size = 0;
    unsigned short timeout = 0;
    PyObject *callback = nullptr, *pyoffset = nullptr, *pysize = nullptr, *pytimeout = nullptr;
    if( !self->file->IsOpen() ) return FileClosedError();
    if( !PyArg_ParseTupleAndKeywords( args, kwds, "|OOOO:read", (char **)keys,
                                     &pyoffset, &pysize, &pytimeout, &callback ) ||
        !IONumbers( pyoffset, pysize, pytimeout, offset, size, timeout ) ||
        !CheckCallback( callback ) ) return nullptr;
    if( !size )
    {
      XrdCl::StatInfo *info = nullptr;
      XrdCl::XRootDStatus status;
      async( status = self->file->Stat( true, info, timeout ) );
      if( !status.IsOK() ) { delete info; return IOResult( status, callback ); }
      uint64_t length = info->GetSize();
      delete info;
      if( length > std::numeric_limits<uint32_t>::max() )
      {
        PyErr_SetString( PyExc_OverflowError, "read size exceeds 32 bits" );
        return nullptr;
      }
      size = length;
    }
    PyObject *data = PyBytes_FromStringAndSize( nullptr, size );
    if( !data ) return nullptr;
    auto owned = new OwnedIO( self, callback, OwnedIO::Read, data );
    Py_DECREF( data );
    XrdCl::XRootDStatus status;
    if( callback && callback != Py_None )
    {
      async( status = self->file->Read( offset, size, PyBytes_AS_STRING( owned->data ), owned, timeout ) );
      if( !status.IsOK() ) delete owned;
      return IOResult( status, callback );
    }
    uint32_t count = 0;
    async( status = self->file->Read( offset, size, PyBytes_AS_STRING( owned->data ), count, timeout ) );
    XrdCl::AnyObject response;
    response.Set( new XrdCl::ChunkInfo( offset, count, nullptr ) );
    PyObject *result = status.IsOK() ? owned->Result( &response ) : nullptr;
    delete owned;
    if( PyErr_Occurred() ) return nullptr;
    return IOResult( status, callback, result );
  }

  PyObject* File::ReadInto( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *keys[] = { "buffer", "offset", "timeout", "callback", nullptr };
    PyObject *buffer = nullptr, *callback = nullptr;
    unsigned long long offset = 0;
    unsigned short timeout = 0;
    if( !self->file->IsOpen() ) return FileClosedError();
    PyObject *pyoffset = nullptr, *pytimeout = nullptr;
    unsigned int unused = 0;
    if( !PyArg_ParseTupleAndKeywords( args, kwds, "O|OOO:readinto", (char **)keys,
                                     &buffer, &pyoffset, &pytimeout, &callback ) ||
        !IONumbers( pyoffset, nullptr, pytimeout, offset, unused, timeout ) ||
        !CheckCallback( callback ) ) return nullptr;
    auto owned = new OwnedIO( self, callback, OwnedIO::ReadInto, nullptr );
    if( PyObject_GetBuffer( buffer, &owned->view, PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS ) )
    { delete owned; return nullptr; }
    if( owned->view.len > std::numeric_limits<uint32_t>::max() )
    {
      delete owned;
      PyErr_SetString( PyExc_OverflowError, "readinto size exceeds 32 bits" );
      return nullptr;
    }
    XrdCl::XRootDStatus status;
    if( callback && callback != Py_None )
    {
      if( !owned->view.len ) owned->HandleResponse( new XrdCl::XRootDStatus(), nullptr );
      else
      {
        async( status = self->file->Read( offset, owned->view.len, owned->view.buf, owned, timeout ) );
        if( !status.IsOK() ) delete owned;
      }
      return IOResult( status, callback );
    }
    uint32_t count = 0;
    if( owned->view.len )
      async( status = self->file->Read( offset, owned->view.len, owned->view.buf, count, timeout ) );
    delete owned;
    return IOResult( status, callback, PyLong_FromUnsignedLong( count ) );
  }

  PyObject* File::Drain( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *keys[] = { "callback", nullptr };
    PyObject *callback = nullptr;
    if( !PyArg_ParseTupleAndKeywords( args, kwds, "O:drain", (char **)keys, &callback ) ||
        !CheckCallback( callback ) ) return nullptr;
    if( callback == Py_None )
    {
      PyErr_SetString( PyExc_TypeError, "drain requires a callback" );
      return nullptr;
    }
    bool pending;
    {
      std::lock_guard<std::mutex> lock( self->io->mutex );
      pending = self->io->pending != 0;
      if( pending )
      {
        Py_INCREF( callback );
        Py_INCREF( self );
        self->io->waiters.push_back( callback );
      }
    }
    XrdCl::XRootDStatus status;
    if( !pending )
    {
      PyObject *pystatus = ConvertType( &status );
      if( !pystatus ) return nullptr;
      PyObject *called = PyObject_CallFunctionObjArgs( callback, pystatus, Py_None, nullptr );
      Py_DECREF( pystatus );
      if( !called ) return nullptr;
      Py_DECREF( called );
    }
    return IOResult( status, callback );
  }


  //----------------------------------------------------------------------------
  // Read a data chunk at a given offset, until the first newline encountered
  // or size data read.
  //----------------------------------------------------------------------------
  PyObject* File::ReadLine( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *kwlist[]  = { "offset", "size", "chunksize", NULL };
    uint64_t           offset    = 0;
    uint32_t           size      = 0;
    uint32_t           chunksize = 0;
    PyObject          *pyline    = NULL;
    PyObject          *py_offset = NULL, *py_size = NULL, *py_chunksize = NULL;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|OOO:readline",
        (char**) kwlist, &py_offset, &py_size, &py_chunksize ) ) return NULL;

    unsigned long long tmp_offset = 0;
    unsigned int tmp_size = 0, tmp_chunksize = 0;

    if ( py_offset && PyObjToUllong(py_offset, &tmp_offset, "offset" ) )
      return NULL;

    if ( py_size && PyObjToUint(py_size, &tmp_size, "size" ) )
      return NULL;

    if ( py_chunksize && PyObjToUint(py_chunksize, &tmp_chunksize, "chunksize" ) )
      return NULL;

    offset = (uint64_t)tmp_offset;
    size = (uint32_t)tmp_size;
    chunksize = (uint32_t)tmp_chunksize;
    uint64_t off_init = offset;

    if (offset == 0)
      offset = self->currentOffset;
    else
      self->currentOffset = offset;

    // Default chunk size is 2MB or equal to size if size less then 2MB
    if ( !chunksize ) chunksize = 1024 * 1024 * 2;
    if ( !size ) size = UINT_MAX;
    if ( size < chunksize ) chunksize = size;

    uint64_t off_end = offset + size;
    std::unique_ptr<XrdCl::Buffer> chunk;
    std::unique_ptr<XrdCl::Buffer> line = std::make_unique<XrdCl::Buffer>();

    while ( offset < off_end )
    {
      chunk.reset( self->ReadChunk( self, offset, chunksize ) );
      offset += chunk->GetSize();

      // Reached end of file
      if ( !chunk->GetSize() )
        break;

      // Check if we read a new line
      bool found_newline = false;

      for( uint32_t i = 0; i < chunk->GetSize(); ++i )
      {
        chunk->SetCursor( i );

        // Stop if found newline or read required amount of data
        if( ( *chunk->GetBufferAtCursor() == '\n') ||
            ( line->GetSize() + i >= size))
        {
          found_newline = true;
          line->Append( chunk->GetBuffer(), i + 1 );
          break;
        }
      }

      if ( !found_newline )
        line->Append( chunk->GetBuffer(), chunk->GetSize() );
      else
        break;
    }

    if ( line->GetSize() != 0 )
    {
      // Update file offset if default readline call
      if ( off_init == 0 )
        self->currentOffset += line->GetSize();

      pyline = PyUnicode_FromStringAndSize( line->GetBuffer(), line->GetSize() );
    }
    else
      pyline = PyUnicode_FromString( "" );

    return pyline;
  }

  //----------------------------------------------------------------------------
  //! Read data chunks from a given offset, separated by newlines, until EOF
  //! encountered. Return list of lines read. A max read size can be specified,
  //! but it should be noted that using this method is probably a bad idea.
  //----------------------------------------------------------------------------
  PyObject* File::ReadLines( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *kwlist[]  = { "offset", "size", "chunksize", NULL };
    uint64_t           offset    = 0;
    uint32_t           size      = 0;
    uint32_t           chunksize = 0;
    PyObject *py_offset = NULL, *py_size = NULL, *py_chunksize = NULL;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|kII:readlines",
          (char**) kwlist, &offset, &size, &chunksize ) ) return NULL;

    unsigned long long tmp_offset = 0;
    unsigned int tmp_size = 0, tmp_chunksize = 0;

    if ( py_offset && PyObjToUllong( py_offset, &tmp_offset, "offset" ) )
      return NULL;

    if ( py_size && PyObjToUint( py_size, &tmp_size, "size" ) )
      return NULL;

    if ( py_chunksize && PyObjToUint( py_chunksize, &tmp_chunksize, "chunksize" ) )
      return NULL;

    offset = (uint64_t)tmp_offset;
    size = (uint32_t)tmp_size;
    chunksize = (uint16_t)tmp_chunksize;

    PyObject *lines = PyList_New( 0 );
    PyObject *line  = NULL;

    for (;;)
    {
      line = self->ReadLine( self, args, kwds );

      if ( !line || PyUnicode_GET_LENGTH( line ) == 0 )
        break;

      PyList_Append( lines, line );
      Py_DECREF( line );
    }

    return lines;
  }

  //----------------------------------------------------------------------------
  //! Read a chunk of the given size from the given offset as a string
  //----------------------------------------------------------------------------
  XrdCl::Buffer* File::ReadChunk( File *self, uint64_t offset, uint32_t size )
  {
    XrdCl::XRootDStatus status;
    XrdCl::Buffer      *buffer;
    XrdCl::Buffer      *temp;
    uint32_t            bytesRead = 0;

    temp = new XrdCl::Buffer( size );
    status = self->file->Read( offset, size, temp->GetBuffer(), bytesRead );

    buffer = new XrdCl::Buffer( bytesRead );
    buffer->Append( temp->GetBuffer(), bytesRead );
    delete temp;
    return buffer;
  }

  //----------------------------------------------------------------------------
  //! Read data chunks from a given offset of the given size, until EOF
  //! encountered. Return chunk iterator.
  //----------------------------------------------------------------------------
  PyObject* File::ReadChunks( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *kwlist[]  = { "offset", "chunksize", NULL };
    uint64_t           offset    = 0;
    uint32_t           chunksize = 0;
    ChunkIterator     *iterator;
    PyObject          *py_offset = NULL, *py_chunksize = NULL;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|OO:readchunks",
         (char**) kwlist, &py_offset, &py_chunksize ) ) return NULL;

    unsigned long long tmp_offset = 0;
    unsigned int tmp_chunksize = 1024 * 1024 *2;  // 2 MB

    if ( py_offset && PyObjToUllong( py_offset, &tmp_offset, "offset" ) )
      return NULL;

    if ( py_chunksize && PyObjToUint( py_chunksize, &tmp_chunksize, "chunksize" ) )
      return NULL;

    offset = (uint64_t)tmp_offset;
    chunksize = (uint32_t)tmp_chunksize;
    ChunkIteratorType.tp_new = PyType_GenericNew;

    if ( PyType_Ready( &ChunkIteratorType ) < 0 ) return NULL;

    args = Py_BuildValue( "ONN", self, Py_BuildValue("k", offset),
                                       Py_BuildValue("I", chunksize) );
    iterator = (ChunkIterator*)
               PyObject_CallObject( (PyObject *) &ChunkIteratorType, args );
    Py_DECREF( args );
    if ( !iterator ) return NULL;

    return (PyObject *) iterator;
  }

  //----------------------------------------------------------------------------
  //! Write a data chunk at a given offset
  //----------------------------------------------------------------------------
  PyObject* File::Write( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *keys[] = { "buffer", "offset", "size", "timeout", "callback", "buffer_offset", nullptr };
    PyObject *buffer = nullptr, *callback = nullptr;
    unsigned long long offset = 0;
    unsigned int size = 0;
    unsigned short timeout = 0;
    if( !self->file->IsOpen() ) return FileClosedError();
    PyObject *pyoffset = nullptr, *pysize = nullptr, *pytimeout = nullptr, *pystart = nullptr;
    unsigned long long start = 0;
    if( !PyArg_ParseTupleAndKeywords( args, kwds, "O|OOOOO:write", (char **)keys,
                                     &buffer, &pyoffset, &pysize, &pytimeout, &callback, &pystart ) ||
        !IONumbers( pyoffset, pysize, pytimeout, offset, size, timeout ) ||
        (pystart && PyObjToUllong( pystart, &start, "buffer_offset" )) ||
        !CheckCallback( callback ) ) return nullptr;
    PyObject *data = nullptr;
    if( PyBytes_Check( buffer ) ) { data = buffer; Py_INCREF( data ); }
    else if( PyUnicode_Check( buffer ) )
    {
      data = PyUnicode_AsUTF8String( buffer );
      if( !data ) return nullptr;
    }
    else
    {
      Py_buffer view = {};
      if( PyObject_GetBuffer( buffer, &view, PyBUF_CONTIG_RO ) ) return nullptr;
      data = PyBytes_FromStringAndSize( static_cast<char *>( view.buf ), view.len );
      PyBuffer_Release( &view );
      if( !data ) return nullptr;
    }
    Py_ssize_t length = PyBytes_Size( data );
    if( start > static_cast<uint64_t>( length ) ||
        (!size && length - start > std::numeric_limits<uint32_t>::max()) || size > length - start )
    {
      Py_DECREF( data );
      PyErr_SetString( PyExc_ValueError, "write size exceeds buffer or 32 bits" );
      return nullptr;
    }
    if( !size ) size = length - start;
    auto owned = new OwnedIO( self, callback, OwnedIO::Write, data );
    Py_DECREF( data );
    XrdCl::XRootDStatus status;
    if( callback && callback != Py_None )
    {
      async( status = self->file->Write( offset, size, PyBytes_AS_STRING( owned->data ) + start, owned, timeout ) );
      if( !status.IsOK() ) delete owned;
    }
    else
    {
      async( status = self->file->Write( offset, size, PyBytes_AS_STRING( owned->data ) + start, timeout ) );
      delete owned;
    }
    return IOResult( status, callback );
  }


  //----------------------------------------------------------------------------
  //! Commit all pending disk writes
  //----------------------------------------------------------------------------
  PyObject* File::Sync( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "timeout", "callback", NULL };
    time_t              timeout  = 0;
    PyObject           *callback = NULL, *pystatus = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|HO:sync", (char**) kwlist,
        &timeout, &callback ) ) return NULL;

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::AnyObject>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->Sync( handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }
    else {
      async( status = self->file->Sync( timeout ) );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "ON", pystatus, Py_BuildValue( "" ) );
    Py_DECREF( pystatus );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Truncate the file to a particular size
  //----------------------------------------------------------------------------
  PyObject* File::Truncate( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *kwlist[] = { "size", "timeout", "callback", NULL };
    uint64_t           size;
    time_t             timeout  = 0;
    PyObject          *callback = NULL, *pystatus = NULL;
    PyObject          *py_size = NULL, *py_timeout = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "O|OO:truncate",
         (char**) kwlist, &py_size, &py_timeout, &callback ) ) return NULL;

    unsigned long long tmp_size = 0;
    unsigned long long tmp_timeout = 0;

    if ( py_size && PyObjToUllong( py_size, &tmp_size, "size" ) )
      return NULL;

    if ( py_timeout && PyObjToUllong( py_timeout, &tmp_timeout, "timeout" ) )
      return NULL;

    size = (uint64_t)tmp_size;
    timeout = (time_t)tmp_timeout;

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::AnyObject>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->Truncate( size, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      async( status = self->file->Truncate( size, timeout ) );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "ON", pystatus, Py_BuildValue( "" ) );
    Py_DECREF( pystatus );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Read scattered data chunks in one operation
  //----------------------------------------------------------------------------
  static PyObject *ReadVectors( File *self, PyObject *args, PyObject *kwds, bool ranges )
  {
    static const char *rangeKeys[] = { "chunks", "timeout", "callback", "parallel", nullptr };
    static const char *vectorKeys[] = { "chunks", "timeout", "callback", nullptr };
    PyObject *input = nullptr, *callback = nullptr;
    unsigned short timeout = 0, parallel = 4;
    if( !self->file->IsOpen() ) return FileClosedError();
    PyObject *pytimeout = nullptr, *pyparallel = nullptr;
    unsigned long long unusedOffset = 0;
    unsigned int unusedSize = 0, parallelValue = 4;
    if( !PyArg_ParseTupleAndKeywords( args, kwds, ranges ? "O|OOO:read_ranges" : "O|OO:vector_read",
          (char **)(ranges ? rangeKeys : vectorKeys), &input, &pytimeout, &callback, &pyparallel ) ||
        !IONumbers( nullptr, nullptr, pytimeout, unusedOffset, unusedSize, timeout ) ||
        (pyparallel && PyObjToUint( pyparallel, &parallelValue, "parallel" )) ||
        !CheckCallback( callback ) ) return nullptr;
    if( !parallelValue || parallelValue > 65535 )
    {
      PyErr_SetString( PyExc_ValueError, "parallel must be between 1 and 65535" );
      return nullptr;
    }
    parallel = parallelValue;
    if( !PyList_Check( input ) || !parallel )
    {
      PyErr_SetString( PyExc_TypeError, "chunks must be a list" );
      return nullptr;
    }
    PyObject *data = PyList_New( PyList_Size( input ) );
    if( !data ) return nullptr;
    XrdCl::ChunkList chunks;
    for( Py_ssize_t i = 0; i < PyList_Size( input ); ++i )
    {
      PyObject *item = PyList_GET_ITEM( input, i );
      unsigned long long offset = 0, size = 0;
      if( !PyTuple_Check( item ) || PyTuple_Size( item ) != 2 )
      {
        Py_DECREF( data );
        PyErr_SetString( PyExc_TypeError, "chunks must contain (offset, size) tuples" );
        return nullptr;
      }
      if( PyObjToUllong( PyTuple_GET_ITEM( item, 0 ), &offset, "offset" ) ||
          PyObjToUllong( PyTuple_GET_ITEM( item, 1 ), &size, "size" ) )
      { Py_DECREF( data ); return nullptr; }
      if( offset > std::numeric_limits<uint64_t>::max() - size ||
          size > static_cast<uint64_t>( PY_SSIZE_T_MAX ) ||
          (!ranges && size > std::numeric_limits<uint32_t>::max()) )
      {
        Py_DECREF( data );
        PyErr_SetString( PyExc_OverflowError, "range exceeds offset or buffer limits" );
        return nullptr;
      }
      PyObject *buffer = PyBytes_FromStringAndSize( nullptr, size );
      if( !buffer ) { Py_DECREF( data ); return nullptr; }
      PyList_SET_ITEM( data, i, buffer );
      uint64_t start = 0;
      do
      {
        uint32_t length = std::min<uint64_t>( size - start, std::numeric_limits<uint32_t>::max() );
        chunks.emplace_back( offset + start, length, PyBytes_AS_STRING( buffer ) + start );
        start += length;
      } while( start < size );
    }
    auto owned = new OwnedIO( self, callback, ranges ? OwnedIO::Ranges : OwnedIO::Vector, data );
    Py_DECREF( data );
    XrdCl::XRootDStatus status;
    if( callback && callback != Py_None )
    {
      if( ranges ) { async( status = self->file->ReadRanges( chunks, owned, parallel, timeout ) ); }
      else { async( status = self->file->VectorRead( chunks, nullptr, owned, timeout ) ); }
      if( !status.IsOK() ) delete owned;
      return IOResult( status, callback );
    }
    XrdCl::VectorReadInfo *info = nullptr;
    if( ranges ) { async( status = self->file->ReadRanges( chunks, parallel, timeout ) ); }
    else { async( status = self->file->VectorRead( chunks, nullptr, info, timeout ) ); }
    XrdCl::AnyObject response;
    if( info ) response.Set( info );
    PyObject *result = status.IsOK() ? owned->Result( &response ) : nullptr;
    delete owned;
    if( PyErr_Occurred() ) return nullptr;
    return IOResult( status, callback, result );
  }

  PyObject* File::VectorRead( File *self, PyObject *args, PyObject *kwds )
  {
    return ReadVectors( self, args, kwds, false );
  }

  PyObject* File::ReadRanges( File *self, PyObject *args, PyObject *kwds )
  {
    return ReadVectors( self, args, kwds, true );
  }


  //----------------------------------------------------------------------------
  // Perform a custom operation on an open file
  //----------------------------------------------------------------------------
  PyObject* File::Fcntl( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "arg", "timeout", "callback", NULL };
    const char         *buffer   = 0;
    Py_ssize_t          buffSize = 0;
    time_t              timeout  = 0;
    PyObject           *callback = NULL, *pystatus = NULL, *pyresponse = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "s#|HO:fcntl",
          (char**) kwlist, &buffer, &buffSize, &timeout, &callback ) )
      return NULL;

    XrdCl::Buffer arg; arg.Append( buffer, buffSize );

    if ( callback && callback != Py_None )
    {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::Buffer>( self, callback, args );
      if( !handler )
        return NULL;
      async( status = self->file->Fcntl( arg, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      XrdCl::Buffer *response = 0;
      async( status = self->file->Fcntl( arg, response, timeout ) );
      pyresponse = ConvertType<XrdCl::Buffer>( response );
      delete response;
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  // Perform a custom operation on an open file
  //----------------------------------------------------------------------------
  PyObject* File::Visa( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "timeout", "callback", NULL };
    time_t              timeout  = 0;
    PyObject           *callback = NULL, *pystatus = NULL, *pyresponse = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|HO:visa",
          (char**) kwlist, &timeout, &callback ) )
      return NULL;

    if ( callback && callback != Py_None )
    {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::Buffer>( self, callback, args );
      if( !handler )
        return NULL;
      async( status = self->file->Visa( handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      XrdCl::Buffer *response = 0;
      async( status = self->file->Visa( response, timeout ) );
      pyresponse = ConvertType<XrdCl::Buffer>( response );
      delete response;
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Check if the file is open
  //----------------------------------------------------------------------------
  PyObject* File::IsOpen( File *self, PyObject *args, PyObject *kwds )
  {
    if ( !PyArg_ParseTuple( args, ":is_open" ) ) return NULL; // Allow no args
    return PyBool_FromLong(self->file->IsOpen());
  }

  //----------------------------------------------------------------------------
  //! Get property
  //----------------------------------------------------------------------------
  PyObject* File::GetProperty( File *self, PyObject *args, PyObject *kwds )
  {
    static const char *kwlist[] = { "name", NULL };
    char        *name = 0;
    std::string  value;

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "s:get_property",
         (char**) kwlist, &name ) ) return NULL;

    bool status = self->file->GetProperty( name, value );

    return status ? Py_BuildValue( "s", value.c_str() ) : Py_None;
  }

  //----------------------------------------------------------------------------
  //! Set property
  //----------------------------------------------------------------------------
  PyObject* File::SetProperty( File *self, PyObject *args, PyObject *kwds )
  {
    (void) FileType; // Suppress unused variable warning

    static const char *kwlist[] = { "name", "value", NULL };
    char *name  = 0;
    char *value = 0;

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "ss:set_property",
         (char**) kwlist, &name, &value ) ) return NULL;

    bool status = self->file->SetProperty( name, value );
    return status ? Py_True : Py_False;
  }

  //----------------------------------------------------------------------------
  //! Set Extended File Attributes
  //----------------------------------------------------------------------------
  PyObject* File::SetXAttr( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "attrs", "timeout", "callback", NULL };

    std::vector<XrdCl::xattr_t>  attrs;
    time_t timeout = 0;

    PyObject    *callback = NULL, *pystatus   = NULL;
    PyObject    *pyattrs  = NULL, *pyresponse = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "O|HO:set_xattr",
         (char**) kwlist, &pyattrs, &timeout, &callback ) ) return NULL;

    // it should be a list
    if( !PyList_Check( pyattrs ) )
      return NULL;

    // now parse the input
    Py_ssize_t size = PyList_Size( pyattrs );
    attrs.reserve( size );
    for( ssize_t i = 0; i < size; ++i )
    {
      // get the item at respective index
      PyObject *item = PyList_GetItem( pyattrs, i );
      // make sure the item is a tuple
      if( !item || !PyTuple_Check( item ) )
        return NULL;
      // make sure the tuple size equals to 2
      if( PyTuple_Size( item ) != 2 )
        return NULL;
      // extract the attribute name from the tuple
      PyObject *py_name = PyTuple_GetItem( item, 0 );
      if( !PyUnicode_Check( py_name ) )
        return NULL;
      std::string name = PyUnicode_AsUTF8( py_name );
      // extract the attribute value from the tuple
      PyObject *py_value = PyTuple_GetItem( item, 1 );
      if( !PyUnicode_Check( py_value ) )
        return NULL;
      std::string value = PyUnicode_AsUTF8( py_value );
      // update the C++ list of xattrs
      attrs.push_back( XrdCl::xattr_t( name, value ) );
    }

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<std::vector<XrdCl::XAttrStatus>>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->SetXAttr( attrs, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      std::vector<XrdCl::XAttrStatus>  result;
      async( status = self->file->SetXAttr( attrs, result, timeout ) );
      pyresponse = ConvertType( &result );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Get Extended File Attributes
  //----------------------------------------------------------------------------
  PyObject* File::GetXAttr( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "attrs", "timeout", "callback", NULL };

    std::vector<std::string>  attrs;
    time_t timeout = 0;

    PyObject    *callback = NULL, *pystatus   = NULL;
    PyObject    *pyattrs  = NULL, *pyresponse = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "O|HO:set_xattr",
         (char**) kwlist, &pyattrs, &timeout, &callback ) ) return NULL;

    // it should be a list
    if( !PyList_Check( pyattrs ) )
      return NULL;

    // now parse the input
    Py_ssize_t size = PyList_Size( pyattrs );
    attrs.reserve( size );
    for( ssize_t i = 0; i < size; ++i )
    {
      // get the item at respective index
      PyObject *item = PyList_GetItem( pyattrs, i );
      // make sure the item is a string
      if( !item || !PyUnicode_Check( item ) )
        return NULL;
      std::string name = PyUnicode_AsUTF8( item );
      // update the C++ list of xattrs
      attrs.push_back( name );
    }

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<std::vector<XrdCl::XAttr>>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->GetXAttr( attrs, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      std::vector<XrdCl::XAttr>  result;
      async( status = self->file->GetXAttr( attrs, result, timeout ) );
      pyresponse = ConvertType( &result );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Delete Extended File Attributes
  //----------------------------------------------------------------------------
  PyObject* File::DelXAttr( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "attrs", "timeout", "callback", NULL };

    std::vector<std::string>  attrs;
    time_t timeout = 0;

    PyObject    *callback = NULL, *pystatus   = NULL;
    PyObject    *pyattrs  = NULL, *pyresponse = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "O|HO:set_xattr",
         (char**) kwlist, &pyattrs, &timeout, &callback ) ) return NULL;

    // it should be a list
    if( !PyList_Check( pyattrs ) )
      return NULL;

    // now parse the input
    Py_ssize_t size = PyList_Size( pyattrs );
    attrs.reserve( size );
    for( ssize_t i = 0; i < size; ++i )
    {
      // get the item at respective index
      PyObject *item = PyList_GetItem( pyattrs, i );
      // make sure the item is a string
      if( !item || !PyUnicode_Check( item ) )
        return NULL;
      std::string name = PyUnicode_AsUTF8( item );
      // update the C++ list of xattrs
      attrs.push_back( name );
    }

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<std::vector<XrdCl::XAttrStatus>>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->DelXAttr( attrs, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      std::vector<XrdCl::XAttrStatus>  result;
      async( status = self->file->DelXAttr( attrs, result, timeout ) );
      pyresponse = ConvertType( &result );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  //! List Extended File Attributes
  //----------------------------------------------------------------------------
  PyObject* File::ListXAttr( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "timeout", "callback", NULL };

    time_t timeout = 0;

    PyObject    *callback   = NULL, *pystatus = NULL;
    PyObject    *pyresponse = NULL;
    XrdCl::XRootDStatus status;

    if ( !self->file->IsOpen() ) return FileClosedError();

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "|HO:set_xattr",
         (char**) kwlist, &timeout, &callback ) ) return NULL;

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<std::vector<XrdCl::XAttr>>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->ListXAttr( handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      std::vector<XrdCl::XAttr>  result;
      async( status = self->file->ListXAttr( result, timeout ) );
      pyresponse = ConvertType( &result );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "OO", pystatus, pyresponse );
    Py_DECREF( pystatus );
    Py_XDECREF( pyresponse );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Open the file pointed to by the given URL
  //! Alows one to specify template file. Required if using DUP or SAMEFS
  //! flags.
  //----------------------------------------------------------------------------
  PyObject* File::OpenUsingTemplate( File *self, PyObject *args, PyObject *kwds )
  {
    static const char      *kwlist[] = { "src_file", "url", "flags", "mode",
                                         "timeout", "callback", NULL };
    const  char            *url;
    XrdCl::OpenFlags::Flags flags    = XrdCl::OpenFlags::None;
    XrdCl::Access::Mode     mode     = XrdCl::Access::None;
    time_t                  timeout  = 0;
    PyObject               *callback = NULL, *pystatus = NULL;
    XrdCl::XRootDStatus     status;
    PyObject               *tfile   = NULL;

    // note flags must be parsed as 32 bit (unsigned int), since the DUP is the
    // first to use bits beyond the length of short. Open() was not expecting
    // flags beyond short.
    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "O!s|IHHO:open",
         (char**) kwlist, &FileType, &tfile, &url, &flags, &mode, &timeout, &callback ) )
      return NULL;

    if ( !tfile || tfile == Py_None ) {
      PyErr_SetString( PyExc_TypeError, "openusingtemplate() expects an existing file as argument" );
      return NULL;
    }

    File *fp = reinterpret_cast<File*>(tfile);

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::AnyObject>( self, callback, args );
      if ( !handler ) return NULL;
      async( status = self->file->OpenUsingTemplate( *fp->file, url, flags, mode, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }

    else {
      async( status = self->file->OpenUsingTemplate( *fp->file, url, flags, mode, timeout ) );
    }

    pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "ON", pystatus, Py_BuildValue( "" ) );
    Py_DECREF( pystatus );
    return o;
  }

  //----------------------------------------------------------------------------
  //! Clone
  //----------------------------------------------------------------------------
  PyObject* File::Clone( File *self, PyObject *args, PyObject *kwds )
  {
    static const char  *kwlist[] = { "locs", "timeout", "callback", NULL };

    PyObject    *locs_list   = NULL;
    time_t              timeout  = 0;
    PyObject           *callback = NULL;

    XrdCl::XRootDStatus status;

    if ( !PyArg_ParseTupleAndKeywords( args, kwds, "O!|HO:clone",
         (char**) kwlist, &PyList_Type, &locs_list, &timeout, &callback ) ) return NULL;

    if ( !locs_list || locs_list == Py_None ) {
      PyErr_SetString( PyExc_TypeError, "clone() expects a list of locations" );
      return NULL;
    }

    XrdCl::CloneLocations locs;
    std::unique_ptr<PyObject,decltype(&Py_DecRef)> k1
      {Py_BuildValue( "s", "src_file" ), &Py_DecRef};
    std::unique_ptr<PyObject,decltype(&Py_DecRef)> k2
      {Py_BuildValue( "s", "src_offset" ), &Py_DecRef};
    std::unique_ptr<PyObject,decltype(&Py_DecRef)> k3
      {Py_BuildValue( "s", "src_length" ), &Py_DecRef};
    std::unique_ptr<PyObject,decltype(&Py_DecRef)> k4
      {Py_BuildValue( "s", "dest_offset" ), &Py_DecRef};

    for(Py_ssize_t i=0;i<PyList_Size(locs_list);i++) {
      PyObject *loc_dict = PyList_GetItem(locs_list, i);
      if (!loc_dict || loc_dict == Py_None || !PyDict_Check(loc_dict)) {
        PyErr_Format( PyExc_TypeError,
                      "clone() list of locations at index %l is not a dictionary",
                      (long)i );
        return NULL;
      }
      PyObject *tfile = PyDict_GetItem(loc_dict, k1.get());
      if (!tfile || tfile == Py_None || !PyObject_TypeCheck(tfile, &FileType)) {
        PyErr_Format( PyExc_TypeError,
                      "clone() list of locations at index %l dictionary 'src_file' key is missing or is wrong type",
                      (long)i );
        return NULL;
      }
      PyObject *srcoffs = PyDict_GetItem(loc_dict, k2.get());
      if (!srcoffs || srcoffs == Py_None) {
        PyErr_Format( PyExc_TypeError,
                      "clone() list of locations at index %l dictionary 'src_offset' key missing",
                      (long)i );
        return NULL;
      }
      PyObject *srclen = PyDict_GetItem(loc_dict, k3.get());
      if (!srclen || srclen == Py_None) {
        PyErr_Format( PyExc_TypeError,
                      "clone() list of locations at index %l dictionary 'src_length' key missing",
                      (long)i );
        return NULL;
      }
      PyObject *dstoffs = PyDict_GetItem(loc_dict, k4.get());
      if (!dstoffs || dstoffs == Py_None) {
        PyErr_Format( PyExc_TypeError,
                      "clone() list of locations at index %l dictionary 'dest_offset' key missing",
                      (long)i );
        return NULL;
      }
      File *fp = reinterpret_cast<File*>(tfile);
      unsigned long long tmp_l1 = 0;
      unsigned long long tmp_l2 = 0;
      unsigned long long tmp_l3 = 0;
      if ( PyObjToUllong(dstoffs, &tmp_l1, "dest_offset") ) {
        // error message already set
        return NULL;
      }
      if ( PyObjToUllong(srcoffs, &tmp_l2, "src_offset") ) {
        // error message already set
        return NULL;
      }
      if ( PyObjToUllong(srclen, &tmp_l3, "src_length") ) {
        // error message already set
        return NULL;
      }
      locs.Add(*fp->file, (off_t)tmp_l1, (off_t)tmp_l2, (off_t)tmp_l3);
    }

    if ( callback && callback != Py_None ) {
      XrdCl::ResponseHandler *handler = GetFileHandler<XrdCl::ChunkInfo>( self, callback, args );
      if ( !handler ) {
        return NULL;
      }
      async( status = self->file->Clone( locs, handler, timeout ) );
      if( !status.IsOK() ) delete handler;
    }
    else {
      async( status = self->file->Clone( locs, timeout ) );
    }

    PyObject *pystatus = ConvertType<XrdCl::XRootDStatus>( &status );
    PyObject *o = ( callback && callback != Py_None ) ?
            Py_BuildValue( "O", pystatus ) :
            Py_BuildValue( "ON", pystatus, Py_BuildValue( "" ) );
    Py_DECREF( pystatus );
    return o;
  }
}
