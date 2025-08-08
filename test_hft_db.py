#!/usr/bin/env python3

import unittest
import subprocess
import sys
import shlex
import time


def run_cmd(cmd):
    args = shlex.split(cmd)
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise subprocess.CalledProcessError(
            result.returncode, cmd,
            output=result.stdout,
            stderr=result.stderr
        )
    return result.stdout.strip()


def get_redis_hash(key):
    out = run_cmd(f"redis-cli -n 1 --raw HGETALL {shlex.quote(key)}")
    items = out.splitlines()
    return dict(zip(items[0::2], items[1::2]))


def get_redis_hashes(pattern):
    keys_out = run_cmd(f"redis-cli -n 1 --raw KEYS {shlex.quote(pattern)}")
    keys = keys_out.splitlines()
    result = {}
    for key in keys:
        result[key] = get_redis_hash(key)
    return result


def create_hft_profile(name:str = "test", status:str="enabled", polling_interval:int=300):
    run_cmd(f'redis-cli -n 4 hset "HIGH_FREQUENCY_TELEMETRY_PROFILE|{name}" "stream_state" "{status}" "poll_interval" "{polling_interval}"')


def create_hft_group(profile_name:str="test", group_name:str="PORT", object_names:str="Ethernet0", object_counters:str="IF_IN_OCTETS"):
    run_cmd(f'redis-cli -n 4 hset "HIGH_FREQUENCY_TELEMETRY_GROUP|{profile_name}|{group_name}" "object_names" "{object_names}" "object_counters" "{object_counters}"')


def delete_hft_profile(name:str = "test"):
    run_cmd(f'redis-cli -n 4 del "HIGH_FREQUENCY_TELEMETRY_PROFILE|{name}"')

def delete_hft_group(profile_name:str="test", group_name:str="PORT"):
    run_cmd(f'redis-cli -n 4 del "HIGH_FREQUENCY_TELEMETRY_GROUP|{profile_name}|{group_name}"')

def get_asic_db():
    tam_transport = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM_TRANSPORT:*")
    tam_collector = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM_COLLECTOR:*")
    tam_tel_type = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM_TEL_TYPE:*")
    tam_report = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM_REPORT:*")
    tam = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM:*")
    tam_counter_subscription = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM_COUNTER_SUBSCRIPTION:*")
    tam_telemetry = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_TAM_TELEMETRY:*")
    hostif_user_defined_trap = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP:*")
    host_trap_group = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP:*")
    ports = get_redis_hashes("ASIC_STATE:SAI_OBJECT_TYPE_PORT:*")
    return {
        "tam_transport": tam_transport,
        "tam_collector": tam_collector,
        "tam_tel_type": tam_tel_type,
        "tam_report": tam_report,
        "tam": tam,
        "tam_counter_subscription": tam_counter_subscription,
        "tam_telemetry": tam_telemetry,
        "hostif_user_defined_trap": hostif_user_defined_trap,
        "host_trap_group": host_trap_group,
        "ports": ports
    }


