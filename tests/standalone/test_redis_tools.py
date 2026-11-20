"""Run built tools against real Redis in a disposable root container.

SWSSCONFIG_BINARY and SWSSPLAYER_BINARY select the built executables. Run with
pytest --noconftest tests/standalone/test_redis_tools.py. The fixture refuses
existing SONiC database, policy, or SWSS configuration files.
"""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import resource
import secrets
import shutil
import socket
import subprocess
import tempfile
import time

import pytest
import redis


CONFIG = Path('/var/run/redis/sonic-db/database_config.json')
POLICY = Path('/run/redis-auth')
DEFAULT_CONFIG = Path('/etc/swss/config.d')
SECRET = POLICY / 'local-system/writer.secret'
USERNAME = 'tools-writer'
READ_RULES = ('reset on nopass ~* &* -@all +@read +@transaction '
              '+ping +echo +select +quit +hello +client|setname '
              '+client|getname +client|id +client|info '
              '+info +command +role +time +lastsave +config|get '
              '+acl|whoami +subscribe +psubscribe +ssubscribe '
              '+unsubscribe +punsubscribe +sunsubscribe +pubsub '
              '+eval +evalsha +eval_ro +evalsha_ro +script|exists +script|load')


def port_number():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Fixture:
    def __init__(self, directory, protected):
        self.directory = directory
        self.protected = protected
        self.password = secrets.token_hex(24)
        self.socket = str(directory / 'primary.sock')
        self.port = 0 if protected else port_number()
        self.remote_port = port_number()
        self.servers = []

    def start_server(self, name, port, password=None):
        socket_path = str(self.directory / (name + '.sock'))
        args = ['redis-server', '--bind', '127.0.0.1', '--port', str(port),
                '--unixsocket', socket_path, '--unixsocketperm', '777',
                '--save', '', '--appendonly', 'no']
        if password:
            acl = self.directory / (name + '.acl')
            acl.write_text('user default ' + READ_RULES + '\nuser ' + USERNAME
                           + ' on >' + password + ' ~* &* +@all\n')
            acl.chmod(0o600)
            args += ['--aclfile', str(acl)]
        server = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.STDOUT)
        self.servers.append(server)
        client = redis.Redis(unix_socket_path=socket_path,
                             username=USERNAME if password else None,
                             password=password, decode_responses=True)
        for _ in range(300):
            try:
                client.ping()
                return client
            except redis.ConnectionError:
                if server.poll() is not None:
                    pytest.fail('Redis exited during fixture startup')
                time.sleep(0.01)
        pytest.fail('Redis did not start')

    def metadata(self, remote_port=None):
        CONFIG.write_text(json.dumps({
            'INSTANCES': {
                'redis': {'hostname': '127.0.0.1', 'port': self.port,
                          'unix_socket_path': self.socket},
                'remote': {'hostname': '127.0.0.1',
                           'port': self.remote_port if remote_port is None else remote_port,
                           'unix_socket_path': self.socket}},
            'DATABASES': {
                'APPL_DB': {'id': 0, 'separator': ':', 'instance': 'redis'},
                'DPU_APPL_DB': {'id': 0, 'separator': ':', 'instance': 'remote'}}}))
        CONFIG.chmod(0o644)

    def payload(self, tool, key):
        path = self.directory / (key + ('.json' if tool == 'swssconfig' else '.rec'))
        if tool == 'swssconfig':
            path.write_text(json.dumps([{'TOOLS:' + key: {'field': 'value'}, 'OP': 'SET'}]))
        else:
            path.write_text('timestamp|TOOLS:' + key + '|SET|field:value\n')
        path.chmod(0o644)
        return path

    def run(self, tool, key, ordinary=False, endpoint=None, default=False):
        binary = os.environ[tool.upper() + '_BINARY']
        path = self.payload(tool, key)
        args = [binary]
        if endpoint:
            args += ['-e', endpoint]
        if default:
            shutil.copy2(path, DEFAULT_CONFIG / 'test.json')
        else:
            args.append(str(path))

        def child_setup():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            if ordinary:
                os.setgroups([])
                os.setgid(65534)
                os.setuid(65534)

        return subprocess.run(args, capture_output=True, text=True, timeout=15,
                              preexec_fn=child_setup, close_fds=True)


