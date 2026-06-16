#-------------------------------------------------------------------------------
# Copyright (c) 2012-2014 by European Organization for Nuclear Research (CERN)
# Author: Michal Simon <michal.simon@cern.ch>
#-------------------------------------------------------------------------------
# This file is part of the XRootD software suite.
#
# XRootD is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# XRootD is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
#
# In applying this licence, CERN does not waive the privileges and immunities
# granted to it by virtue of its status as an Intergovernmental Organization
# or submit itself to any jurisdiction.
#------------------------------------------------------------------------------

import os

from pyxrootd import client


def EnvPutString( key, value ):
     """Sets the given key in the xrootd client environment to 
        the given value. Returns false if there is already a 
        shell-imported setting for this key, true otherwise
     """
     return client.EnvPutString_cpp( key, value )
 
def EnvGetString( key ):
     """Gets the given key from the xrootd client environment. 
        If key does not exist in the environment returns None.
     """
     return client.EnvGetString_cpp( key )

def EnvDelString( key ):
     """Deletes the given key from the xrootd client string environment.
        Returns false if there is a shell-imported setting for this key,
        true otherwise.
     """
     return client.EnvDelString_cpp( key )
 
def EnvPutInt( key, value ):
     """Sets the given key in the xrootd client environment to 
        the given value. Returns false if there is already a 
        shell-imported setting for this key, true otherwise
     """
     return client.EnvPutInt_cpp( key, value )
 
def EnvGetInt( key ):
     """Gets the given key from the xrootd client environment. 
        If key does not exist in the environment returns None.
     """
     return client.EnvGetInt_cpp( key )

def EnvDelInt( key ):
     """Deletes the given key from the xrootd client integer environment.
        Returns false if there is a shell-imported setting for this key,
        true otherwise.
     """
     return client.EnvDelInt_cpp( key )

def EnvGetDefault( key ):
     """ Get the default value for the given key.
         If key does not exist in the environment returns None.
     """
     return client.EnvGetDefault_cpp( key )

def SetLogLevel( value ):
     """ Set the client log level. """
     return client.SetLogLevel_cpp( value )

def SetLogMask( level, value ):
     """ Set the log mask for a given client log level. """
     return client.SetLogMask_cpp( level, value )


class EnvContext(object):
     """Temporarily set XRootD client environment values.

        This helper updates both the Python process environment and the global
        XRootD client environment for the lifetime of the context. It is intended
        for client integrations that need scoped configuration of authentication
        and copy options around a group of operations. Since the underlying
        XRootD client environment is global, avoid overlapping contexts with
        different values in different threads.
     """

     def __init__( self, strings=None, integers=None ):
          self.strings = strings or {}
          self.integers = integers or {}
          self._old_environ = {}
          self._old_strings = {}
          self._old_integers = {}

     def __enter__( self ):
          for key, value in self.strings.items():
               self._old_environ[key] = os.environ.get( key )
               self._old_strings[key] = EnvGetString( key )
               if value is not None:
                    os.environ[key] = str( value )
                    EnvPutString( key, str( value ) )

          for key, value in self.integers.items():
               self._old_integers[key] = EnvGetInt( key )
               if value is not None:
                    EnvPutInt( key, int( value ) )

          return self

     def __exit__( self, exc_type, exc_value, traceback ):
          for key, value in self._old_environ.items():
               if value is None:
                    os.environ.pop( key, None )
               else:
                    os.environ[key] = value

          for key, value in self._old_strings.items():
               if value is None:
                    EnvDelString( key )
               else:
                    EnvPutString( key, value )

          for key, value in self._old_integers.items():
               if value is None:
                    EnvDelInt( key )
               else:
                    EnvPutInt( key, value )

          return False


def AuthContext( bearer_token=None, x509_proxy=None, sec_protocol=None ):
     """Return an :class:`EnvContext` configured for common auth settings.

        :param bearer_token: bearer token to expose as ``BEARER_TOKEN``
        :param x509_proxy:   proxy path to expose as ``X509_USER_PROXY``
        :param sec_protocol: value for ``XrdSecPROTOCOL``. If omitted, ``ztn``
                             is selected for bearer tokens and ``gsi`` for
                             X.509 proxies.
     """
     if sec_protocol is None:
          if bearer_token:
               sec_protocol = 'ztn'
          elif x509_proxy:
               sec_protocol = 'gsi'

     values = {}
     if sec_protocol:
          values['XrdSecPROTOCOL'] = sec_protocol
     if bearer_token:
          values['BEARER_TOKEN'] = bearer_token
     if x509_proxy:
          values['X509_USER_PROXY'] = x509_proxy

     return EnvContext( strings=values )
