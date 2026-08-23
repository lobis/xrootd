//------------------------------------------------------------------------------
// Unit tests for XrdXrootdGSReal construction failures.
//------------------------------------------------------------------------------

#include "XrdXrootd/XrdXrootdGSReal.hh"
#include "XrdXrootd/XrdXrootdMonitor.hh"

#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

int main()
{
   XrdSysLogger logger;
   XrdSysError  eDest(&logger, "test");
   XrdXrootdGSReal::GSParms parms =
      {"test", "127.0.0.1:invalid", 0, 1024, 600, 'T', 0,
       XrdXrootdGSReal::fmtCgi, XrdXrootdGSReal::hdrNorm};
   bool aOK;

   XrdXrootdMonitor::Init(0, &eDest, "localhost", "xrootd", "test", 1094);
   XrdXrootdGSReal *stream = new XrdXrootdGSReal(parms, aOK);

   if (aOK)
      {delete stream;
       return 1;
      }
   delete stream;
   return XrdXrootdMonitor::Hello::Hail() ? 1 : 0;
}
