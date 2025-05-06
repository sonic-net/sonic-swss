from swsscommon import swsscommon
from swsscommon.swsscommon import CounterTable, MacsecCounter
import conftest

import time
import functools
import typing
import re
import time


def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


class Table(object):
    def __init__(self, database: conftest.DVSDatabase, table_name: str):
        self.db = database
        self.table_name = table_name

    def convert_key(self, key: str):
        return key

    def __setitem__(self, key: str, pairs: dict):
        pairs_str = {}
        for k, v in pairs.items():
            pairs_str[to_string(k)] = to_string(v)
        key = self.convert_key(key)
        if self.__getitem__(key) is None:
            self.db.create_entry(self.table_name, key, pairs_str)
        else:
            self.db.update_entry(self.table_name, key, pairs_str)

    def __getitem__(self, key: str):
        key = self.convert_key(key)
        return self.db.get_entry(self.table_name, key)

    def __delitem__(self, key: str):
        key = self.convert_key(key)
        self.db.delete_entry(self.table_name, key)

    def wait(self, key: str):
        key = self.convert_key(key)
        # return True
        return self.db.wait_for_entry(self.table_name, key)

    def wait_delete(self, key: str):
        key = self.convert_key(key)
        # return True
        return self.db.wait_for_deleted_entry(self.table_name, key)


class ProduceStateTable(object):
    def __init__(self, database: conftest.DVSDatabase, table_name: str):
        self.table = swsscommon.ProducerStateTable(
            database.db_connection,
            table_name)

    def __setitem__(self, key: str, pairs: typing.Union[dict, list, tuple]):
        pairs_str = []
        if isinstance(pairs, dict):
            pairs = pairs.items()
        for k, v in pairs:
            pairs_str.append((to_string(k), to_string(v)))
        self.table.set(key, pairs_str)

    def __delitem__(self, key: str):
        self.table.delete(key)


class AppDBTable(ProduceStateTable):
    SEPARATOR = ":"

    def __init__(self, dvs: conftest.DockerVirtualSwitch, table_name: str):
        super(AppDBTable, self).__init__(dvs.get_app_db(), table_name)


class StateDBTable(Table):
    SEPARATOR = "|"

    def __init__(self, dvs: conftest.DockerVirtualSwitch, table_name: str):
        super(StateDBTable, self).__init__(dvs.get_state_db(), table_name)

    def convert_key(self, key: str):
        return key.translate(
            str.maketrans(
                AppDBTable.SEPARATOR,
                StateDBTable.SEPARATOR))


class ConfigTable(Table):

    def __init__(self, dvs: conftest.DockerVirtualSwitch, table_name: str):
        super(ConfigTable, self).__init__(dvs.get_config_db(), table_name)


def gen_sci(macsec_system_identifier: str, macsec_port_identifier: int) -> str:
    macsec_system_identifier = macsec_system_identifier.translate(
        str.maketrans("", "", ":.-"))
    sci = "{}{}".format(
        macsec_system_identifier,
        str(macsec_port_identifier).zfill(4)).lower()
    return sci


def gen_sc_key(
        separator: str,
        port_name: str,
        macsec_system_identifier: str,
        macsec_port_identifier: int) -> str:
    sci = gen_sci(macsec_system_identifier, macsec_port_identifier)
    key = "{}{}{}".format(
        port_name,
        separator,
        sci)
    return key


def gen_sa_key(
        separator: str,
        port_name: str,
        macsec_system_identifier: str,
        macsec_port_identifier: int,
        an: int):
    sc_key = gen_sc_key(
        separator,
        port_name,
        macsec_system_identifier,
        macsec_port_identifier)
    key = "{}{}{}".format(sc_key, separator, an)
    return key


def macsec_sc(separator: str = AppDBTable.SEPARATOR):
    def inner(func: typing.Callable) -> typing.Callable:
        @functools.wraps(func)
        def wrap_func(
                self,
                port_name: str,
                macsec_system_identifier: str,
                macsec_port_identifier: int,
                *args,
                **kwargs) -> typing.Any:
            key = gen_sc_key(
                separator,
                port_name,
                macsec_system_identifier,
                macsec_port_identifier)
            return func(self, key, *args, **kwargs)
        return wrap_func
    return inner


