import os
import shutil
import tempfile

from XRootD import client
import XRootD.client.copyprocess as copyprocess
from XRootD.client.flags import OpenFlags
from env import *

def test_copy_smallfile():
  f = client.File()
  s, r = f.open(smallfile, OpenFlags.DELETE)
  assert s.ok
  f.write(smallbuffer)
  size1 = f.stat(force=True)[1].size
  s, r = f.close()
  assert s.ok

  c = client.CopyProcess()
  c.add_job(source=smallfile, target=smallcopy, force=True)
  s = c.prepare()
  assert s.ok
  s, __ = c.run()
  assert s.ok

  f = client.File()
  s, r = f.open(smallcopy, OpenFlags.READ)
  size2 = f.stat()[1].size

  assert size1 == size2
  f.close()

def test_create_bigfile():
  f = client.File()
  s, r = f.open(bigfile, OpenFlags.DELETE)
  assert s.ok

  for i in range(1000):
    f.write(smallbuffer)
  size1 = f.stat(force=True)[1].size
  s, r = f.close()
  assert s.ok

def test_copy_bigfile():
  f = client.File()
  s, r = f.open(bigfile)
  assert s.ok
  size1 = f.stat(force=True)[1].size
  f.close()

  c = client.CopyProcess()
  c.add_job(source=bigfile, target=bigcopy, force=True)
  s = c.prepare()
  assert s.ok
  s, __ = c.run()
  assert s.ok

  f = client.File()
  s, r = f.open(bigcopy, OpenFlags.READ)
  size2 = f.stat()[1].size

  assert size1 == size2
  f.close()

def test_copy_nojobs():
  c = client.CopyProcess()
  s = c.prepare()
  assert s.ok
  s, __ = c.run()
  assert s.ok

def test_copy_noprep():
  c = client.CopyProcess()
  c.add_job(source=bigfile, target=bigcopy, force=True)
  s, __ = c.run()
  assert s.ok

def test_recursive_copy():
  root = tempfile.mkdtemp(prefix='pyxrootd-recursive-')
  try:
    source = os.path.join(root, 'source')
    nested = os.path.join(source, 'nested')
    target = os.path.join(root, 'target')
    os.makedirs(nested)
    os.makedirs(target)
    with open(os.path.join(source, 'one.txt'), 'wb') as output:
      output.write(b'one')
    with open(os.path.join(nested, 'two.txt'), 'wb') as output:
      output.write(b'two')

    process = client.CopyProcess()
    process.add_job('file://' + source, 'file://' + target,
                    recursive=True)
    status = process.prepare()
    assert status.ok
    status, results = process.run()
    assert status.ok
    assert len(results) == 2
    assert all(result['status'].ok for result in results)

    copied = os.path.join(target, 'source')
    with open(os.path.join(copied, 'one.txt'), 'rb') as copied_file:
      assert copied_file.read() == b'one'
    with open(os.path.join(copied, 'nested', 'two.txt'), 'rb') as copied_file:
      assert copied_file.read() == b'two'
  finally:
    shutil.rmtree(root)

def test_copy_options_follow_native_order(monkeypatch):
  class CapturingCopyProcess(object):
    def add_job(self, *args):
      self.args = args

  native_process = CapturingCopyProcess()
  monkeypatch.setattr(copyprocess.client, 'CopyProcess',
                      lambda: native_process)

  process = copyprocess.CopyProcess()
  process.add_job('file:///source', 'file:///target',
                  cptimeout=11, xrateThreshold=12, xrate=13, retry=14)

  assert native_process.args[17:21] == (11, 12, 13, 14)

class TestProgressHandler(object):
  def begin(self, id, total, source, target):
    print('+++ begin(): %d, total: %d' % (id, total))
    print('+++ source: %s' % source)
    print('+++ target: %s' % target)

  def end(self, jobId, status):
    print('+++ end(): jobId: %s, status: %s'  % (jobId, status))

  def update(self, jobId, processed, total):
    print('+++ update(): jobid: %s, processed: %d, total: %d' % (jobId, processed, total))

  def should_cancel(self, jobId):
    print('+++ should_cancel(): jobid: %s' % (jobId))
    return False

def test_copy_progress_handler():
  c = client.CopyProcess()
  c.add_job( source=bigfile, target=bigcopy, force=True )
  s = c.prepare()
  assert s.ok

  h = TestProgressHandler()
  s, _ = c.run(handler=h)
  assert s.ok