@pytest.fixture
def tools():
    if os.geteuid() != 0:
        pytest.skip('requires a disposable root container')
    for tool in ('SWSSCONFIG_BINARY', 'SWSSPLAYER_BINARY'):
        if not os.environ.get(tool):
            pytest.skip(tool + ' must select the built executable')
    for path in (CONFIG, POLICY, DEFAULT_CONFIG, Path('/etc/swss/orch_zmq_tables.conf')):
        if path.exists() or path.is_symlink():
            pytest.fail('refusing existing runtime fixture: ' + str(path))
    created_dirs = []
    for directory in (CONFIG.parent, DEFAULT_CONFIG):
        missing = []
        current = directory
        while not current.exists():
            missing.append(current)
            current = current.parent
        for current in reversed(missing):
            current.mkdir(mode=0o755)
            created_dirs.append(current)

    @contextmanager
    def environment(protected):
        directory = Path(tempfile.mkdtemp(prefix='swss-tools-', dir='/tmp'))
        directory.chmod(0o755)
        state = Fixture(directory, protected)
        try:
            state.primary = state.start_server('primary', state.port,
                                                state.password if protected else None)
            state.raw = redis.Redis(unix_socket_path=state.socket,
                                    username=USERNAME if protected else None,
                                    password=state.password if protected else None)
            state.remote = state.start_server('remote', state.remote_port)
            state.metadata()
            if protected:
                SECRET.parent.mkdir(parents=True, mode=0o755)
                (POLICY / 'local.json').write_text(json.dumps({
                    'username': USERNAME, 'unix_socket': state.socket,
                    'tcp_host': '127.0.0.1', 'tcp_port': 0}))
                SECRET.write_text(state.password)
                SECRET.chmod(0o400)
            yield state
        finally:
            for server in state.servers:
                server.terminate()
                server.wait(timeout=10)
            shutil.rmtree(directory)
            if POLICY.exists():
                shutil.rmtree(POLICY)
    try:
        yield environment
    finally:
        CONFIG.unlink(missing_ok=True)
        (DEFAULT_CONFIG / 'test.json').unlink(missing_ok=True)
        for directory in reversed(created_dirs):
            directory.rmdir()


@pytest.mark.parametrize('tool', ['swssconfig', 'swssplayer'])
def test_protected_local_writer_and_reader_denial(tools, tool):
    with tools(True) as state:
        result = state.run(tool, 'root')
        assert result.returncode == 0, result.stdout + result.stderr
        assert state.primary.hgetall('_TOOLS:root') == {'field': 'value'}
        assert state.primary.smembers('TOOLS_KEY_SET') == {'root'}
        before = {key: state.raw.dump(key) for key in state.raw.keys('*')}
        state.primary.acl_log_reset()
        result = state.run(tool, 'ordinary', ordinary=True)
        # swssconfig's existing buffered flush does not return server errors.
        # Check the actual ACL denial and stored data, not its exit status.
        denied = state.primary.acl_log()
        assert any(row['username'] == 'default' and row['object'] == 'sadd'
                   and row['reason'] == 'command' for row in denied)
        if tool == 'swssplayer':
            assert result.returncode != 0
            assert 'no permissions' in result.stdout + result.stderr
        assert not state.primary.exists('_TOOLS:ordinary')
        assert state.primary.hgetall('_TOOLS:root') == {'field': 'value'}
        assert state.primary.smembers('TOOLS_KEY_SET') == {'root'}
        assert {key: state.raw.dump(key) for key in state.raw.keys('*')} == before
        assert not state.remote.keys('*TOOLS*')


@pytest.mark.parametrize('tool', ['swssconfig', 'swssplayer'])
@pytest.mark.parametrize('ordinary', [False, True])
def test_legacy_local_clients_work_with_tcp_enabled(tools, tool, ordinary):
    with tools(False) as state:
        result = state.run(tool, 'legacy', ordinary=ordinary)
        assert result.returncode == 0, result.stdout + result.stderr
        assert state.primary.hgetall('_TOOLS:legacy') == {'field': 'value'}
        assert not state.remote.keys('*TOOLS*')


def test_swssconfig_uses_local_socket_with_a_positive_metadata_port(tools):
    with tools(False) as state:
        config = json.loads(CONFIG.read_text())
        config['INSTANCES']['redis']['port'] = port_number()
        CONFIG.write_text(json.dumps(config))
        result = state.run('swssconfig', 'socket')
        assert result.returncode == 0, result.stdout + result.stderr
        assert state.primary.hgetall('_TOOLS:socket') == {'field': 'value'}


def test_default_config_directory_uses_local_socket(tools):
    with tools(True) as state:
        result = state.run('swssconfig', 'default', default=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert state.primary.hgetall('_TOOLS:default') == {'field': 'value'}


def test_explicit_dpu_endpoint_keeps_remote_tcp_and_no_local_credentials(tools):
    with tools(True) as state:
        SECRET.unlink()
        result = state.run('swssconfig', 'remote', endpoint='tcp://127.0.0.2')
        assert result.returncode == 0, result.stdout + result.stderr
        assert state.remote.hgetall('_TOOLS:remote') == {'field': 'value'}
        assert not state.primary.exists('_TOOLS:remote')


def test_explicit_dpu_port_zero_has_no_socket_fallback(tools):
    with tools(True) as state:
        state.metadata(remote_port=0)
        result = state.run('swssconfig', 'strict', endpoint='tcp://127.0.0.2')
        assert result.returncode != 0
        assert 'connect' in (result.stdout + result.stderr).lower()
        assert not state.primary.exists('_TOOLS:strict')
        assert not state.remote.exists('_TOOLS:strict')


@pytest.mark.parametrize('tool', ['swssconfig', 'swssplayer'])
def test_local_port_zero_missing_socket_fails(tools, tool):
    with tools(True) as state:
        config = json.loads(CONFIG.read_text())
        config['INSTANCES']['redis']['unix_socket_path'] = str(state.directory / 'missing.sock')
        CONFIG.write_text(json.dumps(config))
        result = state.run(tool, 'missing')
        assert result.returncode != 0
        assert not state.primary.keys('*TOOLS*')
        assert not state.remote.keys('*TOOLS*')
