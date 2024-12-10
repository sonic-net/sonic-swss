# This test suite covers the functionality of twamp light feature in SwSS
import pytest
import time

@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_twamp_manager')
class TestTwampLight(object):

    def check_syslog(self, dvs, marker, log, expected_cnt):
        (ec, out) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \'%s\' | wc -l" % (marker, log)])
        assert out.strip() == str(expected_cnt)

    def test_SenderPacketCountSingle(self, dvs, testlog):
        """
        This test covers the TWAMP Light session creation and removal operations
        Operation flow:
        1. Create twamp-light session-sender using once packet-count
           The session remains inactive
        2. Start twamp-light session
           The session becomes active
        3. Remove twamp-light session
        """

        session = "TEST_SENDER1"
        src_ip = "1.1.1.1"
        src_udp_port = "862"
        dst_ip = "2.2.2.2"
        dst_udp_port = "863"
        packet_count = "1000"
        tx_interval = "10"
        timeout = "10"
        stats_interval = "20000"

        marker = dvs.add_log_marker()

        # create twamp-light session
        self.dvs_twamp.create_twamp_light_session_sender_packet_count(session, src_ip, src_udp_port, dst_ip, dst_udp_port, packet_count, tx_interval, timeout)

        # start twamp-light session
        self.dvs_twamp.start_twamp_light_sender(session)

        # wait for sending TWAMP-test done
        time.sleep(12)

        # remove twamp-light session
        self.dvs_twamp.remove_twamp_light_session(session)
        self.dvs_twamp.verify_no_session()

    def test_SenderPacketCountMulti(self, dvs, testlog):
        """
        This test covers the TWAMP Light Sender session creation and removal operations
        Operation flow:
        1. Create twamp-light session-sender using multi packet-count
           The session remains inactive
        2. Start twamp-light session
           The session becomes active
        3. Remove twamp-light session
        """

        session = "TEST_SENDER1"
        src_ip = "1.2.3.4"
        src_udp_port = "862"
        dst_ip = "5.6.7.8"
        dst_udp_port = "863"
        packet_count = "1000"
        tx_interval = "10"
        timeout = "10"
        stats_interval = "11000"

        marker = dvs.add_log_marker()

        # create twamp-light session
        self.dvs_twamp.create_twamp_light_session_sender_packet_count(session, src_ip, src_udp_port, dst_ip, dst_udp_port, packet_count, tx_interval, timeout, stats_interval)

        # start twamp-light session
        self.dvs_twamp.start_twamp_light_sender(session)

        # wait for sending TWAMP-test done
        time.sleep(120)

        # remove twamp-light session
        self.dvs_twamp.remove_twamp_light_session(session)
        self.dvs_twamp.verify_no_session()

    def test_SenderContinuousSingle(self, dvs, testlog):
        """
        This test covers the TWAMP Light Sender session creation and removal operations
        Operation flow:
        1. Create twamp-light session-sender using once continuous
           The session remains inactive
        2. Start twamp-light session
           The session becomes active
        3. Remove twamp-light session
        """

        session = "TEST_SENDER2"
        src_ip = "11.11.11.11"
        src_udp_port = "862"
        dst_ip = "12.12.12.12"
        dst_udp_port = "863"
        monitor_time = "60"
        tx_interval = "100"
        timeout = "10"
        stats_interval = "60000"

        marker = dvs.add_log_marker()

        # create twamp-light session
        self.dvs_twamp.create_twamp_light_session_sender_continuous(session, src_ip, src_udp_port, dst_ip, dst_udp_port, monitor_time, tx_interval, timeout)

        # start twamp-light session
        self.dvs_twamp.start_twamp_light_sender(session)
        # wait for sending TWAMP-test done
        time.sleep(60)

        # remove twamp-light session
        self.dvs_twamp.remove_twamp_light_session(session)
        self.dvs_twamp.verify_no_session()

    def test_SenderContinuousMulti(self, dvs, testlog):
        """
        This test covers the continuous TWAMP Light Sender session creation and removal operations
        Operation flow:
        1. Create twamp-light session-sender using multi continuous
           The session remains inactive
        2. Start twamp-light session
           The session becomes active
        3. Remove twamp-light session
        """

        session = "TEST_SENDER2"
        src_ip = "11.12.13.14"
        src_udp_port = "862"
        dst_ip = "15.16.17.18"
        dst_udp_port = "863"
        monitor_time = "60"
        tx_interval = "100"
        timeout = "10"
        stats_interval = "20000"

        marker = dvs.add_log_marker()

        # create twamp-light session
        self.dvs_twamp.create_twamp_light_session_sender_continuous(session, src_ip, src_udp_port, dst_ip, dst_udp_port, monitor_time, tx_interval, timeout, stats_interval)

        # start twamp-light session
        self.dvs_twamp.start_twamp_light_sender(session)

        # wait for sending TWAMP-test done
        time.sleep(60)

        # remove twamp-light session
        self.dvs_twamp.remove_twamp_light_session(session)
        self.dvs_twamp.verify_no_session()

    def test_Reflector(self, dvs, testlog):
        """
        This test covers the TWAMP Light Reflector session creation and removal operations
        Operation flow:
        1. Create twamp-light session-reflector
        2. Remove twamp-light session
        """

        session = "TEST_REFLECTOR1"
        src_ip = "22.1.1.1"
        src_udp_port = "862"
        dst_ip = "22.1.1.2"
        dst_udp_port = "863"

        marker = dvs.add_log_marker()

        # create twamp-light session
        self.dvs_twamp.create_twamp_light_session_reflector(session, src_ip, src_udp_port, dst_ip, dst_udp_port)

        # remove twamp-light session
        self.dvs_twamp.remove_twamp_light_session(session)
        self.dvs_twamp.verify_no_session()

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
