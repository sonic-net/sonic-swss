import pytest
import logging


logging.basicConfig(level=logging.INFO)
hashlogger = logging.getLogger(__name__)


HASH_FIELD_LIST = [
    "DST_MAC",
    "SRC_MAC",
    "ETHERTYPE",
    "IP_PROTOCOL",
    "DST_IP",
    "SRC_IP",
    "L4_DST_PORT",
    "L4_SRC_PORT",
    "IPV6_FLOW_LABEL"
]
INNER_HASH_FIELD_LIST = [
    "INNER_DST_MAC",
    "INNER_SRC_MAC",
    "INNER_ETHERTYPE",
    "INNER_IP_PROTOCOL",
    "INNER_DST_IP",
    "INNER_SRC_IP",
    "INNER_L4_DST_PORT",
    "INNER_L4_SRC_PORT"
]
DEFAULT_HASH_FIELD_LIST = [
    "DST_MAC",
    "SRC_MAC",
    "ETHERTYPE",
    "IN_PORT"
]
HASH_ALGORITHM = [
    "CRC",
    "XOR",
    "RANDOM",
    "CRC_32LO",
    "CRC_32HI",
    "CRC_CCITT",
    "CRC_XOR"
]

SAI_HASH_FIELD_LIST = [
    "SAI_NATIVE_HASH_FIELD_DST_MAC",
    "SAI_NATIVE_HASH_FIELD_SRC_MAC",
    "SAI_NATIVE_HASH_FIELD_ETHERTYPE",
    "SAI_NATIVE_HASH_FIELD_IP_PROTOCOL",
    "SAI_NATIVE_HASH_FIELD_DST_IP",
    "SAI_NATIVE_HASH_FIELD_SRC_IP",
    "SAI_NATIVE_HASH_FIELD_L4_DST_PORT",
    "SAI_NATIVE_HASH_FIELD_L4_SRC_PORT",
    "SAI_NATIVE_HASH_FIELD_IPV6_FLOW_LABEL"
]
SAI_INNER_HASH_FIELD_LIST = [
    "SAI_NATIVE_HASH_FIELD_INNER_DST_MAC",
    "SAI_NATIVE_HASH_FIELD_INNER_SRC_MAC",
    "SAI_NATIVE_HASH_FIELD_INNER_ETHERTYPE",
    "SAI_NATIVE_HASH_FIELD_INNER_IP_PROTOCOL",
    "SAI_NATIVE_HASH_FIELD_INNER_DST_IP",
    "SAI_NATIVE_HASH_FIELD_INNER_SRC_IP",
    "SAI_NATIVE_HASH_FIELD_INNER_L4_DST_PORT",
    "SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT"
]
SAI_DEFAULT_HASH_FIELD_LIST = [
    "SAI_NATIVE_HASH_FIELD_DST_MAC",
    "SAI_NATIVE_HASH_FIELD_SRC_MAC",
    "SAI_NATIVE_HASH_FIELD_ETHERTYPE",
    "SAI_NATIVE_HASH_FIELD_IN_PORT"
]
SAI_HASH_ALGORITHM = [
    "SAI_HASH_ALGORITHM_CRC",
    "SAI_HASH_ALGORITHM_XOR",
    "SAI_HASH_ALGORITHM_RANDOM",
    "SAI_HASH_ALGORITHM_CRC_32LO",
    "SAI_HASH_ALGORITHM_CRC_32HI",
    "SAI_HASH_ALGORITHM_CRC_CCITT",
    "SAI_HASH_ALGORITHM_CRC_XOR"
]


