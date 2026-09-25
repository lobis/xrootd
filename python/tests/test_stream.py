"""Standard Python file behavior over a local XRootD server."""

import io
import uuid

import pytest

from XRootD import client
from env import SERVER_URL


def remote_path():
    return SERVER_URL + '/tmp/stream-' + uuid.uuid4().hex


def remove(path):
    client.FileSystem(SERVER_URL).rm(client.URL(path).path)


def test_binary_file_object():
    path = remote_path()
    try:
        with client.open(path, 'wb') as file:
            assert isinstance(file, io.BufferedWriter)
            assert file.write(b'abcdef') == 6
            assert not file.closed
        assert file.closed

        with client.open(path, 'rb') as file:
            assert file.read(2) == b'ab'
            assert file.tell() == 2
            assert file.seek(-2, io.SEEK_END) == 4
            assert file.read() == b'ef'

        with client.open(path, 'r+b', buffering=0) as file:
            assert file.seek(2) == 2
            assert file.write(b'XY') == 2
            assert file.seek(0) == 0
            output = bytearray(6)
            assert file.readinto(output) == 6
            assert output == b'abXYef'

        with client.open(path, 'ab') as file:
            file.seek(0)
            file.write(b'!')
        with client.open(path, 'rb') as file:
            assert file.read() == b'abXYef!'
    finally:
        remove(path)


def test_text_file_object():
    path = remote_path()
    try:
        with client.open(path, 'w', encoding='utf-8') as file:
            assert isinstance(file, io.TextIOWrapper)
            file.write('éclair\nsecond\n')
        with client.open(path, 'r', encoding='utf-8') as file:
            assert list(file) == ['éclair\n', 'second\n']
    finally:
        remove(path)


def test_standard_errors_and_mode_validation():
    missing = remote_path()
    with pytest.raises(FileNotFoundError) as error:
        client.open(missing, 'rb')
    assert error.value.filename == missing
    assert error.value.xrootd_status is not None

    with pytest.raises(ValueError):
        client.open(missing, 'bb')
    with pytest.raises(ValueError):
        client.open(missing, 'r', buffering=0)
    with pytest.raises(ValueError):
        client.open(missing, 'rb', encoding='utf-8')


def test_exclusive_create_and_append_new_file():
    exclusive = remote_path()
    append = remote_path()
    try:
        with client.open(exclusive, 'xb') as file:
            file.write(b'first')
        with pytest.raises(FileExistsError):
            client.open(exclusive, 'xb')
        with client.open(append, 'ab') as file:
            file.write(b'created')
        with client.open(append, 'rb') as file:
            assert file.read() == b'created'
    finally:
        remove(exclusive)
        remove(append)


def test_seek_end_after_write_keeps_existing_bytes():
    path = remote_path()
    try:
        with client.open(path, 'w+b', buffering=0) as file:
            file.write(b'root')
            assert file.seek(0, io.SEEK_END) == 4
            file.write(b'\0' * 16)
            file.seek(0)
            assert file.read(4) == b'root'
        with client.open(path, 'rb') as file:
            assert file.read() == b'root' + b'\0' * 16
    finally:
        remove(path)
