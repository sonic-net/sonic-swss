import pytest
import time
import logging

from typing import NamedTuple
from swsscommon import swsscommon

import buffer_model


logging.basicConfig(level=logging.INFO)
trimlogger = logging.getLogger(__name__)


SAI_DSCP_MODE_DICT = {
    "dscp-value": "SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_DSCP_VALUE",
    "from-tc": "SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_FROM_TC"
}
SAI_QUEUE_MODE_DICT = {
    "static": "SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC",
    "dynamic": "SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_DYNAMIC"
}
SAI_BUFFER_PROFILE_MODE_DICT = {
    "drop": "SAI_BUFFER_PROFILE_PACKET_ADMISSION_FAIL_ACTION_DROP",
    "trim": "SAI_BUFFER_PROFILE_PACKET_ADMISSION_FAIL_ACTION_DROP_AND_TRIM"
}
SAI_BUFFER_PROFILE_LIST_DICT = {
    "ingress": "SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST",
    "egress": "SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST"
}


class TrimmingTuple(NamedTuple):
    """Config DB trimming attribute container"""
    size: str
    dscp: str
    tc: str
    queue: str


class TrimmingTupleSai(NamedTuple):
    """ASIC DB trimming attribute container"""
    size: str
    dscpMode: str
    dscp: str
    tc: str
    queueMode: str
    queue: str


@pytest.fixture(scope="class")
def dynamicModel(dvs):
    trimlogger.info("Enable dynamic buffer model")
    buffer_model.enable_dynamic_buffer(dvs.get_config_db(), dvs.runcmd)
    yield
    buffer_model.disable_dynamic_buffer(dvs)
    trimlogger.info("Disable dynamic buffer model")


@pytest.fixture(scope="class")
def switchCounters(request, dvs_flex_counter_manager):
    trimlogger.info("Initialize switch counters")

    request.cls.dvs_flex_counter.set_interval("SWITCH", "1000")
    request.cls.dvs_flex_counter.set_status("SWITCH", "enable")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "enable",
        swsscommon.POLL_INTERVAL_FIELD: "1000",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="SWITCH_STAT_COUNTER",
        qualifiers=attr_dict
    )

    yield

    request.cls.dvs_flex_counter.set_status("SWITCH", "disable")
    request.cls.dvs_flex_counter.set_interval("SWITCH", "60000")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "disable",
        swsscommon.POLL_INTERVAL_FIELD: "60000",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="SWITCH_STAT_COUNTER",
        qualifiers=attr_dict
    )

    trimlogger.info("Deinitialize switch counters")


@pytest.fixture(scope="class")
def portCounters(request, dvs_flex_counter_manager):
    trimlogger.info("Initialize port counters")

    request.cls.dvs_flex_counter.set_status("PORT", "enable")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "enable"
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="PORT_STAT_COUNTER",
        qualifiers=attr_dict
    )

    yield

    request.cls.dvs_flex_counter.set_status("PORT", "disable")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "disable",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="PORT_STAT_COUNTER",
        qualifiers=attr_dict
    )

    trimlogger.info("Deinitialize port counters")


@pytest.fixture(scope="class")
def pgCounters(request, dvs_flex_counter_manager):
    trimlogger.info("Initialize priority group counters")

    request.cls.dvs_flex_counter.set_interval("PG_WATERMARK", "1000")
    request.cls.dvs_flex_counter.set_status("PG_WATERMARK", "enable")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "enable",
        swsscommon.POLL_INTERVAL_FIELD: "1000",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="PG_WATERMARK_STAT_COUNTER",
        qualifiers=attr_dict
    )

    yield

    request.cls.dvs_flex_counter.set_status("PG_WATERMARK", "disable")
    request.cls.dvs_flex_counter.set_interval("PG_WATERMARK", "60000")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "disable",
        swsscommon.POLL_INTERVAL_FIELD: "60000",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="PG_WATERMARK_STAT_COUNTER",
        qualifiers=attr_dict
    )

    trimlogger.info("Deinitialize priority group counters")


@pytest.fixture(scope="class")
def queueCounters(request, dvs_flex_counter_manager):
    trimlogger.info("Initialize queue counters")

    request.cls.dvs_flex_counter.set_interval("QUEUE", "1000")
    request.cls.dvs_flex_counter.set_status("QUEUE", "enable")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "enable",
        swsscommon.POLL_INTERVAL_FIELD: "1000",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="QUEUE_STAT_COUNTER",
        qualifiers=attr_dict
    )

    yield

    request.cls.dvs_flex_counter.set_status("QUEUE", "disable")
    request.cls.dvs_flex_counter.set_interval("QUEUE", "10000")

    attr_dict = {
        swsscommon.FLEX_COUNTER_STATUS_FIELD: "disable",
        swsscommon.POLL_INTERVAL_FIELD: "10000",
    }

    request.cls.dvs_flex_counter.verify_flex_counter(
        stat_name="QUEUE_STAT_COUNTER",
        qualifiers=attr_dict
    )

    trimlogger.info("Deinitialize queue counters")


@pytest.mark.usefixtures("dvs_switch_manager")
@pytest.mark.usefixtures("testlog")
class TestTrimmingFlows:
    @pytest.fixture(scope="class")
    def switchData(self):
        trimlogger.info("Initialize switch data")

        trimlogger.info("Verify switch count")
        self.dvs_switch.verify_switch_count(0)

        trimlogger.info("Get switch id")
        switchIdList = self.dvs_switch.get_switch_ids()

        # Assumption: VS has only one switch object
        meta_dict = {
            "id": switchIdList[0]
        }

        yield meta_dict

        trimlogger.info("Deinitialize switch data")


