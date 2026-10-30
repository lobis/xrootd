#include "XrdClHttp/XrdClHttpUtil.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include <gtest/gtest.h>

TEST(HTTPStatusConvertTest, MapsInternalServerErrorToIOError) {
    const auto [code, errNo] = XrdClHttp::HTTPStatusConvert(500);
    EXPECT_EQ(code, XrdCl::errErrorResponse);
    EXPECT_EQ(errNo, kXR_IOError);
}

TEST(HTTPStatusConvertTest, PreservesStandardErrorMappings) {
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(400).second, kXR_InvalidRequest);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(401).second, kXR_NotAuthorized);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(403).second, kXR_NotAuthorized);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(404).second, kXR_NotFound);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(408).second, kXR_ReqTimedOut);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(501).second, kXR_Unsupported);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(503).second, kXR_Overloaded);
    EXPECT_EQ(XrdClHttp::HTTPStatusConvert(507).second, kXR_overQuota);
}
