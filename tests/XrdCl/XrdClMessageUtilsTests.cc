//------------------------------------------------------------------------------
// Focused tests for XrdCl::MessageUtils path rewriting.
//------------------------------------------------------------------------------

#include "XrdCl/XrdClMessageUtils.hh"

#include "XProtocol/XProtocol.hh"

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

XrdCl::Message *MakeMoveMessage( const std::string &payload,
                                 uint16_t            sourceLength )
{
  XrdCl::Message  *msg;
  ClientMvRequest *req;
  XrdCl::MessageUtils::CreateRequest( msg, req, payload.size() );
  req->requestid = kXR_mv;
  req->arg1len = sourceLength;
  req->dlen = payload.size();
  msg->Append( payload.data(), payload.size(), sizeof( ClientMvRequest ) );
  return msg;
}
}

int main()
{
  bool success = true;
  XrdCl::URL::ParamsMap noCgi;

  {
    const std::string payload = "source-without-separator";
    XrdCl::Message *msg = MakeMoveMessage( payload, 0 );
    const std::string before( msg->GetBuffer(), msg->GetSize() );
    std::string oldPath = "unchanged path";

    XrdCl::MessageUtils::RewriteCGIAndPath( msg, noCgi, false,
                                             "/replacement", &oldPath );

    const ClientRequest *req =
      reinterpret_cast<const ClientRequest *>( msg->GetBuffer() );
    const std::string after( msg->GetBuffer(), msg->GetSize() );
    success &= Check( after == before,
                      "malformed move payload was modified" );
    success &= Check( static_cast<size_t>( req->header.dlen ) == payload.size(),
                      "malformed move length was modified" );
    success &= Check( oldPath == "unchanged path",
                      "malformed move path output was modified" );
    success &= Check( msg->GetDescription().empty(),
                      "malformed move description was regenerated" );
    delete msg;
  }

  {
    const std::string source = "source";
    const std::string payload = source + " destination";
    const std::string expected = source + " /replacement";
    XrdCl::Message *msg = MakeMoveMessage( payload, source.size() );

    XrdCl::MessageUtils::RewriteCGIAndPath( msg, noCgi, false,
                                             "/replacement" );

    const ClientMvRequest *req =
      reinterpret_cast<const ClientMvRequest *>( msg->GetBuffer() );
    const std::string actual( msg->GetBuffer( sizeof( ClientMvRequest ) ),
                              req->dlen );
    success &= Check( actual == expected,
                      "valid move destination was not rewritten" );
    success &= Check( static_cast<size_t>( req->dlen ) == expected.size(),
                      "valid move payload length is incorrect" );
    success &= Check( msg->GetSize() == sizeof( ClientMvRequest ) + expected.size(),
                      "valid move buffer size is incorrect" );
    success &= Check( static_cast<size_t>( req->arg1len ) == source.size(),
                      "valid move source length was modified" );
    delete msg;
  }

  return success ? 0 : 1;
}