# Groups: objects_number, stats_number
def check_asic_db(groups=[(1,1)]):
    asic_db = get_asic_db()
    assert len(asic_db["tam_transport"]) == 1, "Expected one tam transport"
    assert list(asic_db["tam_transport"].values())[0]["SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE"] == "SAI_TAM_TRANSPORT_TYPE_NONE", "Expected tam transport type to be SAI_TAM_TRANSPORT_TYPE_NONE"

    assert len(asic_db["tam_collector"]) == 1, "Expected one tam collector"
    assert "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TRANSPORT:" + list(asic_db["tam_collector"].values())[0]["SAI_TAM_COLLECTOR_ATTR_TRANSPORT"] in asic_db["tam_transport"], "Expected tam collector to reference tam transport"
    assert list(asic_db["tam_collector"].values())[0]["SAI_TAM_COLLECTOR_ATTR_LOCALHOST"] == "true", "Expected tam collector to be localhost"
    assert "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP:" + list(asic_db["tam_collector"].values())[0]["SAI_TAM_COLLECTOR_ATTR_HOSTIF_TRAP"] in asic_db["hostif_user_defined_trap"], "Expected tam collector to reference hostif user defined trap"

    assert len(asic_db["tam_tel_type"]) == len(groups), "Expected {} tam telemetry".format(len(groups))
    for i in range(len(groups)):
        assert list(asic_db["tam_tel_type"].values())[i]["SAI_TAM_TEL_TYPE_ATTR_TAM_TELEMETRY_TYPE"] == "SAI_TAM_TELEMETRY_TYPE_COUNTER_SUBSCRIPTION", "Expected tam telemetry type to be SAI_TAM_TELEMETRY_TYPE_COUNTER_SUBSCRIPTION"
        assert list(asic_db["tam_tel_type"].values())[i]["SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_PORT_STATS"] == "true", "Expected tam telemetry to be switch enable port stats"
        assert list(asic_db["tam_tel_type"].values())[i]["SAI_TAM_TEL_TYPE_ATTR_MODE"] == "SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE", "Expected tam telemetry to be mode true"
        assert "ASIC_STATE:SAI_OBJECT_TYPE_TAM_REPORT:" + list(asic_db["tam_tel_type"].values())[i]["SAI_TAM_TEL_TYPE_ATTR_REPORT_ID"] in asic_db["tam_report"], "Expected tam telemetry to reference tam report"


    assert len(asic_db["tam_report"]) == len(groups), "Expected {} tam report".format(len(groups))
    for i in range(len(groups)):
        assert list(asic_db["tam_report"].values())[i]["SAI_TAM_REPORT_ATTR_TYPE"] == "SAI_TAM_REPORT_TYPE_IPFIX", "Expected tam report type to be SAI_TAM_REPORT_TYPE_IPFIX"
        assert list(asic_db["tam_report"].values())[i]["SAI_TAM_REPORT_ATTR_REPORT_MODE"] == "SAI_TAM_REPORT_MODE_BULK", "Expected tam report mode to be SAI_TAM_REPORT_MODE_BULK"
        assert list(asic_db["tam_report"].values())[i]["SAI_TAM_REPORT_ATTR_TEMPLATE_REPORT_INTERVAL"] == "0", "Expected tam report template report interval to be 0"
        assert list(asic_db["tam_report"].values())[i]["SAI_TAM_REPORT_ATTR_REPORT_INTERVAL"] == "300", "Expected tam report report interval to be 300"


    assert len(asic_db["tam"]) == 1, "Expected one tam"
    assert "SAI_TAM_BIND_POINT_TYPE_PORT" in list(asic_db["tam"].values())[0]["SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST"], "Expected tam to have bind point type list"
    assert "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TELEMETRY:" + ":".join(list(asic_db["tam"].values())[0]["SAI_TAM_ATTR_TELEMETRY_OBJECTS_LIST"].split(":")[1:3]) in asic_db["tam_telemetry"], "Expected tam to reference tam telemetry"

    counters_number = sum([group[0] * group[1] for group in groups])

    assert len(asic_db["tam_counter_subscription"]) == counters_number, "Expected {} tam counter subscription".format(counters_number)
    for i in range(counters_number):
        assert "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TEL_TYPE:" + list(asic_db["tam_counter_subscription"].values())[i]["SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_TEL_TYPE"] in asic_db["tam_tel_type"], "Expected tam counter subscription to reference tam telemetry"
        assert "ASIC_STATE:SAI_OBJECT_TYPE_PORT:" + list(asic_db["tam_counter_subscription"].values())[i]["SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_OBJECT_ID"] in asic_db["ports"], "Expected tam counter subscription to reference port"
        # assert list(asic_db["tam_counter_subscription"].values())[0]["SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STAT_ID"] == "0", "Expected tam counter subscription stat id to be 0"
        # assert list(asic_db["tam_counter_subscription"].values())[0]["SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_LABEL"] == "1", "Expected tam counter subscription label to be 1"
        assert list(asic_db["tam_counter_subscription"].values())[0]["SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STATS_MODE"] == "SAI_STATS_MODE_READ", "Expected tam counter subscription stats mode to be SAI_STATS_MODE_READ"

    assert len(asic_db["tam_telemetry"]) == 1, "Expected one tam telemetry"
    assert list(asic_db["tam_telemetry"].values())[0]["SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST"].split(":")[0] == "1", "Expected tam telemetry collector list to be 1"
    assert "ASIC_STATE:SAI_OBJECT_TYPE_TAM_COLLECTOR:" + ":".join(list(asic_db["tam_telemetry"].values())[0]["SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST"].split(":")[1:3]) in asic_db["tam_collector"], "Expected tam telemetry to reference tam collector"
    assert list(asic_db["tam_telemetry"].values())[0]["SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST"].split(":")[0] == str(len(groups)), "Expected tam telemetry tam type list to be {}".format(len(groups))
    if len(groups) == 1:
        assert "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TEL_TYPE:" + ":".join(list(asic_db["tam_telemetry"].values())[0]["SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST"].split(":")[1:3]) in asic_db["tam_tel_type"], "Expected tam telemetry to reference tam telemetry type"

    assert len(asic_db["hostif_user_defined_trap"]) == 1, "Expected one hostif user defined trap"
    assert list(asic_db["hostif_user_defined_trap"].values())[0]["SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE"] == "SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM", "Expected hostif user defined trap type to be SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM"
    assert "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP:" + list(asic_db["hostif_user_defined_trap"].values())[0]["SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TRAP_GROUP"] in asic_db["host_trap_group"], "Expected hostif user defined trap to reference host trap group"


class TestCalculator(unittest.TestCase):
    def test_simply_hft_one_counter(self):
        create_hft_profile()
        create_hft_group()
        time.sleep(5)
        check_asic_db()
        delete_hft_group()
        check_asic_db(groups=[])
        delete_hft_profile()

    def test_hft_multiple_counters(self):
        create_hft_profile()
        create_hft_group(object_names="Ethernet0,Ethernet4,Ethernet8", object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,IF_IN_DISCARDS")
        time.sleep(5)
        check_asic_db(groups=[(3, 3)])
        delete_hft_group()
        check_asic_db(groups=[])
        delete_hft_profile()

    def test_hft_delete_group_and_rejoin(self):
        create_hft_profile()
        create_hft_group(object_names="Ethernet0,Ethernet4,Ethernet8", object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,IF_IN_DISCARDS")
        time.sleep(5)
        check_asic_db(groups=[(3, 3)])
        delete_hft_group()
        check_asic_db(groups=[])
        create_hft_group(object_names="Ethernet0,Ethernet4,Ethernet8", object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,IF_IN_DISCARDS")
        check_asic_db(groups=[(3, 3)])
        delete_hft_group()
        check_asic_db(groups=[])
        delete_hft_profile()


if __name__ == "__main__":
    unittest.main()
