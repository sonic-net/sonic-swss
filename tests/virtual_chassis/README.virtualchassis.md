HOWTO Use Multiple Virtual Switches to build a Virtual Chassis Topology(Docker)

1. Create a Virtual Chassis Topology with neighbors using multiple
instances of Virtual Switches(Docker) connected together.
virtual_chassis.py expects a topolgy file, create/delete operation to perform,
an unique namespace string to run the test in that namespace.
There are few sample topology*.json files under the directory
device/virtual/x86_64-kvm_x86_64-r0/virtual_chassis/.
topology.json can be to create/delete a virtual chassis with a chassis_db running
in supervisor and 3 linecards.
topology_nbrs.json has all of topology.json and 1 neighbor per linecard.
topology_ecmp_nbrs.json has one additional neighbor for LC1.
Below example is creating a Virtual Chassis with topology_nbrs.json.
Please use the same topology file for create and delete.
```
./virtual_chassis.py topology_nbrs.json create ss
updateInstance 0 create
 hostName supervisor.ss
docker run --privileged --hostname=supervisor.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x8
6_64-kvm_x86_64-r0/virtual_chassis/0:/usr/share/sonic/virtualchassis:ro  --name supervisor.ss -d docker-sonic-vs
21e37f4dbd2aa76d00268c56b98ef67da35b9ed6afdd4c6ef1ad0323b8bc6e18
docker inspect --format '{{.State.Pid}}' supervisor.ss
1804
2020-04-06 22:40:06.669185                                                                                                                                    adding eth0 to supervisor.ss
updateInstance 1 create
 hostName lc1.ss
docker run --privileged --hostname=lc1.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x86_64-kv
m_x86_64-r0/virtual_chassis/1:/usr/share/sonic/virtualchassis:ro  --name lc1.ss -d docker-sonic-vs
7036d24516df8a36b9ad0d07d0472d2c24c8335bd1ea06101a9a304b61cd915d
docker inspect --format '{{.State.Pid}}' lc1.ss
2387
2020-04-06 22:40:11.931197
adding eth0 to lc1.ss
2020-04-06 22:40:12.320137                                                                                                                                    adding eth1 to lc1.ss
updateInstance 2 create
 hostName lc2.ss
docker run --privileged --hostname=lc2.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x86_64-kv
m_x86_64-r0/virtual_chassis/2:/usr/share/sonic/virtualchassis:ro  --name lc2.ss -d docker-sonic-vs
9f19613a80edf274df34d79394d2cc2f15a94281909549570264f246d6ce2212
docker inspect --format '{{.State.Pid}}' lc2.ss
2819
2020-04-06 22:40:15.186898
adding eth0 to lc2.ss
2020-04-06 22:40:15.595700
adding eth1 to lc2.ss
updateInstance 3 create
 hostName lc3.ss
docker run --privileged --hostname=lc3.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x86_64-kv
m_x86_64-r0/virtual_chassis/3:/usr/share/sonic/virtualchassis:ro  --name lc3.ss -d docker-sonic-vs
5a9aa752baaaa92015b9019b0113af5b3bf1d7ec62733a64d308944486211474
docker inspect --format '{{.State.Pid}}' lc3.ss
3887
2020-04-06 22:40:18.772713
adding eth0 to lc3.ss
2020-04-06 22:40:19.235389
adding eth1 to lc3.ss
updateInstance 4 create
 hostName R1.ss
docker run --privileged --hostname=R1.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x86_64-kvm_x86_64-r0/virtual_chassis/4:/usr/share/sonic/virtualchassis:ro  --name R1.ss -d docker-sonic-vs
aa5e0738d4565054236251ee89eac94430fb6074f8b5c61b034392a38c2b3e2b
docker inspect --format '{{.State.Pid}}' R1.ss
4676
2020-04-06 22:40:22.609463
adding eth0 to R1.ss
2020-04-06 22:40:23.087873
adding eth1 to R1.ss
updateInstance 5 create
 hostName R2.ss
docker run --privileged --hostname=R2.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x86_64-kvm_x86_64-r0/virtual_chassis/5:/usr/share/sonic/virtualchassis:ro  --name R2.ss -d docker-sonic-vs
94dab91267933f3473e862e6b4759020f7458486653f76024d474167a27d3d30
docker inspect --format '{{.State.Pid}}' R2.ss
5017
2020-04-06 22:40:25.174909
adding eth0 to R2.ss
2020-04-06 22:40:25.625724
adding eth1 to R2.ss
updateInstance 6 create
 hostName R3.ss
docker run --privileged --hostname=R3.ss --network none -v /home/workspace/multivs/sonic-buildimage/device/virtual/x86_64-kvm_x86_64-r0/virtual_chassis/6:/usr/share/sonic/virtualchassis:ro  --name R3.ss -d docker-sonic-vs
d3b66ce787ddf65c62902248e5a5ed63fcce49e8450da8751143706bb428c82c
docker inspect --format '{{.State.Pid}}' R3.ss
5629
2020-04-06 22:40:27.627166
adding eth0 to R3.ss
2020-04-06 22:40:28.060716
adding eth1 to R3.ss
updateNeighbor 1-4 create
add lc1.ss.eth1 to bridge br1-4
add R1.ss.eth1 to bridge br1-4
updateNeighbor 3-6 create
add lc3.ss.eth1 to bridge br3-6
add R3.ss.eth1 to bridge br3-6
updateNeighbor 2-5 create
add R2.ss.eth1 to bridge br2-5
add lc2.ss.eth1 to bridge br2-5
```

2. Ping from LC1 to R1(10.8.100.2)., ping from LC1 to Supervisor(10.8.1.200), LC2(10.8.1.2)

```
docker exec -it lc1.ss bash
root@lc1:/# ping 10.8.101.2
PING 10.8.101.2 (10.8.101.2) 56(84) bytes of data.
64 bytes from 10.8.101.2: icmp_seq=1 ttl=64 time=0.611 ms
64 bytes from 10.8.101.2: icmp_seq=2 ttl=64 time=0.313 ms
64 bytes from 10.8.101.2: icmp_seq=3 ttl=64 time=0.318 ms
64 bytes from 10.8.101.2: icmp_seq=4 ttl=64 time=0.217 ms
^C
--- 10.8.101.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss, time 3054ms
rtt min/avg/max/mdev = 0.217/0.364/0.611/0.149 ms

root@lc1:/# ping 10.8.1.200
PING 10.8.1.200 (10.8.1.200) 56(84) bytes of data.
64 bytes from 10.8.1.200: icmp_seq=1 ttl=64 time=0.268 ms
64 bytes from 10.8.1.200: icmp_seq=2 ttl=64 time=0.047 ms
64 bytes from 10.8.1.200: icmp_seq=3 ttl=64 time=0.070 ms
^C
--- 10.8.1.200 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2025ms
rtt min/avg/max/mdev = 0.047/0.128/0.268/0.099 ms
root@lc1:/# ping 10.8.1.2
PING 10.8.1.2 (10.8.1.2) 56(84) bytes of data.
64 bytes from 10.8.1.2: icmp_seq=1 ttl=64 time=0.273 ms
64 bytes from 10.8.1.2: icmp_seq=2 ttl=64 time=0.055 ms
64 bytes from 10.8.1.2: icmp_seq=3 ttl=64 time=0.063 ms
64 bytes from 10.8.1.2: icmp_seq=4 ttl=64 time=0.044 ms
^C
--- 10.8.1.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss, time 3071ms
rtt min/avg/max/mdev = 0.044/0.108/0.273/0.095 ms
root@lc1:/#
```

Perform any other testing/verification of your choice.

3. Delete the Virtual Chassis Topology and the neighbors.
Match the same number of LCs and unique string as the create command in step1.

```
./virtual_chassis.py topology_nbrs.json delete ss
updateInstance 0 delete
 hostName supervisor.ss
 docker container stop supervisor.ss
supervisor.ss
 docker container rm supervisor.ss
supervisor.ss
updateInstance 1 delete
 hostName lc1.ss
 docker container stop lc1.ss
lc1.ss
 docker container rm lc1.ss
lc1.ss
updateInstance 2 delete
 hostName lc2.ss
 docker container stop lc2.ss
lc2.ss
 docker container rm lc2.ss
lc2.ss
updateInstance 3 delete
 hostName lc3.ss
 docker container stop lc3.ss
lc3.ss
 docker container rm lc3.ss
lc3.ss
updateInstance 4 delete
 hostName R1.ss
 docker container stop R1.ss
R1.ss
 docker container rm R1.ss
R1.ss
updateInstance 5 delete
 hostName R2.ss
 docker container stop R2.ss
R2.ss
 docker container rm R2.ss
R2.ss
updateInstance 6 delete
 hostName R3.ss
 docker container stop R3.ss
R3.ss
 docker container rm R3.ss
R3.ss
updateNeighbor 1-4 delete
updateNeighbor 3-6 delete
updateNeighbor 2-5 delete
```