class TestTrimmingBasicFlows(TestTrimmingFlows):
    @pytest.mark.parametrize(
        "attrDict,saiAttrDict", [
            pytest.param(
                TrimmingTuple(size="100", dscp="10", tc=None, queue="1"),
                TrimmingTupleSai(
                    size="100",
                    dscpMode=SAI_DSCP_MODE_DICT["dscp-value"],
                    dscp="10",
                    tc=None,
                    queueMode=SAI_QUEUE_MODE_DICT["static"],
                    queue="1"
                ),
                id="symmetric-dscp-static-queue-index"
            ),
            pytest.param(
                TrimmingTuple(size="200", dscp="20", tc=None, queue="dynamic"),
                TrimmingTupleSai(
                    size="200",
                    dscpMode=SAI_DSCP_MODE_DICT["dscp-value"],
                    dscp="20",
                    tc=None,
                    queueMode=SAI_QUEUE_MODE_DICT["dynamic"],
                    queue="1"
                ),
                id="symmetric-dscp-dynamic-queue-index"
            ),
            pytest.param(
                TrimmingTuple(size="100", dscp="from-tc", tc="1", queue="1"),
                TrimmingTupleSai(
                    size="100",
                    dscpMode=SAI_DSCP_MODE_DICT["from-tc"],
                    dscp="20",
                    tc="1",
                    queueMode=SAI_QUEUE_MODE_DICT["static"],
                    queue="1"
                ),
                id="asymmetric-dscp-static-queue-index"
            ),
            pytest.param(
                TrimmingTuple(size="200", dscp="from-tc", tc="2", queue="dynamic"),
                TrimmingTupleSai(
                    size="200",
                    dscpMode=SAI_DSCP_MODE_DICT["from-tc"],
                    dscp="20",
                    tc="2",
                    queueMode=SAI_QUEUE_MODE_DICT["dynamic"],
                    queue="1"
                ),
                id="asymmetric-dscp-dynamic-queue-index"
            )
        ]
    )
    def test_TrimSwitchGlobalConfiguration(self, switchData, attrDict, saiAttrDict):
        attr_dict = {
            "size": attrDict.size,
            "dscp_value": attrDict.dscp,
            "queue_index": attrDict.queue
        }

        if attrDict.tc is not None:
            attr_dict["tc_value"] = attrDict.tc

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        switchId = switchData["id"]
        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": saiAttrDict.size,
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": saiAttrDict.dscpMode,
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": saiAttrDict.dscp,
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": saiAttrDict.queueMode,
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": saiAttrDict.queue
        }

        if saiAttrDict.tc is not None:
            sai_attr_dict["SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE"] = saiAttrDict.tc

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )


class TestTrimmingAdvancedFlows(TestTrimmingFlows):
    def test_TrimAsymToSymMigration(self, switchData):
        switchId = switchData["id"]

        # Configure Asymmetric DSCP mode
        attr_dict = {
            "size": "200",
            "dscp_value": "from-tc",
            "tc_value": "2",
            "queue_index": "2"
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": "200",
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": SAI_DSCP_MODE_DICT["from-tc"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE": "2",
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": SAI_QUEUE_MODE_DICT["static"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": "2"
        }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

        # Configure Symmetric DSCP mode
        attr_dict = {
            "size": "100",
            "dscp_value": "10",
            "queue_index": "1"
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": "100",
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": SAI_DSCP_MODE_DICT["dscp-value"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": "10",
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": SAI_QUEUE_MODE_DICT["static"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": "1"
        }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

        # Update TC value
        attr_dict = {
            "tc_value": "5"
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE": "2"
        }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

        # Configure Asymmetric DSCP mode
        attr_dict = {
            "size": "200",
            "dscp_value": "from-tc",
            "tc_value": "2",
            "queue_index": "2"
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": "200",
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": SAI_DSCP_MODE_DICT["from-tc"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE": "2",
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": SAI_QUEUE_MODE_DICT["static"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": "2"
        }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )


