import pytest
import logging

from typing import NamedTuple

import buffer_model


logging.basicConfig(level=logging.INFO)
trimlogger = logging.getLogger(__name__)


SAI_QUEUE_MODE_DICT = {
    "static": "SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC",
    "dynamic": "SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_DYNAMIC"
}
SAI_BUFFER_PROFILE_MODE_DICT = {
    "drop": "SAI_BUFFER_PROFILE_PACKET_ADMISSION_FAIL_ACTION_DROP",
    "trim": "SAI_BUFFER_PROFILE_PACKET_ADMISSION_FAIL_ACTION_DROP_AND_TRIM"
}


class TrimmingTuple(NamedTuple):
    """Config DB trimming attribute container"""
    size: str
    dscp: str
    queue: str


class TrimmingTupleSai(NamedTuple):
    """ASIC DB trimming attribute container"""
    size: str
    dscp: str
    mode: str
    queue: str


@pytest.fixture(scope="class")
def dynamicModel(dvs):
    trimlogger.info("Enable dynamic buffer model")
    buffer_model.enable_dynamic_buffer(dvs.get_config_db(), dvs.runcmd)
    yield
    buffer_model.disable_dynamic_buffer(dvs.get_config_db(), dvs.runcmd)
    trimlogger.info("Disable dynamic buffer model")


@pytest.fixture(scope="class")
def portCounters(dvs):
    trimlogger.info("Initialize port counters")
    dvs.runcmd("counterpoll port enable")
    yield
    dvs.runcmd("counterpoll port disable")
    trimlogger.info("Deinitialize port counters")


@pytest.fixture(scope="class")
def queueCounters(dvs):
    trimlogger.info("Initialize queue counters")
    dvs.runcmd("counterpoll queue enable")
    yield
    dvs.runcmd("counterpoll queue disable")
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
                TrimmingTuple(size="100", dscp="10", queue="1"),
                TrimmingTupleSai(size="100", dscp="10", mode=SAI_QUEUE_MODE_DICT["static"], queue="1"),
                id="static-queue-index"
            ),
            pytest.param(
                TrimmingTuple(size="200", dscp="20", queue="dynamic"),
                TrimmingTupleSai(size="200", dscp="20", mode=SAI_QUEUE_MODE_DICT["dynamic"], queue="1"),
                id="dynamic-queue-index"
            )
        ]
    )
    def test_TrimSwitchGlobalConfiguration(self, switchData, attrDict, saiAttrDict):
        attr_dict = {
            "size": attrDict.size,
            "dscp_value": attrDict.dscp,
            "queue_index": attrDict.queue
        }

        trimlogger.info("Update trimming global")
        self.dvs_switch.update_switch_trimming(
            qualifiers=attr_dict
        )

        switchId = switchData["id"]
        sai_attr_dict = {
            "SAI_SWITCH_ATTR_PACKET_TRIM_SIZE": saiAttrDict.size,
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": saiAttrDict.dscp,
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": saiAttrDict.mode,
            "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": saiAttrDict.queue
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
            "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": "10",
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
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": "10"
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
                TrimmingTuple(size="", dscp=None, queue=None),
                TrimmingTupleSai(size="100", dscp=None, mode=None, queue=None),
                id="size-empty"
            ),
            pytest.param(
                TrimmingTuple(size="-1", dscp=None, queue=None),
                TrimmingTupleSai(size="100", dscp=None, mode=None, queue=None),
                id="size-min-1"
            ),
            pytest.param(
                TrimmingTuple(size="4294967296", dscp=None, queue=None),
                TrimmingTupleSai(size="100", dscp=None, mode=None, queue=None),
                id="size-max+1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp="", queue=None),
                TrimmingTupleSai(size=None, dscp="10", mode=None, queue=None),
                id="dscp-empty"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp="-1", queue=None),
                TrimmingTupleSai(size=None, dscp="10", mode=None, queue=None),
                id="dscp-min-1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp="64", queue=None),
                TrimmingTupleSai(size=None, dscp="10", mode=None, queue=None),
                id="dscp-max+1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, queue=""),
                TrimmingTupleSai(size=None, dscp=None, mode=SAI_QUEUE_MODE_DICT["static"], queue="1"),
                id="queue-empty"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, queue="-1"),
                TrimmingTupleSai(size=None, dscp=None, mode=SAI_QUEUE_MODE_DICT["static"], queue="1"),
                id="queue-min-1"
            ),
            pytest.param(
                TrimmingTuple(size=None, dscp=None, queue="256"),
                TrimmingTupleSai(size=None, dscp=None, mode=SAI_QUEUE_MODE_DICT["static"], queue="1"),
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
                "SAI_SWITCH_ATTR_PACKET_TRIM_DSCP_VALUE": saiAttrDict.dscp
            }

        if saiAttrDict.queue is not None:
            sai_attr_dict = {
                "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_RESOLUTION_MODE": saiAttrDict.mode,
                "SAI_SWITCH_ATTR_PACKET_TRIM_QUEUE_INDEX": saiAttrDict.queue
            }

        trimlogger.info("Validate trimming global")
        self.dvs_switch.verify_switch(
            sai_switch_id=switchId,
            sai_qualifiers=sai_attr_dict
        )


@pytest.mark.usefixtures("dvs_buffer_manager")
@pytest.mark.usefixtures("dvs_queue_manager")
@pytest.mark.usefixtures("testlog")
class TrimmingBufferModel:
    PORT = "Ethernet0"
    QUEUE = "0"
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

    @pytest.fixture(scope="class")
    def bufferData(self, queueCounters):
        trimlogger.info("Initialize buffer data")

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

class TestTrimmingTraditionalBufferModel(TrimmingBufferModel):
    @pytest.mark.parametrize(
        "action", [
            pytest.param("drop", id="drop-packet"),
            pytest.param("trim", id="trim-packet")
        ]
    )
    def test_TrimStaticBufferProfileConfiguration(self, bufferData, action):
        self.verifyBufferProfileConfiguration(bufferData, action)

class TestTrimmingDynamicBufferModel(TrimmingBufferModel):
    @pytest.mark.parametrize(
        "action", [
            pytest.param("drop", id="drop-packet"),
            pytest.param("trim", id="trim-packet")
        ]
    )
    def test_TrimDynamicBufferProfileConfiguration(self, dynamicBuffer, bufferData, action):
        self.verifyBufferProfileConfiguration(bufferData, action)


@pytest.mark.usefixtures("dvs_port_manager")
@pytest.mark.usefixtures("dvs_queue_manager")
@pytest.mark.usefixtures("testlog")
class TestTrimmingStats:
    PORT = "Ethernet4"
    QUEUE = "1"

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
            "SAI_PORT_STAT_TRIM_PACKETS": "0"
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
            "SAI_QUEUE_STAT_TRIM_PACKETS": "0"
        }

        trimlogger.info("Reset queue trimming counters: port={}, queue={}".format(self.PORT, self.QUEUE))
        self.dvs_queue.set_queue_counter(queueId, sai_attr_dict)

        trimlogger.info("Deinitialize queue data")

    def test_TrimPortStats(self, portData):
        sai_attr_dict = {
            "SAI_PORT_STAT_TRIM_PACKETS": "1000"
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

    def test_TrimQueueStats(self, queueData):
        sai_attr_dict = {
            "SAI_QUEUE_STAT_TRIM_PACKETS": "1000"
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
