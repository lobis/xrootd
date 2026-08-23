//------------------------------------------------------------------------------
// Regression tests for malformed XrdOucGMap records.
//------------------------------------------------------------------------------

#include "XrdOuc/XrdOucGMap.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace
{
bool Check( bool condition, const char *message )
{
   if( !condition )
      std::cerr << message << std::endl;
   return condition;
}

bool WriteAll( int fd, const char *text )
{
   const size_t length = std::strlen( text );
   size_t written = 0;
   while( written < length )
   {
      ssize_t result = write( fd, text + written, length - written );
      if( result > 0 )
         written += result;
      else if( result < 0 && errno == EINTR )
         continue;
      else
         return false;
   }
   return true;
}
}

int main()
{
   char mapPath[] = "/tmp/xrdouc-gmap-XXXXXX";
   int fd = mkstemp( mapPath );
   if( fd < 0 )
      return 1;

   const char *contents =
      "/CN=missing-delimiter\n"
      "\"/CN=unterminated user\n"
      "\"\" empty-user\n"
      "\"/CN=valid\" valid-user\n";
   bool success = WriteAll( fd, contents );
   close( fd );

   XrdSysLogger logger;
   XrdSysError error( &logger, "gmap-test" );
   XrdOucGMap map( &error, mapPath, "" );
   success &= Check( map.isValid(), "map did not load" );

   char user[64];
   success &= Check( map.dn2user( "/CN=valid", user, sizeof( user ) ) == 0,
                     "valid mapping was not found" );
   success &= Check( !std::strcmp( user, "valid-user" ),
                     "valid mapping returned the wrong user" );
   success &= Check( map.dn2user( "/CN=missing-delimiter", user,
                                   sizeof( user ) ) != 0,
                     "malformed mapping was accepted" );

   unlink( mapPath );
   return success ? 0 : 1;
}
