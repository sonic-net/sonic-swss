#!/usr/bin/env python

import argparse
import docker
import json
import os
import re
import sys
import time

# input: topology.json create/delete nsName
# create/delete vs instances as part of chassis or one of its neighbors
# after creation, connect each vs to the chassis bridge
# after neighbor creation, connect the chassis's LC with neighbor
# assumptions:
# 1. connect each vs in the chassis network using inband and vlan4094 connected
#    to the chassis bridge
# 2. vs connects to the outer namespace through veth pair connecting to the bridge
# 3. Each neighbor connection the the LC requires a separate bridge
# 4. Inorder to make the front panel ports (EthernetX) work in the vs
#    all the 32 front panel ports requires veth pairs (eth1 to eth32) in the vs
#    eth0 is for the mgmt port

def runCmd(cmd):
   #print(cmd)
   ret = os.popen(cmd)
   res = ret.read().strip()
   if res != "":
      print(cmd)
      print(res)
   #print(res)
   return res


class VirtualChassisTopology(object):
   def __init__(self, topoFile, oper, ns, src=None, image=None):
      self.instInfo = {}
      self.topoFile = topoFile
      self.oper = oper
      self.ns = ns
      self.src = src
      self.image = image
      self.nss = "sudo ip netns exec " + self.ns
      self.brReq = {}
      self.readyIntfsInInst = {}
      self.chassBr = "br4chs"
      self.ctns = {}

   def handleRequest(self):
      with open(self.topoFile, "r") as f:
         virtualTopology = json.load(f)["VIRTUAL_TOPOLOGY"]
         inst = virtualTopology["chassis_instances"]
         if "neighbor_instances" in virtualTopology:
            inst += virtualTopology["neighbor_instances"]
         if self.oper == "verify":
            self.verifyVirtualChassis(virtualTopology)
            return
         # When virtual chassis is created,
         # 1. new namespace and bridge for the chassis are created first
         # 2. containers for each vs instance need to be created
         # 3. neighbor connections are setup at last.
         # when the virtual chassis is deleted,
         # 1. containers are deleted
         # 2. bridge for neighbor connections are deleted
         # 3. namespace and chassis bridge are deleted
         if self.oper == "create":
            runCmd("sudo ip netns add " + self.ns)
            self.handleBridge(self.chassBr)
            # docker-sonic-vs requires eth0 to be present for orchagent
            for i in inst:
               self.brReq[i] = [("eth0", "")]
               self.readyIntfsInInst[i] = []

            if "neighbor_connections" in virtualTopology:
               for nck, ncv in virtualTopology["neighbor_connections"].items():
                  for i in ncv:
                     self.brReq[i].append((ncv[i], "br" + nck))
         for instDir in inst:
            self.handleSonicInstance(instDir)
         if "neighbor_connections" in virtualTopology:
            for nck, ncv in virtualTopology["neighbor_connections"].items():
               self.handleNeighConn(nck, ncv)
         if self.oper == "delete":
            self.handleBridge("br4chs")
            runCmd("sudo ip netns del " + self.ns)

   def handleBridge(self, brName):
      if self.oper == "create":
         runCmd(self.nss + " brctl addbr " + brName)
         runCmd(self.nss + " ip link set dev " + brName + " up")
      else:
         runCmd(self.nss + " ip link set dev " + brName + " down")
         runCmd(self.nss + " brctl delbr " + brName)
      return

   def setInstInfo(self, inst, name, pid):
      self.instInfo[inst] = [name, pid]

   def getInstInfo(self, inst):
      res = self.instInfo[inst]
      return res[0], res[1]

   def getContainer(self, hostName):
      if hostName in self.ctns:
         return self.ctns[hostName]
      for ctn in docker.from_env().containers.list():
         if ctn.name == hostName:
            self.ctns[ctn.name] = ctn
            return ctn
      return None

   def runCmdOnHost(self, hostName, cmd):
      #print hostName + " " + cmd
      ctn = self.getContainer(hostName)
      res = ctn.exec_run(cmd)
      try:
         out = res.output
      except AttributeError:
         out = res
      return out

   def handleSonicInstance(self, instDir):
      print("updateInstance %s %s" % (instDir, self.oper))
      cwd = os.getcwd()
      cfgFile = cwd + "/" + instDir + "/default_config.json"
      with open(cfgFile, "r") as cfg:
         defCfg = json.load(cfg)["DEVICE_METADATA"]["localhost"]
         hostName = defCfg["hostname"] + "." + self.ns
         print(" hostName %s " % hostName)
         if self.oper == "create":
            vol = " -v " + cwd + "/" + instDir + ":/usr/share/sonic/virtualchassis "
            vol += " -v /var/run/redis-vs/" + hostName + ":/var/run/redis "
            if self.src:
               vol += " -v " + self.src + ":/usr/src:ro "
            imageName = "docker-sonic-vs"
            if self.image:
               imageName = self.image
            cmd = "docker run --privileged --hostname=" + hostName + \
                  " --network none" + vol + " --name " + hostName +\
                  " -d " + imageName
            runCmd(cmd)
            pid = runCmd("docker inspect --format '{{.State.Pid}}' " + hostName)
            self.setInstInfo(instDir, hostName, pid)
            # pass self.ns into the vs to be use for vs restarts by swss conftest.
            # connection to chassBr is setup by chassis_connect.py within the vs
            data = {}
            data["chassis_namespace"] = self.ns
            data["chassis_bridge"] = self.chassBr
            data["neighbor_connections"] = {}
            # keep orchagent happy with all eth ports by
            # creating 33 veth pairs to enable the front panel ports
            for i in range(0, 33):
               intf = "eth" + str(i)
               self.addVethPairNs(intf, instDir)
            for intf, br in self.brReq[instDir]:
               hostIntf = hostName + "." + intf
               data["neighbor_connections"][hostIntf] = br
            if "inband_address" in defCfg.keys():
               iaddr = defCfg["inband_address"]
               ifname = self.ns + "veth" + instDir
               ifpair = "inband"
               data["inband_intf"] = ifname
               data["inband_intf_pair"] = ifpair
               data["inband_address"] = iaddr
               runCmd(self.nss + " ip link add " + ifname + " type veth peer name "
                       + ifpair)
               runCmd(self.nss + " ip link set " + ifpair + " netns " + pid)
               runCmd(self.nss + " ip link set dev " + ifname + " up")
               runCmd(self.nss + " brctl addif br4chs " + ifname)
               res = self.runCmdOnHost(hostName, " cat /sys/class/net/" +
                                        ifpair + "/operstate")
               while res.strip() != "up":
                  self.runCmdOnHost(hostName, " ip link set dev " + ifpair + " up")
                  time.sleep(1)
                  res = self.runCmdOnHost(hostName, " cat /sys/class/net/" +
                                           ifpair + "/operstate")

            vct_data = cwd + "/" + instDir + "/vct_connections.json"
            with open (vct_data, "w") as outfile:
               json.dump(data, outfile)

         else:
            runCmd(" docker container stop " + hostName)
            runCmd(" docker container rm " + hostName)
      return

   def addVethPairNs(self, intf, inst):
      nss = self.nss
      instName, pid = self.getInstInfo(inst)
      ifn = instName[:9] + "." + intf
      ifpair = intf

      runCmd(nss + " ip link add " + ifn + " type veth peer name " + ifpair)
      runCmd(nss + " ip link set " + ifn + " netns " + self.ns)
      runCmd(nss + " ip link set " + ifpair + " netns " + pid)
      runCmd(nss + " ip link set dev " + ifn + " up ")
      self.runCmdOnHost(instName, " ip link set dev " + ifpair + " up")
      return

   def addToBr(self, intf, inst, br):
      instName, _ = self.getInstInfo(inst)
      ifn = instName + "." + intf
      print("add %s to bridge %s " % (ifn, br))
      runCmd(self.nss + " brctl addif " + br + " " + ifn)

   def handleNeighConn(self, nck, ncv):
      print("updateNeighbor %s %s" % (nck, self.oper))
      br = "br" + nck
      if self.oper == "create":
         self.handleBridge(br)
         for inst in ncv:
            for srv in range(1, 33):
               ifn = "eth" + str(srv)
               if (ifn, br) not in self.readyIntfsInInst[inst]:
                  if (ifn, br) in self.brReq[inst]:
                     self.addToBr(ifn, inst, br)
                  self.readyIntfsInInst[inst].append((ifn, br))
      else:
         self.handleBridge(br)
      return

   def verifyChassisDb(self, virtualTopology ):
      passed = True
      hostNames = {}
      cwd = os.getcwd()
      inst = virtualTopology["chassis_instances"]
      for instDir in inst:
         cfgFile = cwd + "/" + instDir + "/default_config.json"
         with open(cfgFile, "r") as cfg:
            defCfg = json.load(cfg)["DEVICE_METADATA"]["localhost"]
            hostName = defCfg["hostname"] + "." + self.ns
            hostNames[ instDir ] = hostName
            # verify connectivity to chassis-db from inst
            if "chassis_db_address" in defCfg:
               print("verify chassisdb connectivity to %s " % hostName)
               res = self.runCmdOnHost(hostName,
                               "sonic-db-cli CHASSIS_DB keys SYSTEM_PORT\"*\"")
               if len(res.split("\n")) != 97:
                  print("FAILED:chassisdb in vs must have %s sysports" % res)
                  print(len(res.split("\n")))
                  passed = False
      return hostNames, passed

   def verifyConnectivity(self, virtualTopology):
      passed = True
      if "neighbor_connections" not in virtualTopology:
         return passed
      cwd = os.getcwd()
      for nck, ncv in virtualTopology["neighbor_connections"].items():
         # connectivity check between the neighbors
         inst = nck.split("-")[0]
         nbrInst = nck.split("-")[1]
         nbrVethIntfId = int(ncv[nbrInst].split("eth")[1])
         nbrIntf = "ethernet%d|" % ((nbrVethIntfId - 1) * 4)
         cfgFile = cwd + "/" + inst + "/default_config.json"
         hostName = ""
         with open(cfgFile, "r") as cfg:
            defCfg = json.load(cfg)["DEVICE_METADATA"]["localhost"]
            hostName = defCfg["hostname"] + "." + self.ns
         cfgFile = cwd + "/" + nbrInst + "/default_config.json"
         with open(cfgFile, "r") as cfg:
            intfCfg = json.load(cfg)["INTERFACE"]
            for key in intfCfg:
               nbrAddr = ""
               if key.lower().startswith(nbrIntf):
                  intfAddr = re.split("/|\\|", key)
                  if len(intfAddr) > 1:
                     nbrAddr = intfAddr[1]
                  if nbrAddr == "":
                     continue
                  print("verify neighbor connectivity from %s to %s nbrAddr " % (
                     hostName, nbrAddr))
                  res = self.runCmdOnHost(hostName, " ping -c 5 " + nbrAddr)
                  if "5 received" not in res.split("\n")[-3]:
                     print("FAILED:%s: ping %s \n res: %s " % (hostName, nbrAddr,
                                                               res))
                     passed = False
      return passed

   def verifyCrashes( self, virtualTopology, hostNames ):
      inst = virtualTopology[ 'chassis_instances' ]
      passed = True
      # verify no crashes
      for instDir in inst:
         hostName = hostNames[instDir]
         res = self.runCmdOnHost(hostName,
                                 " grep 'terminated by SIGABRT' /var/log/syslog ")
         if res != "":
            print("FAILED: container %s has agent termination(s)" % hostName)
            print(res)
            passed = False
      return passed

   def verifyVirtualChassis(self, virtualTopology):
      hostNames, ret1 = self.verifyChassisDb( virtualTopology )
      ret2 = self.verifyConnectivity( virtualTopology )
      ret3 = self.verifyCrashes( virtualTopology, hostNames )
      if ret1 and ret2 and ret3:
         print("All verifications PASSED")
      print("Verifications completed")

if __name__ == "__main__":
   parser = argparse.ArgumentParser()
   parser.add_argument("topology", help="Topology file ")
   parser.add_argument("oper", help="create/delete/verify")
   parser.add_argument("name", help="Unique Name")
   parser.add_argument("--source", "-s", required=False, nargs="?",
         help="Source directory to mount")
   parser.add_argument("--image", "-i", required=False, nargs="?",
         help="Special image [registry:]docker-sonic-vs[-dbg][:tag]")
   args = parser.parse_args()
   if len(sys.argv) < 4:
      print("usage: %s topo.json create/delete uniqueString" % (sys.argv[0]))
   else:
      vct = VirtualChassisTopology(args.topology, args.oper, args.name,
                                    args.source, args.image)
      vct.handleRequest()

