"""
 Copyright (c) 2023. ByteDance Inc. All rights reserved.
A simple topo:

client-----left switch-----rightswitch----... server nodes

This script use sub process to start up nodes. You may also directly run with xterm or screen.
"""
from time import sleep
import subprocess

from mininet.net import Mininet
from mininet.node import Controller
from mininet.cli import CLI
from mininet.link import TCLink
from mininet.log import info, setLogLevel, warning
import shlex
import os

server_num = 6  # data nodes number
method_changebw = 2  # 1: use mininet to change bw, 2: use tc command to change bw
curr_dir = os.getcwd()
"""
we assume  curr_dir/ has following struct
   ./bin/servertest datanode program
   ./MPDtest download program
   ./config/upnode_mn0.json config file for data node 0
   ./config/upnode_mn1.json config file for data node 1
   ./config/...........more data nodes
   ./config/downnode_mn.json download node config file
"""

if __name__ == '__main__':
    setLogLevel('info')
    net = Mininet(controller=Controller)
    info('*** Adding controller\n')
    net.addController('c0')

    server_nodes = []
    info(f'****Add {server_num} server nodes')
    for i in range(server_num):
        s = net.addHost(name=f'server{i}', ip=f'10.100.2.{i}')
        server_nodes.append(s)

    info(f'****Add 1 client node')
    client_node = net.addHost(name='client', ip='10.100.0.1')

    compete_node = net.addHost(name='compete', ip='10.100.10.1')

    info('*** Adding switches\n')
    leftswitch = net.addSwitch('s1')
    rightswitch = net.addSwitch('s2')
    switchid = 3

    """
    It's not a good idea to set both bw and loss rate at same link.
    We have to split the link and set bw and loss rate separately
    """


    def addTClink(l, r, delay='5ms', bw=100, loss=0, queue=1000):
        bridgeswitch = net.addSwitch(name=f's{switchid}')
        net.addLink(l, bridgeswitch, cls=TCLink, delay=delay, loss=loss, max_queue_size=queue)
        link_bw = net.addLink(bridgeswitch, r, cls=TCLink, bw=bw)
        return link_bw


    info('*** Creating links\n')
    """ Note: don't limit the bw for a very small number, like 1Kbps.
        Normally, the value of bw should be around 1Mbps to 1Gbps
    """
    # connect client node with leftside switch
    net.addLink(client_node, leftswitch, cls=TCLink, bw=100)

    serverdelays = ['20ms', '30ms', '40ms', '25ms', '30ms', '35ms', '40ms', '45ms', '50ms', '55ms', \
                    '50ms', '45ms', '40ms', '35ms', '30ms', '40ms', '50ms', '35ms', '25ms', '30ms']
    # serverbws = [10, 5, 100, 10, 10, 10, 10, 10, 10, 10]  # Mbits
    serverbws = [12/server_num if server_num <= 8 else 1.5] * server_num  # Mbits
    # connect datanodes with right side switch, note we use tclink here
    for i in range(server_num):
        addTClink(rightswitch, server_nodes[i], delay=serverdelays[i], bw=serverbws[i])
        switchid += 1

    # connect left and right switches with a bottlenect link, config isdelay 25ms, bw 10Mbps
    bottleneck_link = addTClink(leftswitch, rightswitch, delay='25ms', bw=10, loss=0.1, queue = 100)
    switchid += 1

    # connect compete node with right side switch
    net.addLink(compete_node, rightswitch, cls=TCLink, bw=100)

    info('*** Starting network\n')
    net.start()
    info('*** Testing connectivity\n')
    # net.ping(server_nodes + [tracker_node] + [client_node])

    info('start servers\n')
    # Start servertest program in data node, redirect the stdout to server{i}stdout
    server_proc_ls = []
    server_std_f_ls = []
    for i in range(server_num):
        # use absolute dir to start servertest program, and the parameter is the upnode config json file
        server_start_cmd = f'{curr_dir}/bin/servertest {curr_dir}/config/upnode_mn{i}.json '
        server_std_f = open(f'server{i}stdout', mode='w')
        server_std_f_ls.append(server_std_f)
        server_start_cmd_args = shlex.split(server_start_cmd)
        server_proc = server_nodes[i].popen(server_start_cmd_args, stdout=server_std_f)  # no output
        server_proc_ls.append(server_proc)
        # sleep(1)  # pause for one second
    sleep(3)  # wait for three seconds to finish the connecting process

    info('*** Running CLI\n')
    warning('TYPE exit or CTRL + D to exit!! DO NOT kill the CLI interface.There will be zombie process ')

    # CLI(net)  # start cmd line interface
    client = net.get('client')
    p = client.popen('./MPDtest ./config/downnode_mn.json', shell=True, stdout=None, stderr=None)

    # change the bw of the bottleneck link to 2Mbps for 4 seconds, then back to 10Mbps
    # method 1: use mininet to config the link bw, but can lead to packet loss in switch buffer
    if method_changebw == 1:
        sleep(6)
        intf = bottleneck_link.intf2
        intf.config(bw=2)

        sleep(4)
        intf.config(bw=10)

    #method 2: use tc command to config the link bw, no packet loss in switch buffer
    elif method_changebw == 2:
        sleep(6)
        os.system('tc class change dev s2-eth' + str(server_num+1) + ' parent 5:0 classid 5:1 htb rate 2Mbit burst 15k')

        sleep(4)
        os.system('tc class change dev s2-eth' + str(server_num+1) + ' parent 5:0 classid 5:1 htb rate 10Mbit burst 15k')
    
    #method 3: add competing traffic to the bottleneck link to change the bw
    elif method_changebw == 3:
        sleep(6)
        # use iperf to add competing UDP traffic rate 6Mbps
        compete = net.get('compete')
        client = net.get('client')
        client_p = client.popen('iperf -u -s -i 1', shell=True, stdout=None, stderr=None)
        compete_p = compete.popen('iperf -u -c 10.100.0.1 -b 8M -t 4 -i 5', shell=True, stdout=None, stderr=None)
        compete_p.wait()
        client_p.kill()
        
    
    p.wait()


    """
    To start download test, in CMD line interface,type: 

    client {absolute dir}/MPDtest {absolute_dir}/downnode_mn.json
    
    Once the program is successfully stopped, you may find MPDTrace.txt inside the dir of this python file.
    """

    info('*** Stopping network\n')
    # client_pcap.terminate()
    # clean server first
    for server_proc in server_proc_ls:
        server_proc.kill()
    for sf in server_std_f_ls:
        if not sf.closed:
            sf.close()

    sleep(1)
    net.stop()
