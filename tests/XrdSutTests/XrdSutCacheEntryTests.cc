//------------------------------------------------------------------------------
// Unit tests for XrdSutCacheEntry copy assignment.
//------------------------------------------------------------------------------

#include "XrdSut/XrdSutCacheEntry.hh"

#include <cstring>

namespace
{
bool BufferMatches(const XrdSutCacheEntryBuf &buffer, const char *expected,
                   int length)
{
   return buffer.len == length && buffer.buf
       && !memcmp(buffer.buf, expected, length);
}
}

int main()
{
   XrdSutCacheEntry source("source", kCE_ok, 7, 1234);
   source.buf1.SetBuf("one", 3);
   source.buf2.SetBuf("two-two", 7);
   source.buf3.SetBuf("three", 5);
   source.buf4.SetBuf("four", 4);

   XrdSutCacheEntry destination("destination");
   destination = source;

   if (!destination.name || strcmp(destination.name, "source")
   ||  destination.status != kCE_ok || destination.cnt != 7
   ||  destination.mtime != 1234
   || !BufferMatches(destination.buf1, "one", 3)
   || !BufferMatches(destination.buf2, "two-two", 7)
   || !BufferMatches(destination.buf3, "three", 5)
   || !BufferMatches(destination.buf4, "four", 4)) return 1;

   XrdSutCacheEntry *same = &destination;
   destination = *same;
   return !destination.name || strcmp(destination.name, "source")
       || !BufferMatches(destination.buf1, "one", 3)
       || !BufferMatches(destination.buf4, "four", 4);
}
