#!/usr/bin/python

from __future__ import division
import sys
import os
import math
import numpy as np
from collections import defaultdict
from tabulate import tabulate
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from scipy.stats import norm
matplotlib.rcParams['font.size'] = 16
matplotlib.rcParams['xtick.labelsize'] = 14
matplotlib.rcParams['ytick.labelsize'] = 14
try:
    from xml.etree import cElementTree as ElementTree
except ImportError:
    from xml.etree import ElementTree

def parse_time_ns(tm):
    if tm.endswith('ns'):
        return long(tm[:-4])
    raise ValueError(tm)

BOTTLENECK_RATE = 10e9 # 10Gbps
PERCENTILE_CUTOFF = 99
N_BINS = 8 

## FiveTuple
class FiveTuple(object):
    ## class variables
    ## @var sourceAddress 
    #  source address
    ## @var destinationAddress 
    #  destination address
    ## @var protocol 
    #  network protocol
    ## @var sourcePort 
    #  source port
    ## @var destinationPort 
    #  destination port
    ## @var __slots__ 
    #  class variable list
    __slots__ = ['sourceAddress', 'destinationAddress', 'protocol', 'sourcePort', 'destinationPort']
    def __init__(self, el):
        '''The initializer.
        @param self The object pointer.
        @param el The element.
        '''
        self.sourceAddress = el.get('sourceAddress')
        self.destinationAddress = el.get('destinationAddress')
        self.sourcePort = int(el.get('sourcePort'))
        self.destinationPort = int(el.get('destinationPort'))
        self.protocol = int(el.get('protocol'))
        
## Histogram
class Histogram(object):
    ## class variables
    ## @var bins
    #  histogram bins
    ## @var nbins
    #  number of bins
    ## @var number_of_flows
    #  number of flows
    ## @var __slots__
    #  class variable list
    __slots__ = 'bins', 'nbins', 'number_of_flows'
    def __init__(self, el=None):
        ''' The initializer.
        @param self The object pointer.
        @param el The element.
        '''
        self.bins = []
        if el is not None:
            #self.nbins = int(el.get('nBins'))
            for bin in el.findall('bin'):
                self.bins.append( (float(bin.get("start")), float(bin.get("width")), int(bin.get("count"))) )