@pytest.mark.usefixtures("genericConfig")
@pytest.mark.usefixtures("restoreConfig")
class TestTrimmingNegativeFlows(TestTrimmingFlows):
    @pytest.fixture(scope="class")
    def genericConfig(self, switchData):
        trimlogger.info("Add generic configuration")

        switchId = switchData["id"]

        # Asymmetric DSCP mode

        attr_dict = {
            "size": "100",
            "dscp_value": "from-tc",
            "tc_value": "1",
            "queue_index": "1"
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        # Symmetric DSCP mode

        attr_dict = {
            "size": "100",
            "dscp_value": "10",
            "queue_index": "1"
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        # Validation

        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": "100",
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": SAI_DSCP_MODE_DICT["dscp-value"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": "10",
            "SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE": "1",
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": SAI_QUEUE_MODE_DICT["static"],
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": "1"
        }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

        yield

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

        trimlogger.info("Verify generic configuration")

    @pytest.fixture(scope="function")
    def restoreConfig(self, switchData, request):
        switchId = switchData["id"]

        attrDict = request.getfixturevalue("attrDict")
        saiAttrDict = request.getfixturevalue("saiAttrDict")

        yield

        attr_dict = {}

        if attrDict.size is not None:
            attr_dict = {
                "size": "100"
            }

        if attrDict.dscp is not None:
            attr_dict = {
                "dscp_value": "10"
            }

        if attrDict.tc is not None:
            attr_dict = {
                "tc_value": "1"
            }

        if attrDict.queue is not None:
            attr_dict = {
                "queue_index": "1"
            }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        sai_attr_dict = {}

        if saiAttrDict.size is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": "100"
            }

        if saiAttrDict.dscp is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": SAI_DSCP_MODE_DICT["dscp-value"],
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": "10"
            }

        if saiAttrDict.tc is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": SAI_DSCP_MODE_DICT["dscp-value"],
                "SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE": "1"
            }


        if saiAttrDict.queue is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": SAI_QUEUE_MODE_DICT["static"],
                "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": "1"
            }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )

        trimlogger.info("Verify configuration rollback: {}".format(str(attrDict)))

    @pytest.mark.parametrize(
        "attrDict,saiAttrDict", [
            pytest.param(
                TrimmingTuple(size="", dscp=None, tc=None, queue=None),
                TrimmingTupleSai(size="100", dscpMode=None, dscp=None, tc=None, queueMode=None, queue=None),
                id="size-empty"
            ),
            pytest.param(
                TrimmingTuple(size="-1", dscp=None, tc=None, queue=None),
                TrimmingTupleSai(size="100", dscpMode=None, dscp=None, tc=None, queueMode=None, queue=None),
                id="size-min-1"
            ),
            pytest.param(
                TrimmingTuple(size="4294967296", dscp=None, tc=None, queue=None),
                TrimmingTupleSai(size="100", dscpMode=None, dscp=None, tc=None, queueMode=None, queue=None),
                id="size-max+1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp="", tc=None, queue=None),
                TrimmingTupleSai(
                    size=None, tc=None, queueMode=None, queue=None,
                    dscpMode=SAI_DSCP_MODE_DICT["dscp-value"], dscp="10"
                ),
                id="dscp-empty"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp="-1", tc=None, queue=None),
                TrimmingTupleSai(
                    size=None, tc=None, queueMode=None, queue=None,
                    dscpMode=SAI_DSCP_MODE_DICT["dscp-value"], dscp="10"
                ),
                id="dscp-min-1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp="64", tc=None, queue=None),
                TrimmingTupleSai(
                    size=None, tc=None, queueMode=None, queue=None,
                    dscpMode=SAI_DSCP_MODE_DICT["dscp-value"], dscp="10"
                ),
                id="dscp-max+1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, tc="", queue=None),
                TrimmingTupleSai(size=None, dscpMode=None, dscp=None, tc="1", queueMode=None, queue=None),
                id="tc-empty"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, tc="-1", queue=None),
                TrimmingTupleSai(size=None, dscpMode=None, dscp=None, tc="1", queueMode=None, queue=None),
                id="tc-min-1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, tc="256", queue=None),
                TrimmingTupleSai(size=None, dscpMode=None, dscp=None, tc="1", queueMode=None, queue=None),
                id="tc-max+1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, tc=None, queue=""),
                TrimmingTupleSai(
                    size=None, dscpMode=None, dscp=None, tc=None,
                    queueMode=SAI_QUEUE_MODE_DICT["static"], queue="1"
                ),
                id="queue-empty"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, tc=None, queue="-1"),
                TrimmingTupleSai(
                    size=None, dscpMode=None, dscp=None, tc=None,
                    queueMode=SAI_QUEUE_MODE_DICT["static"], queue="1"
                ),
                id="queue-min-1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, tc=None, queue="256"),
                TrimmingTupleSai(
                    size=None, dscpMode=None, dscp=None, tc=None,
                    queueMode=SAI_QUEUE_MODE_DICT["static"], queue="1"
                ),
                id="queue-max+1"
            )
        ]
    )
    def test_TrimNegValueOutOfBound(self, switchData, attrDict, saiAttrDict):
        switchId = switchData["id"]

        attr_dict = {}

        if attrDict.size is not None:
            attr_dict = {
                "size": attrDict.size
            }

        if attrDict.dscp is not None:
            attr_dict = {
                "dscp_value": attrDict.dscp
            }

        if attrDict.tc is not None:
            attr_dict = {
                "tc_value": attrDict.tc
            }

        if attrDict.queue is not None:
            attr_dict = {
                "queue_index": attrDict.queue
            }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        sai_attr_dict = {}

        if saiAttrDict.size is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": saiAttrDict.size
            }

        if saiAttrDict.dscp is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_RESOLUTION_MODE": saiAttrDict.dscpMode,
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": saiAttrDict.dscp
            }

        if saiAttrDict.tc is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_TC_VALUE": saiAttrDict.tc
            }

        if saiAttrDict.queue is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": saiAttrDict.queueMode,
                "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": saiAttrDict.queue
            }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )


@pytest.mark.usefixtures("dvs_buffer_manager")
@pytest.mark.usefixtures("testlog")
class TrimmingBufferModel:
    PORT = "Ethernet0"
    MMU = "12766208"

    @pytest.fixture(scope="class")
    def dynamicBuffer(self, dvs, dynamicModel):
        trimlogger.info("Add dynamic buffer configuration")

        # W/A: Enable dynamic buffer model on VS platform
        trimlogger.info("Configure buffer MMU: size={}".format(self.MMU))
        self.dvs_buffer.update_buffer_mmu(self.MMU)

        trimlogger.info("Set interface admin state to UP: port={}".format(self.PORT))
        dvs.port_admin_set(self.PORT, "up")

        yield

        trimlogger.info("Set interface admin state to DOWN: port={}".format(self.PORT))
        dvs.port_admin_set(self.PORT, "down")

        trimlogger.info("Remove buffer MMU")
        self.dvs_buffer.remove_buffer_mmu()

        trimlogger.info("Remove dynamic buffer configuration")


