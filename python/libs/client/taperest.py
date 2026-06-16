# This file intentionally uses the historical Python style used in this tree.
from __future__ import absolute_import, division, print_function

import json

from pyxrootd import client
from XRootD.client.responses import XRootDStatus
from XRootD.client.responses import TapeRestEndpoint, TapeRestArchiveInfo
from XRootD.client.responses import TapeRestStageResponse, TapeRestStageStatus

try:
  string_types = (basestring,)
except NameError:
  string_types = (str,)

class TapeRestClient(object):
  """Synchronous client for the WLCG Tape REST API.

  :param  timeout: Maximum HTTP operation time in seconds
  :param     cert: User certificate or proxy path
  :param      key: User private key path
  :param verbosity: Verbosity level used for HTTP debugging
  """

  def __init__(self, timeout=-1, cert='', key='', verbosity=0):
    self.timeout = timeout
    self.cert = cert
    self.key = key
    self.verbosity = verbosity

  def _normalize_stage_files(self, files):
    normalized = []
    for item in files:
      if isinstance(item, string_types):
        normalized.append((item, '', '', ''))
        continue

      url = item.get('url', '')
      path = item.get('path', '')
      disk_lifetime = item.get('diskLifetime',
                               item.get('disk_lifetime', ''))
      targeted_metadata = item.get('targetedMetadata',
                                   item.get('targeted_metadata', ''))
      if targeted_metadata and not isinstance(targeted_metadata, string_types):
        targeted_metadata = json.dumps(targeted_metadata, separators=(',', ':'))
      normalized.append((url, path, disk_lifetime, targeted_metadata))
    return normalized

  def _derive_url(self, files):
    if not files:
      return ''
    first = files[0]
    if isinstance(first, string_types):
      return first
    return first.get('url', '')

  def discover(self, url):
    """Discover the Tape REST API endpoint for a storage URL.

    :returns: tuple containing :mod:`XRootD.client.responses.XRootDStatus`
              and :mod:`XRootD.client.responses.TapeRestEndpoint`
    """
    status, endpoint = client.TapeRestDiscover_cpp(
      url, self.timeout, self.cert, self.key, self.verbosity)
    if endpoint:
      endpoint = TapeRestEndpoint(endpoint)
    return XRootDStatus(status), endpoint

  def stage(self, url, files=None):
    """Submit a Tape REST stage request.

    :param url: Storage URL used for endpoint discovery, or the file list if
                each file entry contains a URL
    :param files: Sequence of file URLs or dictionaries with ``url`` or
                  ``path``, optional ``diskLifetime``, and optional
                  ``targetedMetadata``
    """
    if files is None:
      files = list(url)
      url = self._derive_url(files)
    else:
      files = list(files)
    status, response = client.TapeRestStage_cpp(
      url, self._normalize_stage_files(files), self.timeout, self.cert,
      self.key, self.verbosity)
    if response:
      response = TapeRestStageResponse(response)
    return XRootDStatus(status), response

  def stage_status(self, url, request_id):
    """Poll the status of a previously submitted Tape REST stage request."""
    status, response = client.TapeRestStageStatus_cpp(
      url, request_id, self.timeout, self.cert, self.key, self.verbosity)
    if response:
      response = TapeRestStageStatus(response)
    return XRootDStatus(status), response

  def stage_cancel(self, url, request_id, paths):
    """Cancel a subset of files from a Tape REST stage request."""
    status = client.TapeRestStageCancel_cpp(
      url, request_id, paths, self.timeout, self.cert, self.key,
      self.verbosity)
    return XRootDStatus(status)

  def stage_delete(self, url, request_id):
    """Delete a Tape REST stage request."""
    status = client.TapeRestStageDelete_cpp(
      url, request_id, self.timeout, self.cert, self.key, self.verbosity)
    return XRootDStatus(status)

  def release(self, url, request_id, paths):
    """Release disk-latency requirements for paths in a stage request."""
    status = client.TapeRestRelease_cpp(
      url, request_id, paths, self.timeout, self.cert, self.key,
      self.verbosity)
    return XRootDStatus(status)

  def archive_info(self, urls):
    """Query archive locality information for one or more storage URLs.

    :returns: tuple containing :mod:`XRootD.client.responses.XRootDStatus`
              and a list of :mod:`XRootD.client.responses.TapeRestArchiveInfo`
    """
    status, results = client.TapeRestArchiveInfo_cpp(
      urls, self.timeout, self.cert, self.key, self.verbosity)
    return XRootDStatus(status), [TapeRestArchiveInfo(r) for r in results]