def macsec_sa(separator: str = AppDBTable.SEPARATOR):
    def inner(func: typing.Callable) -> typing.Callable:
        @functools.wraps(func)
        def wrap_func(
                self,
                port_name: str,
                macsec_system_identifier: str,
                macsec_port_identifier: int,
                an: int,
                *args,
                **kwargs) -> typing.Any:
            key = gen_sa_key(
                separator,
                port_name,
                macsec_system_identifier,
                macsec_port_identifier,
                an)
            return func(self, key, *args, **kwargs)
        return wrap_func
    return inner


class WPASupplicantMock(object):
    def __init__(self, dvs: conftest.DockerVirtualSwitch):
        self.dvs = dvs
        self.app_port_table = AppDBTable(
            self.dvs, swsscommon.APP_MACSEC_PORT_TABLE_NAME)
        self.app_receive_sc_table = AppDBTable(
            self.dvs, swsscommon.APP_MACSEC_INGRESS_SC_TABLE_NAME)
        self.app_transmit_sc_table = AppDBTable(
            self.dvs, swsscommon.APP_MACSEC_EGRESS_SC_TABLE_NAME)
        self.app_receive_sa_table = AppDBTable(
            self.dvs, swsscommon.APP_MACSEC_INGRESS_SA_TABLE_NAME)
        self.app_transmit_sa_table = AppDBTable(
            self.dvs, swsscommon.APP_MACSEC_EGRESS_SA_TABLE_NAME)
        self.state_port_table = StateDBTable(
            self.dvs, swsscommon.STATE_MACSEC_PORT_TABLE_NAME)
        self.state_receive_sc_table = StateDBTable(
            self.dvs, swsscommon.STATE_MACSEC_INGRESS_SC_TABLE_NAME)
        self.state_transmit_sc_table = StateDBTable(
            self.dvs, swsscommon.STATE_MACSEC_EGRESS_SC_TABLE_NAME)
        self.state_receive_sa_table = StateDBTable(
            self.dvs, swsscommon.STATE_MACSEC_INGRESS_SA_TABLE_NAME)
        self.state_transmit_sa_table = StateDBTable(
            self.dvs, swsscommon.STATE_MACSEC_EGRESS_SA_TABLE_NAME)

    def init_macsec_port(self, port_name: str):
        self.app_port_table[port_name] = {
            "enable": False,
            "cipher_suite": "GCM-AES-128",
        }
        self.state_port_table.wait(port_name)

    def deinit_macsec_port(self, port_name: str):
        del self.app_port_table[port_name]
        self.state_port_table.wait_delete(port_name)

    def config_macsec_port(
            self,
            port_name: str,
            config: typing.Dict[str, typing.Any]):
        self.app_port_table[port_name] = config

    def set_macsec_control(self, port_name: str, enable: bool):
        self.app_port_table[port_name] = {"enable": True}

    @macsec_sc()
    def create_receive_sc(self, sci: str):
        self.app_receive_sc_table[sci] = {"NULL": "NULL"}
        self.state_receive_sc_table.wait(sci)

    @macsec_sc()
    def delete_receive_sc(self, sci: str):
        del self.app_receive_sc_table[sci]
        self.state_receive_sc_table.wait_delete(sci)

    @macsec_sc()
    def create_transmit_sc(self, sci: str):
        self.app_transmit_sc_table[sci] = {"encoding_an": 0}
        self.state_transmit_sc_table.wait(sci)

    @macsec_sc()
    def delete_transmit_sc(self, sci: str):
        del self.app_transmit_sc_table[sci]
        self.state_transmit_sc_table.wait_delete(sci)

    def check_valid_sa_parameter(
            self,
            sak: str,
            auth_key: str,
            lowest_acceptable_pn: int,
            ssci: int,
            salt: str) -> bool:
        # Check SAK is hex string
        int(sak, 16)
        assert(
            len(sak) == 32 or len(sak) == 64,
            "Wrong length {} sak {}".format(
                len(sak),
                sak))
        # Check auth_key is valid
        int(auth_key, 16)
        assert(
            len(auth_key) == 32,
            "Wrong length {} auth_key {}".format(
                len(auth_key),
                auth_key))
        # Check lowest acceptable packet number is valid
        assert(
            lowest_acceptable_pn > 0,
            "Wrong packet number {}".format(lowest_acceptable_pn))
        return True

    @macsec_sa()
    def create_receive_sa(
            self,
            sai: str,
            sak: str,
            auth_key: str,
            lowest_acceptable_pn: int,
            ssci: int,
            salt: str):
        assert(
            self.check_valid_sa_parameter(
                sak,
                auth_key,
                lowest_acceptable_pn,
                ssci,
                salt),
            "Wrong parameter to MACsec receive SA")
        self.app_receive_sa_table[sai] = {
            "active": False, "sak": sak, "auth_key": auth_key,
            "lowest_acceptable_pn": lowest_acceptable_pn,
            "ssci": ssci, "salt": salt}

    @macsec_sa()
    def delete_receive_sa(self, sai: str):
        del self.app_receive_sa_table[sai]
        self.state_receive_sa_table.wait_delete(sai)

    @macsec_sa()
    def set_enable_receive_sa(self, sai: str, enable: bool):
        self.app_receive_sa_table[sai] = {"active": enable}
        if enable:
            self.state_receive_sa_table.wait(sai)

    @macsec_sa()
    def create_transmit_sa(
            self,
            sai: str,
            sak: str,
            auth_key: str,
            init_pn: int,
            ssci: int,
            salt: str):
        assert(
            self.check_valid_sa_parameter(
                sak,
                auth_key,
                init_pn,
                ssci,
                salt),
            "Wrong parameter to MACsec receive SA")
        self.app_transmit_sa_table[sai] = {
            "sak": sak, "auth_key": auth_key,
            "next_pn": init_pn, "ssci": ssci, "salt": salt}

    @macsec_sa()
    def delete_transmit_sa(self, sai: str):
        del self.app_transmit_sa_table[sai]
        self.state_transmit_sa_table.wait_delete(sai)

    @macsec_sa()
    def set_macsec_pn(
            self,
            sai: str,
            pn: int):
        self.app_transmit_sa_table[sai] = {"next_pn": pn}

    @macsec_sc()
    def set_enable_transmit_sa(self, sci: str, an: int, enable: bool):
        if enable:
            self.app_transmit_sc_table[sci] = {"encoding_an": an}
            assert(
                self.state_transmit_sa_table.wait(
                    "{}{}{}".format(
                        sci,
                        StateDBTable.SEPARATOR,
                        an)))