@pytest.mark.usefixtures("dvs_queue_manager")
class TrimmingRegularBufferModel(TrimmingBufferModel):
    QUEUE = "0"

    @pytest.fixture(scope="class")
    def bufferData(self, queueCounters):
        trimlogger.info("Initialize buffer data")

        trimlogger.info("Verify buffer profiles are loaded")
        self.dvs_buffer.wait_for_buffer_profiles()

        trimlogger.info("Get buffer profile name: port={}, queue={}".format(self.PORT, self.QUEUE))
        bufferProfileName = self.dvs_queue.get_queue_buffer_profile_name(self.PORT, self.QUEUE)

        trimlogger.info("Get buffer profile id: port={}, queue={}".format(self.PORT, self.QUEUE))
        bufferProfileId = self.dvs_queue.get_queue_buffer_profile_id(self.PORT, self.QUEUE)

        meta_dict = {
            "name": bufferProfileName,
            "id": bufferProfileId
        }

        yield meta_dict

        attr_dict = {
            "packet_discard_action": "drop"
        }

        trimlogger.info("Reset buffer profile trimming configuration: {}".format(bufferProfileName))
        self.dvs_buffer.update_buffer_profile(bufferProfileName, attr_dict)

        trimlogger.info("Deinitialize buffer data")

    def verifyBufferProfileConfiguration(self, bufferData, action):
        attr_dict = {
            "packet_discard_action": action
        }

        trimlogger.info("Update buffer profile: {}".format(bufferData["name"]))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=bufferData["name"],
            qualifiers=attr_dict
        )

        bufferProfileId = bufferData["id"]
        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT[action]
        }

        trimlogger.info("Validate buffer profile: {}".format(bufferData["name"]))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=bufferProfileId,
            sai_qualifiers=sai_attr_dict
        )


class TestTrimmingTraditionalBufferModel(TrimmingRegularBufferModel):
    @pytest.mark.parametrize(
        "action", [
            pytest.param("drop", id="drop-packet"),
            pytest.param("trim", id="trim-packet")
        ]
    )
    def test_TrimStaticBufferProfileConfiguration(self, bufferData, action):
        self.verifyBufferProfileConfiguration(bufferData, action)


@pytest.mark.usefixtures("dynamicBuffer")
class TestTrimmingDynamicBufferModel(TrimmingRegularBufferModel):
    @pytest.mark.parametrize(
        "action", [
            pytest.param("drop", id="drop-packet"),
            pytest.param("trim", id="trim-packet")
        ]
    )
    def test_TrimDynamicBufferProfileConfiguration(self, bufferData, action):
        self.verifyBufferProfileConfiguration(bufferData, action)


