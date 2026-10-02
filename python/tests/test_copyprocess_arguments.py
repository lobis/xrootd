"""Keep the Python adapter aligned with the native CopyProcess signature."""
import importlib

import pytest

copyprocess = importlib.import_module('XRootD.client.copyprocess')


@pytest.mark.parametrize('retry,threshold,rate',
                         [(3, 11000, 22000), (0, 0, 0)])
def test_copy_job_rate_and_retry_arguments(
        monkeypatch, retry, threshold, rate):
    jobs = []

    class NativeProcess:
        def add_job(self, *args):
            jobs.append(args)

    monkeypatch.setattr(copyprocess.client, 'CopyProcess', NativeProcess)
    process = copyprocess.CopyProcess()
    process.add_job('file:///source', 'file:///target', retry=retry,
                    xrateThreshold=threshold, xrate=rate, cont=True,
                    rtrplc='continue')
    assert jobs[0][18:23] == (threshold, rate, retry, True, 'continue')
    assert jobs[0][:2] == ('file:///source', 'file:///target')


def test_native_copy_with_rate_and_retry_settings(tmp_path):
    source = tmp_path / 'source'
    target = tmp_path / 'target'
    source.write_bytes(b'copy through the native extension')
    process = copyprocess.CopyProcess()
    process.add_job(source.as_uri(), target.as_uri(), retry=3,
                    xrateThreshold=0, xrate=22000)
    assert process.prepare().ok
    status, results = process.run()
    assert status.ok
    assert len(results) == 1
    assert target.read_bytes() == source.read_bytes()