@pytest.mark.usefixtures("dvs_hash_manager")
@pytest.mark.usefixtures("dvs_switch_manager")
class TestHashBasicFlows:
    @pytest.fixture(scope="class")
    def hashData(self, dvs_hash_manager):
        hashlogger.info("Initialize HASH data")

        hashlogger.info("Verify HASH count")
        self.dvs_hash.verify_hash_count(0)

        hashlogger.info("Get ECMP/LAG HASH id")
        hashIdList = sorted(self.dvs_hash.get_hash_ids())

        # Assumption: VS has only two HASH objects: ECMP, LAG
        meta_dict = {
            "ecmp": hashIdList[0],
            "lag": hashIdList[1]
        }

        yield meta_dict

        hashlogger.info("Deinitialize HASH data")

    @pytest.fixture(scope="class")
    def switchData(self, dvs_switch_manager):
        hashlogger.info("Initialize SWITCH data")

        hashlogger.info("Verify SWITCH count")
        self.dvs_switch.verify_switch_count(0)

        hashlogger.info("Get SWITCH id")
        switchIdList = self.dvs_switch.get_switch_ids()

        # Assumption: VS has only one SWITCH object
        meta_dict = {
            "id": switchIdList[0]
        }

        yield meta_dict

        hashlogger.info("Deinitialize SWITCH data")

    @pytest.mark.parametrize(
        "hash,field", [
            pytest.param(
                "ecmp",
                "ecmp_hash",
                id="ecmp-hash"
            ),
            pytest.param(
                "lag",
                "lag_hash",
                id="lag-hash"
            )
        ]
    )
    @pytest.mark.parametrize(
        "hfList,saiHfList", [
            pytest.param(
                ",".join(HASH_FIELD_LIST),
                SAI_HASH_FIELD_LIST,
                id="outer-frame"
            ),
            pytest.param(
                ",".join(INNER_HASH_FIELD_LIST),
                SAI_INNER_HASH_FIELD_LIST,
                id="inner-frame"
            )
        ]
    )
    def test_HashSwitchGlobalConfiguration(self, hash, field, hfList, saiHfList, testlog, hashData):
        attr_dict = {
            field: hfList
        }

        hashlogger.info("Update {} hash".format(hash.upper()))
        self.dvs_hash.update_switch_hash(
            qualifiers=attr_dict
        )

        hashId = hashData[hash]
        sai_attr_dict = {
            "SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST": saiHfList
        }

        hashlogger.info("Validate {} hash".format(hash.upper()))
        self.dvs_hash.verify_hash_generic(
            sai_hash_id=hashId,
            sai_qualifiers=sai_attr_dict
        )

    @pytest.mark.parametrize(
        "hash,field", [
            pytest.param(
                "ecmp",
                "ecmp_hash",
                id="ecmp-hash"
            ),
            pytest.param(
                "lag",
                "lag_hash",
                id="lag-hash"
            )
        ]
    )
    def test_HashDefaultSwitchGlobalConfiguration(self, hash, field, testlog, hashData):
        attr_dict = {
            field: ",".join(DEFAULT_HASH_FIELD_LIST)
        }

        hashlogger.info("Update {} hash".format(hash.upper()))
        self.dvs_hash.update_switch_hash(
            qualifiers=attr_dict
        )

        hashId = hashData[hash]
        sai_attr_dict = {
            "SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST": SAI_DEFAULT_HASH_FIELD_LIST
        }

        hashlogger.info("Validate {} hash".format(hash.upper()))
        self.dvs_hash.verify_hash_generic(
            sai_hash_id=hashId,
            sai_qualifiers=sai_attr_dict
        )

    @pytest.mark.parametrize(
        "algorithm,attr,field", [
            pytest.param(
                "ecmp",
                "SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM",
                "ecmp_hash_algorithm",
                id="ecmp-hash-algorithm"
            ),
            pytest.param(
                "lag",
                "SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM",
                "lag_hash_algorithm",
                id="lag-hash-algorithm"
            )
        ]
    )
    @pytest.mark.parametrize(
        "value", HASH_ALGORITHM
    )
    def test_HashAlgorithmSwitchGlobalConfiguration(self, algorithm, attr, field, value, testlog, switchData):
        attr_dict = {
            field: value
        }

        hashlogger.info("Update {} hash algorithm".format(algorithm.upper()))
        self.dvs_hash.update_switch_hash(
            qualifiers=attr_dict
        )

        switchId = switchData["id"]
        sai_attr_dict = {
            attr: SAI_HASH_ALGORITHM[HASH_ALGORITHM.index(value)]
        }

        hashlogger.info("Validate {} hash algorithm".format(algorithm.upper()))
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

    @pytest.mark.parametrize(
        "algorithm,attr,field", [
            pytest.param(
                "ecmp",
                "SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM",
                "ecmp_hash_algorithm",
                id="ecmp-hash-algorithm"
            ),
            pytest.param(
                "lag",
                "SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM",
                "lag_hash_algorithm",
                id="lag-hash-algorithm"
            )
        ]
    )
    @pytest.mark.parametrize(
        "value", [ "CRC" ]
    )
    def test_HashDefaultAlgorithmSwitchGlobalConfiguration(self, algorithm, attr, field, value, testlog, switchData):
        attr_dict = {
            field: value
        }

        hashlogger.info("Update {} hash algorithm".format(algorithm.upper()))
        self.dvs_hash.update_switch_hash(
            qualifiers=attr_dict
        )

        switchId = switchData["id"]
        sai_attr_dict = {
            attr: SAI_HASH_ALGORITHM[HASH_ALGORITHM.index(value)]
        }

        hashlogger.info("Validate {} hash algorithm".format(algorithm.upper()))
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )


PKT_TYPE_HASH_FIELD_LIST = [
    "DST_IP",
    "SRC_IP",
    "L4_DST_PORT",
    "L4_SRC_PORT"
]
SAI_PKT_TYPE_HASH_FIELD_LIST = [
    "SAI_NATIVE_HASH_FIELD_DST_IP",
    "SAI_NATIVE_HASH_FIELD_SRC_IP",
    "SAI_NATIVE_HASH_FIELD_L4_DST_PORT",
    "SAI_NATIVE_HASH_FIELD_L4_SRC_PORT"
]
RDMA_HASH_FIELD_LIST = [
    "DST_IP",
    "SRC_IP",
    "RDMA_BTH_OPCODE",
    "RDMA_BTH_DEST_QP"
]
SAI_RDMA_HASH_FIELD_LIST = [
    "SAI_NATIVE_HASH_FIELD_DST_IP",
    "SAI_NATIVE_HASH_FIELD_SRC_IP",
    "SAI_NATIVE_HASH_FIELD_RDMA_BTH_OPCODE",
    "SAI_NATIVE_HASH_FIELD_RDMA_BTH_DEST_QP"
]


@pytest.mark.usefixtures("dvs_hash_manager")
class TestHashPacketType:
    """Test per-packet-type hash configuration (ECMP and LAG)."""

    @pytest.mark.parametrize(
        "field,hfList,saiHfList", [
            pytest.param(
                "ecmp_hash_ipv4",
                ",".join(PKT_TYPE_HASH_FIELD_LIST),
                SAI_PKT_TYPE_HASH_FIELD_LIST,
                id="ecmp-ipv4"
            ),
            pytest.param(
                "lag_hash_ipv4",
                ",".join(PKT_TYPE_HASH_FIELD_LIST),
                SAI_PKT_TYPE_HASH_FIELD_LIST,
                id="lag-ipv4"
            ),
            pytest.param(
                "ecmp_hash_ipv4_rdma",
                ",".join(RDMA_HASH_FIELD_LIST),
                SAI_RDMA_HASH_FIELD_LIST,
                id="ecmp-ipv4-rdma"
            ),
            pytest.param(
                "lag_hash_ipv6_rdma",
                ",".join(RDMA_HASH_FIELD_LIST),
                SAI_RDMA_HASH_FIELD_LIST,
                id="lag-ipv6-rdma"
            ),
        ]
    )
    def test_HashPacketTypeCreate(self, field, hfList, saiHfList, testlog):
        """Configure a per-packet-type hash (including RDMA) and verify a new ASIC hash object is created."""

        hashlogger.info("Configure %s", field)
        self.dvs_hash.update_switch_hash(qualifiers={field: hfList})

        new_ids = self.dvs_hash.get_hash_ids(expected=1)
        assert len(new_ids) == 1, "Expected 1 new hash object"

        sai_attr = {"SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST": saiHfList}
        self.dvs_hash.verify_hash_generic(sai_hash_id=new_ids[0], sai_qualifiers=sai_attr)

        hashlogger.info("Cleanup %s", field)
        self.dvs_hash.delete_switch_hash_field(field)

    def test_HashPacketTypeUpdate(self, testlog):
        """Configure a per-packet-type hash, update its fields, and verify ASIC DB reflects the update."""
        field = "ecmp_hash_ipv6"

        hashlogger.info("Create %s with basic fields", field)
        self.dvs_hash.update_switch_hash(
            qualifiers={field: ",".join(PKT_TYPE_HASH_FIELD_LIST)}
        )

        new_ids = self.dvs_hash.get_hash_ids(expected=1)
        assert len(new_ids) == 1
        hash_oid = new_ids[0]

        sai_attr = {"SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST": SAI_PKT_TYPE_HASH_FIELD_LIST}
        self.dvs_hash.verify_hash_generic(sai_hash_id=hash_oid, sai_qualifiers=sai_attr)

        hashlogger.info("Update %s to RDMA fields", field)
        self.dvs_hash.update_switch_hash(
            qualifiers={field: ",".join(RDMA_HASH_FIELD_LIST)}
        )

        sai_attr_updated = {"SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST": SAI_RDMA_HASH_FIELD_LIST}
        self.dvs_hash.verify_hash_generic(sai_hash_id=hash_oid, sai_qualifiers=sai_attr_updated)

        hashlogger.info("Cleanup %s", field)
        self.dvs_hash.delete_switch_hash_field(field)

    def test_HashPacketTypeDelete(self, testlog):
        """Configure a per-packet-type hash, then remove it and verify the ASIC hash object is removed."""
        field = "lag_hash_ipnip"

        hashlogger.info("Create %s", field)
        self.dvs_hash.update_switch_hash(
            qualifiers={field: ",".join(PKT_TYPE_HASH_FIELD_LIST)}
        )

        new_ids = self.dvs_hash.get_hash_ids(expected=1)
        assert len(new_ids) == 1

        hashlogger.info("Delete %s", field)
        self.dvs_hash.delete_switch_hash_field(field)

        self.dvs_hash.verify_hash_count(0)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