class MACsecInspector(object):
    def __init__(self, dvs: conftest.DockerVirtualSwitch):
        self.dvs = dvs

    def __load_macsec_info(self, port_name: str) -> (bool, str):
        return self.dvs.runcmd("ip macsec show {}".format(port_name))

    def get_macsec_port(self, port_name: str) -> str:
        exitcode, info = self.__load_macsec_info(port_name)
        if exitcode != 0 or not info:
            return ""
        print(info)
        return info

    def get_macsec_sc(
            self,
            port_name: str,
            macsec_system_identifier: str,
            macsec_port_identifier: int) -> str:
        info = self.get_macsec_port(port_name)
        if not info:
            return ""
        macsec_system_identifier = macsec_system_identifier.translate(
            str.maketrans("", "", ":.-"))
        sci = "{}{}".format(
            macsec_system_identifier,
            str(macsec_port_identifier).zfill(4))
        sc_pattern = r"(TXSC|RXSC):\s*{}[ \w,]+\n?(?:\s*\d:[,\w ]+\n?)*".format(
            sci)
        info = re.search(sc_pattern, info, re.IGNORECASE)
        if not info:
            return ""
        print(info.group(0))
        return info.group(0)

    def get_macsec_sa(
            self,
            port_name: str,
            macsec_system_identifier: str,
            macsec_port_identifier: str,
            an: int) -> str:
        info = self.get_macsec_sc(
            port_name,
            macsec_system_identifier,
            macsec_port_identifier)
        if not info:
            return ""
        sa_pattern = r"\s*{}:\s*PN\s*\d+[,\w ]+\n?".format(an)
        info = re.search(sa_pattern, info, re.IGNORECASE)
        if not info:
            return ""
        print(info.group(0))
        return info.group(0)

    @macsec_sa()
    def get_macsec_xpn_counter(
            self,
            sai: str) -> int:
        counter_table = CounterTable(self.dvs.get_counters_db().db_connection)
        for i in range(3):
            r, value = counter_table.hget(
                MacsecCounter(),
                sai,
                "SAI_MACSEC_SA_ATTR_CURRENT_XPN")
            if r: return int(value)
            time.sleep(1) # wait a moment for polling counter

        return None


