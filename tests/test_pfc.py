import time
from swsscommon import swsscommon


def getBitMaskStr(bits):

    mask = 0

    for b in bits:
        mask = mask | 1 << b

    return str(mask)


def setPortPfc(dvs, port_name, pfc_queues):

    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    port_qos_tbl = swsscommon.Table(cfg_db, 'PORT_QOS_MAP')
    fvs = swsscommon.FieldValuePairs([('pfc_enable', ",".join(str(q) for q in pfc_queues))])
    port_qos_tbl.set(port_name, fvs)

    time.sleep(1)


def setPortPfcAsym(dvs, port_name, pfc_asym):

    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    port_tbl = swsscommon.Table(cfg_db, 'PORT')
    fvs = swsscommon.FieldValuePairs([('pfc_asym', pfc_asym)])
    port_tbl.set(port_name, fvs)

    time.sleep(1)


def getPortOid(dvs, port_name):

    cnt_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    port_map_tbl = swsscommon.Table(cnt_db, 'COUNTERS_PORT_NAME_MAP')

    for k in port_map_tbl.get('')[1]:
        if k[0] == port_name:
            return k[1]

    return ''


def getPortAttr(dvs, port_oid, port_attr):

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    port_tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_PORT:{0}'.format(port_oid))

    for k in port_tbl.get('')[1]:
        if k[0] == port_attr:
            return k[1]

    return ''


def test_PfcAsymmetric(dvs):

    port_name = 'Ethernet0'
    pfc_queues = [ 3, 4 ]

    # Configure default PFC
    setPortPfc(dvs, port_name, pfc_queues)

    # Get SAI object ID for the interface
    port_oid = getPortOid(dvs, port_name)

    # Verify default PFC is set to configured value
    pfc = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL')
    assert pfc == getBitMaskStr(pfc_queues)

    # Enable asymmetric PFC
    setPortPfcAsym(dvs, port_name, 'on')

    # Verify PFC mode is set to 'SEPARATE'
    pfc_mode = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE')
    assert pfc_mode == 'SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE'

    # Verify TX PFC is set to previous PFC value
    pfc_tx = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX')
    assert pfc_tx == pfc

    # Verify RX PFC is set to 0xFF (255)
    pfc_rx = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX')
    assert pfc_rx == '255'

    # Disable asymmetric PFC
    setPortPfcAsym(dvs, port_name, 'off')

    # Verify PFC mode is set to 'COMBINED'
    pfc_mode = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE')
    assert pfc_mode == 'SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED'

    # Verify PFC is set to TX PFC value
    pfc = getPortAttr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL')
    assert pfc == pfc_tx

