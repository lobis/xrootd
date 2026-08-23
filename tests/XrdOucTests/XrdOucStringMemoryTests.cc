//------------------------------------------------------------------------------
// Focused memory-safety tests for XrdOucString formatting.
//------------------------------------------------------------------------------

#include "XrdOuc/XrdOucString.hh"

#include <iostream>
#include <string>

namespace
{
bool Check( bool condition, const char *message )
{
   if( !condition )
      std::cerr << message << std::endl;
   return condition;
}
}

int main()
{
   const std::string expected( 600, 'x' );
   bool success = true;

   XrdOucString member( "member sentinel" );
   int memberLength = member.form( "%s", expected.c_str() );
   success &= Check( memberLength == static_cast<int>( expected.size() ),
                     "member form returned the wrong length" );
   success &= Check( member.length() == memberLength,
                     "member form stored the wrong length" );
   success &= Check( std::string( member.c_str(), member.length() ) == expected,
                     "member form produced the wrong content" );

   XrdOucString target( "static sentinel" );
   int staticLength = XrdOucString::form( target, "%s", expected.c_str() );
   success &= Check( staticLength == static_cast<int>( expected.size() ),
                     "static form returned the wrong length" );
   success &= Check( target.length() == staticLength,
                     "static form stored the wrong length" );
   success &= Check( std::string( target.c_str(), target.length() ) == expected,
                     "static form produced the wrong content" );

   return success ? 0 : 1;
}