@pytest.mark.usefixtures("dvs_port_manager")
class TrimmingNegativeBufferModel(TrimmingBufferModel):
    INGRESS_TRIM_PROFILE = "ingress_trim_profile"
    EGRESS_TRIM_PROFILE = "egress_trim_profile"

    INGRESS_DEFAULT_PROFILE = "ingress_default_profile"
    EGRESS_DEFAULT_PROFILE = "egress_default_profile"

    PG = "0"

    def createBufferProfile(self, profileName, attrDict):
        bufferProfileIds = self.dvs_buffer.get_buffer_profile_ids()

        self.dvs_buffer.create_buffer_profile(
            buffer_profile_name=profileName,
            qualifiers=attrDict
        )

        bufferProfileIdsExt = self.dvs_buffer.get_buffer_profile_ids(len(bufferProfileIds)+1)

        return (set(bufferProfileIdsExt) - set(bufferProfileIds)).pop()

    def getPgBufferKeyValuePair(self, portName, pgIdx):
        keyList = self.dvs_buffer.get_buffer_pg_keys(portName, pgIdx)
        assert len(keyList) <= 1, "Invalid BUFFER_PG table"

        if keyList:
            key = keyList[0]
            value = self.dvs_buffer.get_buffer_pg_value(key)

            return key, value

        return None, None

    @pytest.fixture(scope="class")
    def portData(self, portCounters):
        trimlogger.info("Initialize port data")

        trimlogger.info("Get port id: port={}".format(self.PORT))
        portId = self.dvs_port.get_port_id(self.PORT)

        meta_dict = {
            "id": portId
        }

        yield meta_dict

        trimlogger.info("Deinitialize port data")

    @pytest.fixture(scope="class")
    def pgData(self, pgCounters):
        trimlogger.info("Initialize priority group data")

        trimlogger.info("Get priority group id: port={}, pg={}".format(self.PORT, self.PG))
        pgId = self.dvs_buffer.get_priority_group_id(self.PORT, self.PG)

        trimlogger.info("Get priority group buffer profile: port={}, pg={}".format(self.PORT, self.PG))
        pgBufferKey, pgBufferProfile = self.getPgBufferKeyValuePair(self.PORT, self.PG)

        if pgBufferKey is not None:
            trimlogger.info("Remove priority group buffer profile: key={}".format(pgBufferKey))
            self.dvs_buffer.remove_buffer_pg(pgBufferKey)

        meta_dict = {
            "id": pgId
        }

        yield meta_dict

        if pgBufferKey is not None:
            trimlogger.info(
                "Restore priority group buffer profile: key={}, profile={}".format(
                    pgBufferKey, pgBufferProfile
                )
            )
            self.dvs_buffer.update_buffer_pg(
                pg_buffer_key=pgBufferKey,
                pg_buffer_profile=pgBufferProfile
            )
        else:
            if self.dvs_buffer.is_priority_group_exists(self.PORT, self.PG):
                trimlogger.info("Remove priority group buffer profile: port={}, pg={}".format(self.PORT, self.PG))
                self.dvs_buffer.remove_priority_group(self.PORT, self.PG)

        trimlogger.info("Deinitialize priority group data")

    @pytest.fixture(scope="class")
    def bufferData(self):
        trimlogger.info("Initialize buffer data")

        trimlogger.info("Verify buffer profiles are loaded")
        self.dvs_buffer.wait_for_buffer_profiles()

        attr_dict = {
            "dynamic_th": "3",
            "size": "0",
            "pool": "ingress_lossless_pool"
        }

        trimlogger.info("Create buffer profile: {}".format(self.INGRESS_TRIM_PROFILE))
        iBufferProfileTrimId = self.createBufferProfile(self.INGRESS_TRIM_PROFILE, attr_dict)

        trimlogger.info("Create buffer profile: {}".format(self.INGRESS_DEFAULT_PROFILE))
        iBufferProfileDefId = self.createBufferProfile(self.INGRESS_DEFAULT_PROFILE, attr_dict)

        attr_dict = {
            "dynamic_th": "3",
            "size": "1518",
            "pool": "egress_lossy_pool"
        }

        trimlogger.info("Create buffer profile: {}".format(self.EGRESS_TRIM_PROFILE))
        eBufferProfileTrimId = self.createBufferProfile(self.EGRESS_TRIM_PROFILE, attr_dict)

        trimlogger.info("Create buffer profile: {}".format(self.EGRESS_DEFAULT_PROFILE))
        eBufferProfileDefId = self.createBufferProfile(self.EGRESS_DEFAULT_PROFILE, attr_dict)

        iBufferProfileListOld = None
        eBufferProfileListOld = None

        if self.dvs_port.is_buffer_profile_list_exists(self.PORT):
            trimlogger.info("Get ingress buffer profile list: port={}".format(self.PORT))
            iBufferProfileListOld = self.dvs_port.get_buffer_profile_list(self.PORT)

        if self.dvs_port.is_buffer_profile_list_exists(self.PORT, False):
            trimlogger.info("Get egress buffer profile list: port={}".format(self.PORT))
            eBufferProfileListOld = self.dvs_port.get_buffer_profile_list(self.PORT, False)

        meta_dict = {
            "id": {
                "profile": {
                    "trim": {
                        "ingress": iBufferProfileTrimId,
                        "egress": eBufferProfileTrimId
                    },
                    "default": {
                        "ingress": iBufferProfileDefId,
                        "egress": eBufferProfileDefId
                    }
                }
            }
        }

        yield meta_dict

        if iBufferProfileListOld is not None:
            trimlogger.info(
                "Restore ingress buffer profile list: port={}, profile_list={}".format(
                    self.PORT, ",".join(iBufferProfileListOld)
                )
            )
            self.dvs_port.update_buffer_profile_list(
                port_name=self.PORT,
                profile_list=",".join(iBufferProfileListOld)
            )
        else:
            if self.dvs_port.is_buffer_profile_list_exists(self.PORT):
                trimlogger.info("Remove ingress buffer profile list: port={}".format(self.PORT))
                self.dvs_port.remove_buffer_profile_list(self.PORT)

        if eBufferProfileListOld is not None:
            trimlogger.info(
                "Restore egress buffer profile list: port={}, profile_list={}".format(
                    self.PORT, ",".join(eBufferProfileListOld)
                )
            )
            self.dvs_port.update_buffer_profile_list(
                port_name=self.PORT,
                profile_list=",".join(eBufferProfileListOld),
                ingress=False
            )
        else:
            if self.dvs_port.is_buffer_profile_list_exists(self.PORT, False):
                trimlogger.info("Remove egress buffer profile list: port={}".format(self.PORT))
                self.dvs_port.remove_buffer_profile_list(self.PORT, False)

        trimlogger.info("Remove buffer profile: {}".format(self.INGRESS_TRIM_PROFILE))
        self.dvs_buffer.remove_buffer_profile(self.INGRESS_TRIM_PROFILE)

        trimlogger.info("Remove buffer profile: {}".format(self.INGRESS_DEFAULT_PROFILE))
        self.dvs_buffer.remove_buffer_profile(self.INGRESS_DEFAULT_PROFILE)

        trimlogger.info("Remove buffer profile: {}".format(self.EGRESS_TRIM_PROFILE))
        self.dvs_buffer.remove_buffer_profile(self.EGRESS_TRIM_PROFILE)

        trimlogger.info("Remove buffer profile: {}".format(self.EGRESS_DEFAULT_PROFILE))
        self.dvs_buffer.remove_buffer_profile(self.EGRESS_DEFAULT_PROFILE)

        trimlogger.info("Deinitialize buffer data")

    def verifyPriorityGroupBufferAttachConfiguration(self, bufferData, pgData):
        trimProfile = self.INGRESS_TRIM_PROFILE
        defaultProfile = self.INGRESS_DEFAULT_PROFILE

        pgId = pgData["id"]
        trimProfileId = bufferData["id"]["profile"]["trim"]["ingress"]
        defaultProfileId = bufferData["id"]["profile"]["default"]["ingress"]

        # Update priority group with the default buffer profile

        trimlogger.info(
            "Update priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, defaultProfile)
        )
        self.dvs_buffer.update_priority_group(
            port_name=self.PORT,
            pg_index=self.PG,
            buffer_profile_name=defaultProfile
        )

        sai_attr_dict = {
            "SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE": defaultProfileId
        }

        trimlogger.info(
            "Validate priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, defaultProfile)
        )
        self.dvs_buffer.verify_priority_group(
            sai_priority_group_id=pgId,
            sai_qualifiers=sai_attr_dict
        )

        # Set buffer profile trimming eligibility

        attr_dict = {
            "packet_discard_action": "trim"
        }

        trimlogger.info("Update buffer profile: {}".format(trimProfile))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=trimProfile,
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT["trim"]
        }

        trimlogger.info("Validate buffer profile: {}".format(trimProfile))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=trimProfileId,
            sai_qualifiers=sai_attr_dict
        )

        # Update priority group with the trimming buffer profile
        # and verify no update is done to ASIC DB

        trimlogger.info(
            "Update priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, trimProfile)
        )
        self.dvs_buffer.update_priority_group(
            port_name=self.PORT,
            pg_index=self.PG,
            buffer_profile_name=trimProfile
        )
        time.sleep(1)

        sai_attr_dict = {
            "SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE": defaultProfileId
        }

        trimlogger.info(
            "Validate priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, defaultProfile)
        )
        self.dvs_buffer.verify_priority_group(
            sai_priority_group_id=pgId,
            sai_qualifiers=sai_attr_dict
        )

    def verifyPriorityGroupBufferEditConfiguration(self, bufferData, pgData):
        trimProfile = self.INGRESS_TRIM_PROFILE
        defaultProfile = self.INGRESS_DEFAULT_PROFILE

        pgId = pgData["id"]
        trimProfileId = bufferData["id"]["profile"]["trim"]["ingress"]
        defaultProfileId = bufferData["id"]["profile"]["default"]["ingress"]

        # Reset buffer profile trimming eligibility

        attr_dict = {
            "packet_discard_action": "drop"
        }

        trimlogger.info("Update buffer profile: {}".format(trimProfile))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=trimProfile,
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT["drop"]
        }

        trimlogger.info("Validate buffer profile: {}".format(trimProfile))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=trimProfileId,
            sai_qualifiers=sai_attr_dict
        )

        # Update priority group with the trimming buffer profile

        trimlogger.info(
            "Update priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, trimProfile)
        )
        self.dvs_buffer.update_priority_group(
            port_name=self.PORT,
            pg_index=self.PG,
            buffer_profile_name=trimProfile
        )

        sai_attr_dict = {
            "SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE": trimProfileId
        }

        trimlogger.info(
            "Validate priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, trimProfile)
        )
        self.dvs_buffer.verify_priority_group(
            sai_priority_group_id=pgId,
            sai_qualifiers=sai_attr_dict
        )

        # Set buffer profile trimming eligibility
        # and verify no update is done to ASIC DB

        attr_dict = {
            "packet_discard_action": "trim"
        }

        trimlogger.info("Update buffer profile: {}".format(trimProfile))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=trimProfile,
            qualifiers=attr_dict
        )
        time.sleep(1)

        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT["drop"]
        }

        trimlogger.info("Validate buffer profile: {}".format(trimProfile))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=trimProfileId,
            sai_qualifiers=sai_attr_dict
        )

        # Update priority group with the default buffer profile

        trimlogger.info(
            "Update priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, defaultProfile)
        )
        self.dvs_buffer.update_priority_group(
            port_name=self.PORT,
            pg_index=self.PG,
            buffer_profile_name=defaultProfile
        )

        sai_attr_dict = {
            "SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE": defaultProfileId
        }

        trimlogger.info(
            "Validate priority group: port={}, pg={}, profile={}".format(self.PORT, self.PG, defaultProfile)
        )
        self.dvs_buffer.verify_priority_group(
            sai_priority_group_id=pgId,
            sai_qualifiers=sai_attr_dict
        )

    def verifyProfileListBufferAttachConfiguration(self, portData, bufferData, ingress):
        trimProfile = self.INGRESS_TRIM_PROFILE if ingress else self.EGRESS_TRIM_PROFILE
        defaultProfile = self.INGRESS_DEFAULT_PROFILE if ingress else self.EGRESS_DEFAULT_PROFILE

        direction = "ingress" if ingress else "egress"

        portId = portData["id"]
        trimProfileId = bufferData["id"]["profile"]["trim"][direction]
        defaultProfileId = bufferData["id"]["profile"]["default"][direction]

        # Update port ingress/egress buffer profile list with the default buffer profile

        trimlogger.info(
            "Update {} buffer profile list: port={}, profile={}".format(direction, self.PORT, defaultProfile)
        )
        self.dvs_port.update_buffer_profile_list(
            port_name=self.PORT,
            profile_list=defaultProfile,
            ingress=ingress
        )

        sai_attr_dict = {
            SAI_BUFFER_PROFILE_LIST_DICT[direction]: [ defaultProfileId ]
        }

        trimlogger.info(
            "Validate {} buffer profile list: port={}, profile={}".format(direction, self.PORT, defaultProfile)
        )
        self.dvs_port.verify_port(
            sai_port_id=portId,
            sai_qualifiers=sai_attr_dict
        )

        # Set buffer profile trimming eligibility

        attr_dict = {
            "packet_discard_action": "trim"
        }

        trimlogger.info("Update buffer profile: {}".format(trimProfile))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=trimProfile,
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT["trim"]
        }

        trimlogger.info("Validate buffer profile: {}".format(trimProfile))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=trimProfileId,
            sai_qualifiers=sai_attr_dict
        )

        # Update port ingress/egress buffer profile list with the trimming buffer profile
        # and verify no update is done to ASIC DB

        trimlogger.info(
            "Update {} buffer profile list: port={}, profile={}".format(direction, self.PORT, trimProfile)
        )
        self.dvs_port.update_buffer_profile_list(
            port_name=self.PORT,
            profile_list=trimProfile,
            ingress=ingress
        )
        time.sleep(1)

        sai_attr_dict = {
            SAI_BUFFER_PROFILE_LIST_DICT[direction]: [ defaultProfileId ]
        }

        trimlogger.info(
            "Validate {} buffer profile list: port={}, profile={}".format(direction, self.PORT, trimProfile)
        )
        self.dvs_port.verify_port(
            sai_port_id=portId,
            sai_qualifiers=sai_attr_dict
        )

        # Update port ingress/egress buffer profile list with the default buffer profile

        trimlogger.info(
            "Update {} buffer profile list: port={}, profile={}".format(direction, self.PORT, defaultProfile)
        )
        self.dvs_port.update_buffer_profile_list(
            port_name=self.PORT,
            profile_list=defaultProfile,
            ingress=ingress
        )

        sai_attr_dict = {
            SAI_BUFFER_PROFILE_LIST_DICT[direction]: [ defaultProfileId ]
        }

        trimlogger.info(
            "Validate {} buffer profile list: port={}, profile={}".format(direction, self.PORT, defaultProfile)
        )
        self.dvs_port.verify_port(
            sai_port_id=portId,
            sai_qualifiers=sai_attr_dict
        )

    def verifyProfileListBufferEditConfiguration(self, portData, bufferData, ingress):
        trimProfile = self.INGRESS_TRIM_PROFILE if ingress else self.EGRESS_TRIM_PROFILE
        defaultProfile = self.INGRESS_DEFAULT_PROFILE if ingress else self.EGRESS_DEFAULT_PROFILE

        direction = "ingress" if ingress else "egress"

        portId = portData["id"]
        trimProfileId = bufferData["id"]["profile"]["trim"][direction]
        defaultProfileId = bufferData["id"]["profile"]["default"][direction]

        # Reset buffer profile trimming eligibility

        attr_dict = {
            "packet_discard_action": "drop"
        }

        trimlogger.info("Update buffer profile: {}".format(trimProfile))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=trimProfile,
            qualifiers=attr_dict
        )

        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT["drop"]
        }

        trimlogger.info("Validate buffer profile: {}".format(trimProfile))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=trimProfileId,
            sai_qualifiers=sai_attr_dict
        )

        # Update port ingress/egress buffer profile list with the trimming buffer profile

        trimlogger.info(
            "Update {} buffer profile list: port={}, profile={}".format(direction, self.PORT, trimProfile)
        )
        self.dvs_port.update_buffer_profile_list(
            port_name=self.PORT,
            profile_list=trimProfile,
            ingress=ingress
        )

        sai_attr_dict = {
            SAI_BUFFER_PROFILE_LIST_DICT[direction]: [ trimProfileId ]
        }

        trimlogger.info(
            "Validate {} buffer profile list: port={}, profile={}".format(direction, self.PORT, trimProfile)
        )
        self.dvs_port.verify_port(
            sai_port_id=portId,
            sai_qualifiers=sai_attr_dict
        )

        # Set buffer profile trimming eligibility
        # and verify no update is done to ASIC DB

        attr_dict = {
            "packet_discard_action": "trim"
        }

        trimlogger.info("Update buffer profile: {}".format(trimProfile))
        self.dvs_buffer.update_buffer_profile(
            buffer_profile_name=trimProfile,
            qualifiers=attr_dict
        )
        time.sleep(1)

        sai_attr_dict = {
            "SAI_BUFFER_PROFILE_ATTR_PACKET_ADMISSION_FAIL_ACTION": SAI_BUFFER_PROFILE_MODE_DICT["drop"]
        }

        trimlogger.info("Validate buffer profile: {}".format(trimProfile))
        self.dvs_buffer.verify_buffer_profile(
            sai_buffer_profile_id=trimProfileId,
            sai_qualifiers=sai_attr_dict
        )

        # Update port ingress/egress buffer profile list with the default buffer profile

        trimlogger.info(
            "Update {} buffer profile list: port={}, profile={}".format(direction, self.PORT, defaultProfile)
        )
        self.dvs_port.update_buffer_profile_list(
            port_name=self.PORT,
            profile_list=defaultProfile,
            ingress=ingress
        )

        sai_attr_dict = {
            SAI_BUFFER_PROFILE_LIST_DICT[direction]: [ defaultProfileId ]
        }

        trimlogger.info(
            "Validate {} buffer profile list: port={}, profile={}".format(direction, self.PORT, defaultProfile)
        )
        self.dvs_port.verify_port(
            sai_port_id=portId,
            sai_qualifiers=sai_attr_dict
        )