class TestMACsec(object):
    def init_macsec(
            self,
            wpa: WPASupplicantMock,
            port_name: str,
            local_mac_address: str,
            macsec_port_identifier: int):
        wpa.init_macsec_port(port_name)
        wpa.config_macsec_port(port_name, {"enable_protect": True})
        wpa.config_macsec_port(port_name, {"enable_encrypt": True})
        wpa.config_macsec_port(port_name, {"send_sci": True})
        wpa.config_macsec_port(
            port_name,
            {
                "enable_replay_protect": True,
                "replay_window": 0
            })
        wpa.set_macsec_control(port_name, False)
        wpa.create_transmit_sc(
            port_name,
            local_mac_address,
            macsec_port_identifier)

    def establish_macsec(
            self,
            wpa: WPASupplicantMock,
            port_name: str,
            local_mac_address: str,
            peer_mac_address: str,
            macsec_port_identifier: int,
            an: int,
            sak: str,
            packet_number: int,
            auth_key: str,
            ssci: int,
            salt: str):
        wpa.create_receive_sc(
            port_name,
            peer_mac_address,
            macsec_port_identifier)
        wpa.create_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            an,
            sak,
            auth_key,
            packet_number,
            ssci,
            salt)
        wpa.create_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            an,
            sak,
            auth_key,
            packet_number,
            ssci,
            salt)
        wpa.set_enable_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            an,
            True)
        wpa.set_macsec_control(port_name, True)
        wpa.set_enable_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            an,
            True)

    def rekey_macsec(
            self,
            wpa: WPASupplicantMock,
            port_name: str,
            local_mac_address: str,
            peer_mac_address: str,
            macsec_port_identifier: int,
            an: int,
            last_an: int,
            sak: str,
            packet_number: int,
            auth_key: str,
            ssci: int,
            salt: str):
        wpa.set_macsec_pn(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            an,
            0x00000000C0000000)
        wpa.create_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            an,
            sak,
            auth_key,
            packet_number,
            ssci,
            salt)
        wpa.create_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            an,
            sak,
            auth_key,
            packet_number,
            ssci,
            salt)
        wpa.set_enable_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            an,
            True)
        wpa.set_macsec_control(port_name, True)
        wpa.set_enable_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            an,
            True)
        wpa.set_enable_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            last_an,
            False)
        wpa.delete_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            last_an)
        wpa.set_enable_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            last_an,
            False)
        wpa.delete_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            last_an)

    def deinit_macsec(
            self,
            wpa: WPASupplicantMock,
            inspector: MACsecInspector,
            port_name: str,
            macsec_port: str,
            local_mac_address: str,
            peer_mac_address: str,
            macsec_port_identifier: int,
            last_an: int):
        wpa.set_enable_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            last_an,
            False)
        wpa.delete_receive_sa(
            port_name,
            peer_mac_address,
            macsec_port_identifier,
            last_an)
        assert(
            not inspector.get_macsec_sa(
                macsec_port,
                peer_mac_address,
                macsec_port_identifier,
                last_an))
        wpa.delete_receive_sc(
            port_name,
            peer_mac_address,
            macsec_port_identifier)
        assert(
            not inspector.get_macsec_sc(
                macsec_port,
                peer_mac_address,
                macsec_port_identifier))
        wpa.set_enable_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            last_an,
            False)
        wpa.delete_transmit_sa(
            port_name,
            local_mac_address,
            macsec_port_identifier,
            last_an)
        assert(
            not inspector.get_macsec_sa(
                macsec_port,
                local_mac_address,
                macsec_port_identifier,
                last_an))
        wpa.delete_transmit_sc(
            port_name,
            local_mac_address,
            macsec_port_identifier)
        assert(
            not inspector.get_macsec_sc(
                macsec_port,
                local_mac_address,
                macsec_port_identifier))
        wpa.deinit_macsec_port(port_name)

    def test_macsec_term_orch(self, dvs: conftest.DockerVirtualSwitch, testlog):
        port_name = "Ethernet0"
        local_mac_address = "00-15-5D-78-FF-C1"
        peer_mac_address = "00-15-5D-78-FF-C2"
        macsec_port_identifier = 1
        macsec_port = "macsec_eth1"
        sak = "0" * 32
        auth_key = "0" * 32
        packet_number = 1
        ssci = 1
        salt = "0" * 24

        wpa = WPASupplicantMock(dvs)
        inspector = MACsecInspector(dvs)

        self.init_macsec(
            wpa,
            port_name,
            local_mac_address,
            macsec_port_identifier)
        self.establish_macsec(
            wpa,
            port_name,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            0,
            sak,
            packet_number,
            auth_key,
            ssci,
            salt)
        assert(inspector.get_macsec_port(macsec_port))
        assert(
            inspector.get_macsec_sc(
                macsec_port,
                local_mac_address,
                macsec_port_identifier))
        assert(
            inspector.get_macsec_sc(
                macsec_port,
                peer_mac_address,
                macsec_port_identifier))
        assert(
            inspector.get_macsec_sa(
                macsec_port,
                local_mac_address,
                macsec_port_identifier,
                0))
        assert(
            inspector.get_macsec_sa(
                macsec_port,
                peer_mac_address,
                macsec_port_identifier,
                0))
        assert(
            inspector.get_macsec_xpn_counter(
                port_name,
                local_mac_address,
                macsec_port_identifier,
                0) == packet_number)
        assert(
            inspector.get_macsec_xpn_counter(
                port_name,
                peer_mac_address,
                macsec_port_identifier,
                0) == packet_number)
        self.rekey_macsec(
            wpa,
            port_name,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            1,
            0,
            sak,
            packet_number,
            auth_key,
            ssci,
            salt)
        assert(
            inspector.get_macsec_sa(
                macsec_port,
                local_mac_address,
                macsec_port_identifier,
                1))
        assert(
            inspector.get_macsec_sa(
                macsec_port,
                peer_mac_address,
                macsec_port_identifier,
                1))
        assert(
            inspector.get_macsec_xpn_counter(
                port_name,
                local_mac_address,
                macsec_port_identifier,
                1) == packet_number)
        assert(
            inspector.get_macsec_xpn_counter(
                port_name,
                peer_mac_address,
                macsec_port_identifier,
                1) == packet_number)
        assert(
            not inspector.get_macsec_sa(
                macsec_port,
                local_mac_address,
                macsec_port_identifier,
                0))
        assert(
            not inspector.get_macsec_sa(
                macsec_port,
                peer_mac_address,
                macsec_port_identifier,
                0))
        assert(
            not inspector.get_macsec_xpn_counter(
                port_name,
                local_mac_address,
                macsec_port_identifier,
                0) == packet_number)
        assert(
            not inspector.get_macsec_xpn_counter(
                port_name,
                peer_mac_address,
                macsec_port_identifier,
                0) == packet_number)
        # Exit MACsec port
        self.deinit_macsec(
            wpa,
            inspector,
            port_name,
            macsec_port,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            1)
        assert(not inspector.get_macsec_port(macsec_port))

    def test_macsec_attribute_change(self, dvs: conftest.DockerVirtualSwitch, testlog):
        port_name = "Ethernet0"
        local_mac_address = "00-15-5D-78-FF-C1"
        peer_mac_address = "00-15-5D-78-FF-C2"
        macsec_port_identifier = 1
        macsec_port = "macsec_eth1"
        sak = "0" * 32
        auth_key = "0" * 32
        packet_number = 1
        ssci = 1
        salt = "0" * 24

        wpa = WPASupplicantMock(dvs)
        inspector = MACsecInspector(dvs)

        self.init_macsec(
            wpa,
            port_name,
            local_mac_address,
            macsec_port_identifier)
        wpa.set_macsec_control(port_name, True)
        wpa.config_macsec_port(port_name, {"enable_encrypt": False})
        wpa.config_macsec_port(port_name, {"cipher_suite": "GCM-AES-256"})
        self.establish_macsec(
            wpa,
            port_name,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            0,
            sak,
            packet_number,
            auth_key,
            ssci,
            salt)
        macsec_info = inspector.get_macsec_port(macsec_port)
        assert("encrypt off" in macsec_info)
        assert("GCM-AES-256" in macsec_info)
        self.deinit_macsec(
            wpa,
            inspector,
            port_name,
            macsec_port,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            0)

    def test_macsec_with_portchannel(self, dvs: conftest.DockerVirtualSwitch, testlog):

        # Set MACsec enabled on Ethernet0
        ConfigTable(dvs, "PORT")["Ethernet0"] = {"macsec" : "test"}
        StateDBTable(dvs, "FEATURE")["macsec"] = {"state": "enabled"}

        # Setup Port-channel
        ConfigTable(dvs, "PORTCHANNEL")["PortChannel001"] = {"admin": "up", "mtu": "9100", "oper_status": "up"}
        time.sleep(1)

        # create port channel member
        ConfigTable(dvs, "PORTCHANNEL_MEMBER")["PortChannel001|Ethernet0"] = {"NULL": "NULL"}
        ConfigTable(dvs, "PORTCHANNEL_INTERFACE")["PortChannel001"] = {"NULL": "NULL"}
        ConfigTable(dvs, "PORTCHANNEL_INTERFACE")["PortChannel001|40.0.0.0/31"] = {"NULL": "NULL"}
        time.sleep(3)

        # Check Portchannel member in ASIC db that shouldn't been created before MACsec enabled
        lagmtbl = swsscommon.Table(swsscommon.DBConnector(1, dvs.redis_sock, 0), "ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER")
        lagms = lagmtbl.getKeys()
        assert len(lagms) == 0

        # Create MACsec session
        port_name = "Ethernet0"
        local_mac_address = "00-15-5D-78-FF-C1"
        peer_mac_address = "00-15-5D-78-FF-C2"
        macsec_port_identifier = 1
        macsec_port = "macsec_eth1"
        sak = "0" * 32
        auth_key = "0" * 32
        packet_number = 1
        ssci = 1
        salt = "0" * 24

        wpa = WPASupplicantMock(dvs)
        inspector = MACsecInspector(dvs)

        self.init_macsec(
            wpa,
            port_name,
            local_mac_address,
            macsec_port_identifier)
        self.establish_macsec(
            wpa,
            port_name,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            0,
            sak,
            packet_number,
            auth_key,
            ssci,
            salt)
        time.sleep(3)

        # Check Portchannel member in ASIC db that should been created after MACsec enabled
        lagmtbl = swsscommon.Table(swsscommon.DBConnector(1, dvs.redis_sock, 0), "ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER")
        lagms = lagmtbl.getKeys()
        assert len(lagms) == 1

        self.deinit_macsec(
            wpa,
            inspector,
            port_name,
            macsec_port,
            local_mac_address,
            peer_mac_address,
            macsec_port_identifier,
            0)

        # remove port channel member
        del ConfigTable(dvs, "PORTCHANNEL_INTERFACE")["PortChannel001"]
        del ConfigTable(dvs, "PORTCHANNEL_INTERFACE")["PortChannel001|40.0.0.0/31"]
        del ConfigTable(dvs, "PORTCHANNEL_MEMBER")["PortChannel001|Ethernet0"]

        # remove port channel
        del ConfigTable(dvs, "PORTCHANNEL")["PortChannel001"]

        # Clear MACsec enabled on Ethernet0
        ConfigTable(dvs, "PORT")["Ethernet0"] = {"macsec" : ""}


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass
