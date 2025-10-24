from dvslib.dvs_common import wait_for_result, PollingConfig
import os

# State DB POST state
STATE_DB_MACSEC_POST_TABLE = "FIPS_MACSEC_POST_TABLE"
STATE_DB_MACSEC_POST_STATE_DISABLED = "disabled"
STATE_DB_MACSEC_POST_STATE_SWITCH_LEVEL_POST_IN_PROGRESS = "switch-level-post-in-progress"
STATE_DB_MACSEC_POST_STATE_MACSEC_LEVEL_POST_IN_PROGRESS = "macsec-level-post-in-progress"
STATE_DB_MACSEC_POST_STATE_PASS = "pass"
STATE_DB_MACSEC_POST_STATE_FAIL = "fail"

# SAI POST capability
SAI_MACSEC_POST_CAPABILITY = "macsec-post-capability"
SAI_MACSEC_POST_CAPABILITY_NOT_SUPPORTED = "not-supported"
SAI_MACSEC_POST_CAPABILITY_SWITCH = "switch"
SAI_MACSEC_POST_CAPABILITY_MACSEC = "macsec"

# VS SAI POST config
VS_SAI_POST_CONFIG_FILE = "/tmp/vs_fips_post_config"
VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_NOTIFY = "switch-macsec-post-status-notify"
VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_QUERY = "switch-macsec-post-status-query"
VS_SAI_POST_CONFIG_INGRESS_MACSEC_POST_STATUS_NOTIFY = "ingress-macsec-post-status-notify"
VS_SAI_POST_CONFIG_EGRESS_MACSEC_POST_STATUS_NOTIFY = "egress-macsec-post-status-notify"
SAI_SWITCH_MACSEC_POST_STATUS_PASS = "SAI_SWITCH_MACSEC_POST_STATUS_PASS"
SAI_SWITCH_MACSEC_POST_STATUS_FAIL = "SAI_SWITCH_MACSEC_POST_STATUS_FAIL"
SAI_SWITCH_MACSEC_POST_STATUS_IN_PROGRESS = "SAI_SWITCH_MACSEC_POST_STATUS_IN_PROGRESS"
SAI_MACSEC_POST_STATUS_PASS = "SAI_MACSEC_POST_STATUS_PASS"
SAI_MACSEC_POST_STATUS_FAIL = "SAI_MACSEC_POST_STATUS_FAIL"
SAI_MACSEC_POST_STATUS_IN_PROGRESS = "SAI_MACSEC_POST_STATUS_IN_PROGRESS"

# POST syslogs
SWITCH_MACSEC_POST_PASS_SYSYLOG = "Switch MACSec POST passed"
SWITCH_MACSEC_POST_FAIL_SYSYLOG = "Switch MACSec POST failed"
SWITCH_MACSEC_POST_FAIL_SYSYLOG_SAI_NOT_SUPPORTED = "MACSec POST is not supported by SAI"
MACSEC_POST_ENABLED_SYSLOG = "Init MACSec objects and enable POST"
INGRESS_MACSEC_POST_PASS_SYSLOG = "Ingress MACSec POST passed"
INGRESS_MACSEC_POST_FAIL_SYSLOG = "Ingress MACSec POST failed"
EGRESS_MACSEC_POST_PASS_SYSLOG = "Egress MACSec POST passed"
EGRESS_MACSEC_POST_FAIL_SYSLOG = "Egress MACSec POST failed"
MACSEC_POST_PASS_SYSLOG = "Ingress and egress MACSec POST passed"
MACSEC_POST_FAIL_SYSLOG = "MACSec POST failed"

FIPS_ENABLE_FILE = "/etc/fips/fips_enable"

class TestMacsecPost(object):
    saved_fips_config = None

    def check_state_db_post_state(self, dvs, expected_state):
        dvs.get_state_db().wait_for_field_match(STATE_DB_MACSEC_POST_TABLE, "sai",
                                                {'post_state': expected_state})
 
    def disable_sonic_fips(self, dvs):
        dvs.runcmd(["sh", "-c", f"echo 0 > {FIPS_ENABLE_FILE}"])

    def enable_sonic_fips(self, dvs):
        dvs.runcmd(["sh", "-c", f"echo 1 > {FIPS_ENABLE_FILE}"])
 
    def restart_dvs_with_post_config(self, dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_SWITCH,
                                     sai_post_notification_status_config=None, sonic_fips_enabled=True):
        if sonic_fips_enabled:
            self.enable_sonic_fips(dvs)
        else:
            self.disable_sonic_fips(dvs)

        sai_post_config = {}    
        if sai_post_capability != SAI_MACSEC_POST_CAPABILITY_NOT_SUPPORTED:
            sai_post_config[SAI_MACSEC_POST_CAPABILITY] = sai_post_capability
        if sai_post_notification_status_config:
            sai_post_config.update(sai_post_notification_status_config)
        dvs.runcmd(["sh", "-c", f"rm -f {VS_SAI_POST_CONFIG_FILE}"])
        dvs.runcmd(["sh", "-c", f"touch {VS_SAI_POST_CONFIG_FILE}"])
        for k, v in sai_post_config.items():
            dvs.runcmd(["sh", "-c", f"echo '{k} {v}' >> {VS_SAI_POST_CONFIG_FILE}"])
 
        marker = dvs.add_log_marker()

        dvs.runcmd('killall5 -15')
        dvs.net_cleanup()
        dvs.destroy_servers()
        dvs.create_servers()
        dvs.restart()

        return marker
 
    def check_syslog(self, dvs, marker, log):
        def do_check_syslog():
            (ec, out) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \'%s\' | wc -l" %(marker, log)])
            return (int(out.strip()) >= 1, None)
        max_poll = PollingConfig(polling_interval=5, timeout=600, strict=True)
        wait_for_result(do_check_syslog, polling_config=max_poll)
 
    def check_asic_db_post_state(self, dvs, sonic_fips_enabled=True, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_SWITCH):
        switch_oids = dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        assert len(switch_oids) == 1
        entry = dvs.get_asic_db().get_entry("ASIC_STATE", f"SAI_OBJECT_TYPE_SWITCH:{switch_oids[0]}")
        if sonic_fips_enabled:
            assert entry["SAI_SWITCH_ATTR_MACSEC_ENABLE_POST"] and entry["SAI_SWITCH_ATTR_SWITCH_MACSEC_POST_STATUS_NOTIFY"]
        else:
            assert "SAI_SWITCH_ATTR_MACSEC_ENABLE_POST" not in entry and "SAI_SWITCH_ATTR_SWITCH_MACSEC_POST_STATUS_NOTIFY" not in entry

        macsec_oids = dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC")            
        if sai_post_capability == SAI_MACSEC_POST_CAPABILITY_SWITCH:
            # No MACSec object should be created since POST is supported in switch init.
            assert not macsec_oids 
        elif sai_post_capability == SAI_MACSEC_POST_CAPABILITY_MACSEC:
            # POST is only supported in MACSec init. Two MACSec objects - ingress and egress - must be created to enable POST.
            assert len(macsec_oids) == 2
            for oid in macsec_oids:
                entry = dvs.get_asic_db().get_entry("ASIC_STATE", f"SAI_OBJECT_TYPE_MACSEC:{oid}")
                assert entry["SAI_MACSEC_ATTR_ENABLE_POST"]

    def test_Setup(self, dvs):
        exitcode, output = dvs.runcmd(f"cat {FIPS_ENABLE_FILE}")
        if exitcode:
            assert "No such file or directory" in output
            dvs.runcmd(["sh", "-c", f"mkdir -p {os.path.dirname(FIPS_ENABLE_FILE)} && touch {FIPS_ENABLE_FILE}"])
        else:
            TestMacsecPost.saved_fips_config = output.strip()

    def test_PostDisabled(self, dvs):
        self.restart_dvs_with_post_config(dvs, sonic_fips_enabled=False)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_DISABLED)
        self.check_asic_db_post_state(dvs, sonic_fips_enabled=False)
 
    def test_PostEnabled_InitialState(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_QUERY : SAI_SWITCH_MACSEC_POST_STATUS_IN_PROGRESS}
        self.restart_dvs_with_post_config(dvs, sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_SWITCH_LEVEL_POST_IN_PROGRESS)
        self.check_asic_db_post_state(dvs)

    def test_PostEnabled_SaiPostNotSupported(self, dvs):
        marker = self.restart_dvs_with_post_config(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_NOT_SUPPORTED)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_DISABLED)
        self.check_syslog(dvs, marker, SWITCH_MACSEC_POST_FAIL_SYSYLOG_SAI_NOT_SUPPORTED)
        self.check_asic_db_post_state(dvs)
        
    def test_PostEnabled_SwitchLevelPost_NotificationPass(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_NOTIFY : SAI_SWITCH_MACSEC_POST_STATUS_PASS,
                                               VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_QUERY : SAI_SWITCH_MACSEC_POST_STATUS_IN_PROGRESS}
        marker = self.restart_dvs_with_post_config(dvs, sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_PASS)
        self.check_syslog(dvs, marker, SWITCH_MACSEC_POST_PASS_SYSYLOG)
        self.check_asic_db_post_state(dvs)
 
    def test_PostEnabled_SwitchLevelPost_NotificationFail(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_NOTIFY : SAI_SWITCH_MACSEC_POST_STATUS_FAIL,
                                               VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_QUERY : SAI_SWITCH_MACSEC_POST_STATUS_IN_PROGRESS}
        marker = self.restart_dvs_with_post_config(dvs, sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_FAIL)
        self.check_syslog(dvs, marker, SWITCH_MACSEC_POST_FAIL_SYSYLOG)
        self.check_asic_db_post_state(dvs)
 
    def test_PostEnabled_SwitchLevelPost_QueryPass(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_QUERY : SAI_SWITCH_MACSEC_POST_STATUS_PASS}
        marker =self.restart_dvs_with_post_config(dvs, sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_PASS)
        self.check_syslog(dvs, marker, SWITCH_MACSEC_POST_PASS_SYSYLOG)
        self.check_asic_db_post_state(dvs)
 
    def test_PostEnabled_SwitchLevelPost_QueryFail(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_SWITCH_POST_STATUS_QUERY : SAI_SWITCH_MACSEC_POST_STATUS_FAIL}
        marker =self.restart_dvs_with_post_config(dvs, sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_FAIL)
        self.check_syslog(dvs, marker, SWITCH_MACSEC_POST_FAIL_SYSYLOG)
        self.check_asic_db_post_state(dvs)

    def test_PostEnabled_MacsecLevelPost_StateBeforeNotification(self, dvs):
        marker = self.restart_dvs_with_post_config(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_MACSEC_LEVEL_POST_IN_PROGRESS)
        self.check_syslog(dvs, marker, MACSEC_POST_ENABLED_SYSLOG)
        self.check_asic_db_post_state(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC)
        
    def test_PostEnabled_MacsecLevelPost_NotificationPass(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_INGRESS_MACSEC_POST_STATUS_NOTIFY : SAI_MACSEC_POST_STATUS_PASS,
                                               VS_SAI_POST_CONFIG_EGRESS_MACSEC_POST_STATUS_NOTIFY : SAI_MACSEC_POST_STATUS_PASS}
        marker = self.restart_dvs_with_post_config(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC,
                                                   sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_PASS)
        for syslog in [INGRESS_MACSEC_POST_PASS_SYSLOG, EGRESS_MACSEC_POST_PASS_SYSLOG, MACSEC_POST_PASS_SYSLOG]:
            self.check_syslog(dvs, marker, syslog)
        self.check_asic_db_post_state(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC)
 
    def test_PostEnabled_MacsecLevelPost_NotificationIngressPostFail(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_INGRESS_MACSEC_POST_STATUS_NOTIFY : SAI_MACSEC_POST_STATUS_FAIL,
                                               VS_SAI_POST_CONFIG_EGRESS_MACSEC_POST_STATUS_NOTIFY : SAI_MACSEC_POST_STATUS_PASS}
        marker = self.restart_dvs_with_post_config(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC,
                                                   sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_FAIL)
        for syslog in [INGRESS_MACSEC_POST_FAIL_SYSLOG, EGRESS_MACSEC_POST_PASS_SYSLOG, MACSEC_POST_FAIL_SYSLOG]:
            self.check_syslog(dvs, marker, syslog)
        self.check_asic_db_post_state(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC)

    def test_PostEnabled_MacsecLevelPost_NotificationEgressPostFail(self, dvs):
        sai_post_notification_status_config = {VS_SAI_POST_CONFIG_INGRESS_MACSEC_POST_STATUS_NOTIFY : SAI_MACSEC_POST_STATUS_PASS,
                                               VS_SAI_POST_CONFIG_EGRESS_MACSEC_POST_STATUS_NOTIFY : SAI_MACSEC_POST_STATUS_FAIL}
        marker = self.restart_dvs_with_post_config(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC,
                                                   sai_post_notification_status_config=sai_post_notification_status_config)
        self.check_state_db_post_state(dvs, STATE_DB_MACSEC_POST_STATE_FAIL)
        for syslog in [INGRESS_MACSEC_POST_PASS_SYSLOG, EGRESS_MACSEC_POST_FAIL_SYSLOG, MACSEC_POST_FAIL_SYSLOG]:
            self.check_syslog(dvs, marker, syslog)
        self.check_asic_db_post_state(dvs, sai_post_capability=SAI_MACSEC_POST_CAPABILITY_MACSEC)

    def test_CleanUp(self,dvs):
        if TestMacsecPost.saved_fips_config is not None:
            dvs.runcmd(["sh", "-c", f"echo {TestMacsecPost.saved_fips_config} > {FIPS_ENABLE_FILE}"])
        else:
            dvs.runcmd(["sh", "-c", f"rm -f {FIPS_ENABLE_FILE}"])
        dvs.runcmd(["sh", "-c", f"rm -f {VS_SAI_POST_CONFIG_FILE}"])
        dvs.restart()


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