class TestTrimmingNegativeTraditionalBufferModel(TrimmingNegativeBufferModel):
    @pytest.mark.parametrize(
        "target", [
            pytest.param("pg", id="priority-group"),
            pytest.param("ibuf", id="ingress-buffer-profile-list"),
            pytest.param("ebuf", id="egress-buffer-profile-list")
        ]
    )
    def test_TrimNegStaticBufferProfileAttach(self, bufferData, portData, pgData, target):
        if target == "pg":
            self.verifyPriorityGroupBufferAttachConfiguration(bufferData, pgData)
        elif target == "ibuf":
            self.verifyProfileListBufferAttachConfiguration(portData, bufferData, True)
        elif target == "ebuf":
            self.verifyProfileListBufferAttachConfiguration(portData, bufferData, False)

    @pytest.mark.parametrize(
        "target", [
            pytest.param("pg", id="priority-group"),
            pytest.param("ibuf", id="ingress-buffer-profile-list"),
            pytest.param("ebuf", id="egress-buffer-profile-list")
        ]
    )
    def test_TrimNegStaticBufferProfileEdit(self, bufferData, portData, pgData, target):
        if target == "pg":
            self.verifyPriorityGroupBufferEditConfiguration(bufferData, pgData)
        elif target == "ibuf":
            self.verifyProfileListBufferEditConfiguration(portData, bufferData, True)
        elif target == "ebuf":
            self.verifyProfileListBufferEditConfiguration(portData, bufferData, False)