## Flow
class Flow(object):
    ## class variables
    ## @var flowId
    #  delay ID
    ## @var delayMean
    #  mean delay
    ## @var packetLossRatio
    #  packet loss ratio
    ## @var rxBitrate
    #  receive bit rate
    ## @var txBitrate
    #  transmit bit rate
    ## @var fiveTuple
    #  five tuple
    ## @var packetSizeMean
    #  packet size mean
    ## @var probe_stats_unsorted
    #  unsirted probe stats
    ## @var size
    #  size of flow
    ## @var hopCount
    #  hop count
    ## @var flowInterruptionsHistogram
    #  flow histogram
    ## @var rx_duration
    #  receive duration
    ## @var slowdown
    #  ratio of actual to optimal flow duration
    ## @var rawDuration
    #  actual duration of flow
    ## @var start
    #  starting time of flow in seconds
    ## @var finish
    #  finishing time of flow in seconds
    ## @var __slots__
    #  class variable list
    __slots__ = ['flowId', 'delayMean', 'packetLossRatio', 'rxBitrate', 'txBitrate',
                 'fiveTuple', 'packetSizeMean', 'probe_stats_unsorted', 'size',
                 'hopCount', 'flowInterruptionsHistogram', 'rx_duration', 'slowdown', 'rawDuration',
                 'start', 'finish']
    def __init__(self, flow_el):
        ''' The initializer.
        @param self The object pointer.
        @param flow_el The element.
        '''
        self.flowId = int(flow_el.get('flowId'))
        rxPackets = long(flow_el.get('rxPackets'))
        txPackets = long(flow_el.get('txPackets'))
        start = long(flow_el.get('timeFirstTxPacket')[:-4]) * 1.0e-9
        finish = long(flow_el.get('timeLastRxPacket')[:-4]) * 1.0e-9
	tx_duration = float(long(flow_el.get('timeLastTxPacket')[:-4]) - long(flow_el.get('timeFirstTxPacket')[:-4]))*1e-9
        rx_duration = float(long(flow_el.get('timeLastRxPacket')[:-4]) - long(flow_el.get('timeFirstRxPacket')[:-4]))*1e-9
	actual_duration = float(long(flow_el.get('timeLastRxPacket')[:-4]) - long(flow_el.get('timeFirstTxPacket')[:-4]))*1e-9
	flowsize_bits = float(long(flow_el.get('txBytes')))*8.0
	agg_capacity = float(40e9)
	edge_capacity = float(10e9)
	host_proc_delay = 1.5e-6
        prop_delay = 2e-6  # delay to travel the length of cable
        processing_delay = 250e-9   # delay for a packet to be processed by the switch software
	optimal_flow_duration = float(2.0*flowsize_bits/edge_capacity) + float(2.0*flowsize_bits/agg_capacity)
        log_base = 10
        slowdown_raw = actual_duration / optimal_flow_duration

        self.slowdown = slowdown_raw
        self.rawDuration = actual_duration

        self.start = start
        self.finish = finish

        self.size = long(flow_el.get('txBytes'))
        self.rx_duration = rx_duration
        self.probe_stats_unsorted = []
        if rxPackets:
            self.hopCount = float(flow_el.get('timesForwarded')) / rxPackets + 1
        else:
            self.hopCount = -1000
        if rxPackets:
            self.delayMean = float(flow_el.get('delaySum')[:-4]) / rxPackets * 1e-9
            self.packetSizeMean = float(flow_el.get('rxBytes')) / rxPackets
        else:
            self.delayMean = None
            self.packetSizeMean = None
        if rx_duration > 0:
            self.rxBitrate = long(flow_el.get('rxBytes'))*8 / rx_duration
        else:
            self.rxBitrate = None
        if tx_duration > 0:
            self.txBitrate = long(flow_el.get('txBytes'))*8 / tx_duration
        else:
            self.txBitrate = None
        lost = float(flow_el.get('lostPackets'))
        if rxPackets == 0:
            self.packetLossRatio = None
        else:
            self.packetLossRatio = (lost / (rxPackets + lost))

        interrupt_hist_elem = flow_el.find("flowInterruptionsHistogram")
        if interrupt_hist_elem is None:
            self.flowInterruptionsHistogram = None
        else:
            self.flowInterruptionsHistogram = Histogram(interrupt_hist_elem)

## ProbeFlowStats
class ProbeFlowStats(object):
    ## class variables
    ## @var probeId
    #  probe ID
    ## @var packets
    #  network packets
    ## @var bytes
    #  bytes
    ## @var delayFromFirstProbe
    #  delay from first probe
    ## @var __slots__
    #  class variable list
    __slots__ = ['probeId', 'packets', 'bytes', 'delayFromFirstProbe']

## Simulation
class Simulation(object):
    ## class variables
    ## @var flows
    #  list of flows
    ## @var flow_map
    #  metadata associated with each flow
    def __init__(self, simulation_el):
        ''' The initializer.
        @param self The object pointer.
        @param simulation_el The element.
        '''
        self.flows = []
        flow_map = {}
        FlowClassifier_el, = simulation_el.findall("Ipv4FlowClassifier")
        for flow_el in simulation_el.findall("FlowStats/Flow"):
            flow = Flow(flow_el)
            flow_map[flow.flowId] = flow
            self.flows.append(flow)
        for flow_cls in FlowClassifier_el.findall("Flow"):
            flowId = int(flow_cls.get('flowId'))
            flow_map[flowId].fiveTuple = FiveTuple(flow_cls)

        for probe_elem in simulation_el.findall("FlowProbes/FlowProbe"):
            probeId = int(probe_elem.get('index'))
            for stats in probe_elem.findall("FlowStats"):
                flowId = int(stats.get('flowId'))
                s = ProbeFlowStats()
                s.packets = int(stats.get('packets'))
                s.bytes = long(stats.get('bytes'))
                s.probeId = probeId
                if s.packets > 0:
                    s.delayFromFirstProbe =  parse_time_ns(stats.get('delayFromFirstProbeSum')) / float(s.packets)
                else:
                    s.delayFromFirstProbe = 0
                flow_map[flowId].probe_stats_unsorted.append(s)


# Function to Fix Step and the end of CDFs
# StackOverflow Credit: https://stackoverflow.com/a/52921726/3341596
def fix_hist_step_vertical_line_at_end(ax):
    axpolygons = [poly for poly in ax.get_children() if isinstance(poly, patches.Polygon)]
    for poly in axpolygons:
                poly.set_xy(poly.get_xy()[:-1])

def main(argv):

    if len(argv) < 2:
        print "usage: ./incast_whiskers.py <path to tmp incast folder>"
        exit(1)

    sim_names = []
    dctcp_sim_dict = {}
    pbs_sim_dict = {}
    path = argv[1]
    for i, xmlfile in enumerate(os.listdir(path)): # send in entire incast directory
        if xmlfile.split('.')[1] != 'xml':
            continue
        file_obj = open(os.path.join(path,xmlfile))
        # names look like "tmp/incast/incast_a10_s128.xml"
        raw_simname = xmlfile.split(".")[0].split("_")
        sim_profile = raw_simname[0]
        is_dctcp = raw_simname[1] == 'dctcp'
        if is_dctcp == True:
            sim_alpha = 'dctcp' 
        else:
            sim_alpha = r'$\alpha$' + ' = ' + raw_simname[1][1:]
        sim_incasters = int(raw_simname[2][1:])
        sim_names.append(sim_alpha + ", degree = " + str(sim_incasters))
        print "_".join(raw_simname)
        print "Reading XML file \n",
 
        sys.stdout.flush()        
        level = 0
        for event, elem in ElementTree.iterparse(file_obj, events=("start", "end")):
            if event == "start":
                level += 1
            if event == "end":
                level -= 1
                if level == 0 and elem.tag == 'FlowMonitor':
                    sim = Simulation(elem)
                    if is_dctcp == True:
                        dctcp_sim_dict[sim_incasters] = sim
                    else:
                        pbs_sim_dict[sim_incasters] = sim
                    elem.clear() # won't need this any more
                    sys.stdout.write(".")
                    sys.stdout.flush()
        print " done."

    # FCT BoxPlot
    fig, ax = plt.subplots()

    dctcp_data = []
    for i, sim in sorted(dctcp_sim_dict.items()):
        flows_of_interest = []
        flowsizes = []

	# DO NOT INCLUDE ACKs
        for flow in sim.flows:
            if flow.fiveTuple.sourcePort != 9:
                flows_of_interest.append( flow )

        # Get FlowSizes for this incast scenario
        fsizes = []
        for flow in flows_of_interest:
            if flow.rawDuration > 0:
                fsizes.append(float(flow.size))
        dctcp_data.append( np.array(fsizes).reshape((-1,1)) )

    pbs_data = []
    for i, sim in sorted(pbs_sim_dict.items()):
        #print "processing pbs data for {} servers".format(i)
        flows_of_interest = []
        flowsizes = []

	# DO NOT INCLUDE ACKs
        for flow in sim.flows:
            if flow.fiveTuple.sourcePort != 9:
                flows_of_interest.append( flow )

        # Get FlowSizes for this incast scenario
        fsizes = []
        for flow in flows_of_interest:
            if flow.rawDuration > 0:
                fsizes.append(float(flow.size))
        pbs_data.append( np.array(fsizes).reshape((-1,1)) )

    print "pbs_data size: ", len(pbs_data)
    plt.boxplot( pbs_data, showfliers=False )
    #plt.title("PBS - FCT Fairness")
    plt.ylabel("Flow Size (bytes)")
    #plt.ylim([0,0.025])
    plt.xlabel("Number of Incast Senders")
    plt.xticks([k for k in range(1,129)], (str(k) if k%16 == 0 else "" for k in range(1,129)), rotation=20)
    outfilename = "pbs_incast_sizes.png"
    plt.tight_layout()
    plt.savefig(outfilename)
    plt.clf()

    print "dctcp_data size: ", len(dctcp_data)
    plt.boxplot( dctcp_data, showfliers=False )
    #plt.title("DCTCP - FCT Fairness")
    plt.ylabel("Flow Size (bytes)", fontsize=16)
    #plt.ylim([0,0.025])
    plt.xlabel("Number of Incast Senders", fontsize=16)
    plt.xticks([k for k in range(1,129)], (str(k) if k%16 == 0 else "" for k in range(1,129)), rotation=20)
    outfilename = "dctcp_incast_sizes.png"
    plt.tight_layout()
    plt.savefig(outfilename, dpi=600)
    plt.clf()
if __name__ == '__main__':
    main(sys.argv)
