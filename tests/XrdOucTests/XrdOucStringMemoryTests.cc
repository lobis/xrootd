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

   XrdOucString resized( "resize me" );
   resized.resize( 128 );
   success &= Check( std::string( resized.c_str(), resized.length() ) == "resize me",
                     "resize did not preserve content" );
   success &= Check( resized.capacity() >= 129,
                     "resize did not grow capacity" );
   resized.resize( 0 );
   success &= Check( resized.c_str() == 0 && resized.length() == 0
                     && resized.capacity() == 0,
                     "resize(0) did not reset the string" );

   const std::string assignedValue( 300, 'a' );
   XrdOucString assigned( "seed" );
   assigned.assign( assignedValue.c_str(), 0 );
   success &= Check( std::string( assigned.c_str(), assigned.length() ) == assignedValue,
                     "assign growth produced the wrong content" );

   const std::string appendedValue( 300, 'b' );
   XrdOucString appended( "prefix" );
   appended.append( appendedValue.c_str() );
   success &= Check( std::string( appended.c_str(), appended.length() )
                     == "prefix" + appendedValue,
                     "append growth produced the wrong content" );

   XrdOucString replaced( "a-b-a" );
   const std::string replacement( 80, 'r' );
   const std::string replacedValue = replacement + "-b-" + replacement;
   const int delta = replaced.replace( "a", replacement.c_str() );
   success &= Check( delta == static_cast<int>( replacedValue.size() - 5 ),
                     "expanding replace returned the wrong delta" );
   success &= Check( std::string( replaced.c_str(), replaced.length() )
                     == replacedValue,
                     "expanding replace produced the wrong content" );

   return success ? 0 : 1;
}
