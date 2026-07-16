//------------------------------------------------------------------------------
// Copyright (c) 2026 by European Organization for Nuclear Research (CERN)
//------------------------------------------------------------------------------
// This file is part of the XRootD software suite.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//------------------------------------------------------------------------------

#include "XrdCl/XrdClFSCompatibility.hh"

#include <gtest/gtest.h>

TEST( XrdClFSCompatibility, ParsesLegacySymbolicAccessModes )
{
  XrdCl::Access::Mode mode = XrdCl::Access::None;

  EXPECT_EQ( XrdCl::ParseAccessMode( mode, "rwxr-x---" ),
             XrdCl::AccessModeFormat::Symbolic );
  EXPECT_EQ( static_cast<unsigned int>( mode ), 0750U );

  EXPECT_EQ( XrdCl::ParseAccessMode( mode, "---------" ),
             XrdCl::AccessModeFormat::Symbolic );
  EXPECT_EQ( mode, XrdCl::Access::None );

  // Preserve the historical parser's permissive ordering within each
  // owner/group/other permission triplet.
  EXPECT_EQ( XrdCl::ParseAccessMode( mode, "xwrxwrxwr" ),
             XrdCl::AccessModeFormat::Symbolic );
  EXPECT_EQ( static_cast<unsigned int>( mode ), 0777U );
}

TEST( XrdClFSCompatibility, ParsesUnsignedOctalAccessModes )
{
  struct TestCase
  {
    const char *input;
    unsigned int expected;
  };
  const TestCase cases[] = {
    {"0", 0000U},
    {"7", 0007U},
    {"75", 0075U},
    {"755", 0755U},
    {"0755", 0755U},
    {"000000755", 0755U},
    {"777", 0777U}
  };

  for( const TestCase &testCase : cases )
  {
    XrdCl::Access::Mode mode = XrdCl::Access::None;
    EXPECT_EQ( XrdCl::ParseAccessMode( mode, testCase.input ),
               XrdCl::AccessModeFormat::Octal ) << testCase.input;
    EXPECT_EQ( static_cast<unsigned int>( mode ), testCase.expected )
      << testCase.input;
  }
}

TEST( XrdClFSCompatibility, RejectsInvalidAccessModes )
{
  const char *invalidModes[] = {
    "", "+755", "-755", "758", "08", "1000", "7777", "755x",
    " 755", "755 ", "rwxr-x--", "rwxr-x---x", "rwxr-z---"
  };

  for( const char *input : invalidModes )
  {
    XrdCl::Access::Mode mode = XrdCl::Access::UR;
    EXPECT_EQ( XrdCl::ParseAccessMode( mode, input ),
               XrdCl::AccessModeFormat::Invalid ) << input;
    EXPECT_EQ( mode, XrdCl::Access::None ) << input;
  }
}

TEST( XrdClFSCompatibility, MapsGFALDiskAndTapeStatus )
{
  EXPECT_STREQ( XrdCl::GetGFALFileStatus( false, false ), "ONLINE" );
  EXPECT_STREQ( XrdCl::GetGFALFileStatus( true, true ), "NEARLINE" );
  EXPECT_STREQ( XrdCl::GetGFALFileStatus( false, true ),
                "ONLINE_AND_NEARLINE" );
  EXPECT_STREQ( XrdCl::GetGFALFileStatus( true, false ), "UNKNOWN" );
}
