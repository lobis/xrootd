import os

from XRootD import client


def test_env_context_restores_environment():
  old_value = os.environ.get('XROOTD_TEST_ENV_CONTEXT')
  old_xrootd_value = client.EnvGetString('XROOTD_TEST_ENV_CONTEXT')

  with client.EnvContext(strings={'XROOTD_TEST_ENV_CONTEXT': 'temporary'}):
    assert os.environ['XROOTD_TEST_ENV_CONTEXT'] == 'temporary'
    assert client.EnvGetString('XROOTD_TEST_ENV_CONTEXT') == 'temporary'

  assert os.environ.get('XROOTD_TEST_ENV_CONTEXT') == old_value
  assert client.EnvGetString('XROOTD_TEST_ENV_CONTEXT') == old_xrootd_value


def test_env_delete_helpers():
  assert client.EnvPutString('XROOTD_TEST_ENV_DELETE_STRING', 'temporary')
  assert client.EnvGetString('XROOTD_TEST_ENV_DELETE_STRING') == 'temporary'
  assert client.EnvDelString('XROOTD_TEST_ENV_DELETE_STRING')
  assert client.EnvGetString('XROOTD_TEST_ENV_DELETE_STRING') is None

  assert client.EnvPutInt('XROOTD_TEST_ENV_DELETE_INT', 42)
  assert client.EnvGetInt('XROOTD_TEST_ENV_DELETE_INT') == 42
  assert client.EnvDelInt('XROOTD_TEST_ENV_DELETE_INT')
  assert client.EnvGetInt('XROOTD_TEST_ENV_DELETE_INT') is None


def test_auth_context_sets_common_values():
  with client.AuthContext(bearer_token='token'):
    assert os.environ['BEARER_TOKEN'] == 'token'
    assert client.EnvGetString('XrdSecPROTOCOL') == 'ztn'