@pytest.mark.usefixtures("dynamicBuffer")
class TestTrimmingNegativeDynamicBufferModel(TrimmingNegativeBufferModel):
    @pytest.mark.parametrize(
        "target", [
            pytest.param("pg", id="priority-group"),
            pytest.param("ibuf", id="ingress-buffer-profile-list"),
            pytest.param("ebuf", id="egress-buffer-profile-list")
        ]
    )
    def test_TrimNegDynamicBufferProfileAttach(self, bufferData, portData, pgData, target):
        if target == "pg":
            self.verifyPriorityGroupBufferAttachConfiguration(bufferData, pgData)
        elif target == "ibuf":
            self.verifyProfileListBufferAttachConfiguration(portData, bufferData, True)
        elif target == "ebuf":
            self.verifyProfileListBufferAttachConfiguration(portData, bufferData, False)

    @pytest.mark.parametrize(
        "target", [
            pytest.param("pg", id="priority-group"),
            pytest.param("ibuf", id="ingress-buffer-profile-list"),
            pytest.param("ebuf", id="egress-buffer-profile-list")
        ]
    )
    def test_TrimNegDynamicBufferProfileEdit(self, bufferData, portData, pgData, target):
        if target == "pg":
            self.verifyPriorityGroupBufferEditConfiguration(bufferData, pgData)
        elif target == "ibuf":
            self.verifyProfileListBufferEditConfiguration(portData, bufferData, True)
        elif target == "ebuf":
            self.verifyProfileListBufferEditConfiguration(portData, bufferData, False)


@pytest.mark.usefixtures("dvs_switch_manager")
@pytest.mark.usefixtures("dvs_port_manager")
@pytest.mark.usefixtures("dvs_queue_manager")
@pytest.mark.usefixtures("testlog")
class TestTrimmingStats:
    PORT = "Ethernet4"
    QUEUE = "1"

    @pytest.fixture(scope="class")
    def switchData(self, switchCounters):
        trimlogger.info("Initialize switch data")

        trimlogger.info("Verify switch count")
        self.dvs_switch.verify_switch_count(0)

        trimlogger.info("Get switch id")
        switchIdList = self.dvs_switch.get_switch_ids()

        # Assumption: VS has only one switch object
        meta_dict = {
            "id": switchIdList[0]
        }

        yield meta_dict

        sai_attr_dict = {
            "SAI_SWITCH_STAT_TX_TRIM_PACKETS": "0",
            "SAI_SWITCH_STAT_DROPPED_TRIM_PACKETS": "0",
        }

        trimlogger.info("Reset switch trimming counters")
        self.dvs_switch.set_switch_counter(switchIdList[0], sai_attr_dict)

        trimlogger.info("Deinitialize switch data")

    @pytest.fixture(scope="class")
    def portData(self, portCounters):
        trimlogger.info("Initialize port data")

        trimlogger.info("Get port id: port={}".format(self.PORT))
        portId = self.dvs_port.get_port_id(self.PORT)

        meta_dict = {
            "id": portId
        }

        yield meta_dict

        sai_attr_dict = {
            "SAI_PORT_STAT_TRIM_PACKETS": "0",
            "SAI_PORT_STAT_TX_TRIM_PACKETS": "0",
            "SAI_PORT_STAT_DROPPED_TRIM_PACKETS": "0"
        }

        trimlogger.info("Reset port trimming counters: port={}".format(self.PORT))
        self.dvs_port.set_port_counter(portId, sai_attr_dict)

        trimlogger.info("Deinitialize port data")

    @pytest.fixture(scope="class")
    def queueData(self, queueCounters):
        trimlogger.info("Initialize queue data")

        trimlogger.info("Get queue id: port={}, queue={}".format(self.PORT, self.QUEUE))
        queueId = self.dvs_queue.get_queue_id(self.PORT, self.QUEUE)

        meta_dict = {
            "id": queueId
        }

        yield meta_dict

        sai_attr_dict = {
            "SAI_QUEUE_STAT_TRIM_PACKETS": "0",
            "SAI_QUEUE_STAT_TX_TRIM_PACKETS": "0",
            "SAI_QUEUE_STAT_DROPPED_TRIM_PACKETS": "0"
        }

        trimlogger.info("Reset queue trimming counters: port={}, queue={}".format(self.PORT, self.QUEUE))
        self.dvs_queue.set_queue_counter(queueId, sai_attr_dict)

        trimlogger.info("Deinitialize queue data")

    @pytest.mark.parametrize(
        "attr, value", [
            pytest.param("SAI_SWITCH_STAT_TX_TRIM_PACKETS", "1000", id="tx-packet"),
            pytest.param("SAI_SWITCH_STAT_DROPPED_TRIM_PACKETS", "2000", id="drop-packet")
        ]
    )
    def test_TrimSwitchStats(self, switchData, attr, value):
        sai_attr_dict = {
            attr: value
        }

        trimlogger.info("Update switch counter")
        self.dvs_switch.set_switch_counter(
            sai_switch_id=switchData["id"],
            sai_qualifiers=sai_attr_dict
        )

        trimlogger.info("Validate switch counter")
        self.dvs_switch.verify_switch_counter(
            sai_switch_id=switchData["id"],
            sai_qualifiers=sai_attr_dict
        )

    @pytest.mark.parametrize(
        "attr, value", [
            pytest.param("SAI_PORT_STAT_TRIM_PACKETS", "1000", id="trim-packet"),
            pytest.param("SAI_PORT_STAT_TX_TRIM_PACKETS", "2000", id="tx-packet"),
            pytest.param("SAI_PORT_STAT_DROPPED_TRIM_PACKETS", "3000", id="drop-packet")
        ]
    )
    def test_TrimPortStats(self, portData, attr, value):
        sai_attr_dict = {
            attr: value
        }

        trimlogger.info("Update port counters: port={}".format(self.PORT))
        self.dvs_port.set_port_counter(
            sai_port_id=portData["id"],
            sai_qualifiers=sai_attr_dict
        )

        trimlogger.info("Validate port counters: port={}".format(self.PORT))
        self.dvs_port.verify_port_counter(
            sai_port_id=portData["id"],
            sai_qualifiers=sai_attr_dict
        )

    @pytest.mark.parametrize(
        "attr, value", [
            pytest.param("SAI_QUEUE_STAT_TRIM_PACKETS", "1000", id="trim-packet"),
            pytest.param("SAI_QUEUE_STAT_TX_TRIM_PACKETS", "2000", id="tx-packet"),
            pytest.param("SAI_QUEUE_STAT_DROPPED_TRIM_PACKETS", "3000", id="drop-packet")
        ]
    )
    def test_TrimQueueStats(self, queueData, attr, value):
        sai_attr_dict = {
            attr: value
        }

        trimlogger.info("Update queue counters: port={}, queue={}".format(self.PORT, self.QUEUE))
        self.dvs_queue.set_queue_counter(
            sai_queue_id=queueData["id"],
            sai_qualifiers=sai_attr_dict
        )

        trimlogger.info("Validate queue counters: port={}, queue={}".format(self.PORT, self.QUEUE))
        self.dvs_queue.verify_queue_counter(
            sai_queue_id=queueData["id"],
            sai_qualifiers=sai_attr_dict
        )


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
