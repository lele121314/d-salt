/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include "ns3/fatal-error.h"
#include "ns3/string.h"
#include "ns3/simulator.h"
#include "ns3/queue-disc.h"
#include "ns3/uinteger.h"
#include "ns3/pointer.h"
#include "ns3/application-container.h"
#include "ns3/socket.h"
#include "ns3/address.h"
#include "ns3/inet-socket-address.h"
#include "ns3/bulk-send-application.h"
#include "prioTag.h"
#include "pbs.h"

namespace ns3 {

	NS_OBJECT_ENSURE_REGISTERED (PbsPacketFilter);
	
	TypeId
	PbsPacketFilter::GetTypeId (void)
	{
		static TypeId tid = TypeId ("ns3::PbsPacketFilter")
		  .SetParent<PacketFilter> ()
		  .SetGroupName ("TrafficControl")
		  .AddConstructor<PbsPacketFilter> ()
		  .AddAttribute ("Alpha", 
				 "The parameter for tuning scheduling policy.",
		  	       	 DoubleValue (0.001),
		               	 MakeDoubleAccessor (&PbsPacketFilter::m_alpha),
		  	       	 MakeDoubleChecker <double> ())	       
		  .AddAttribute ("Profile",
				 "The workload distribution type helps to tune priority boundaries.",
				 UintegerValue (0),
				 MakeUintegerAccessor (&PbsPacketFilter::m_profile),
				 MakeUintegerChecker <uint32_t> ())
		  .AddAttribute ("UsePbs", 
				 "The flag indicating whether or not to use PBS for scheduling priorities.",
		  	       	 BooleanValue (true),
		               	 MakeBooleanAccessor (&PbsPacketFilter::m_usePbs),
		  	       	 MakeBooleanChecker ())	       
		  .AddAttribute ("NonBlind", 
				 "The flag indicating whether or not PBS is operating in blind context.",
		  	       	 BooleanValue (false),
		               	 MakeBooleanAccessor (&PbsPacketFilter::m_nonBlind),
		  	       	 MakeBooleanChecker ())	       
		  ;
		return tid;
	}
	
	PbsPacketFilter::PbsPacketFilter ()
	{
		m_totalBytes = 0;
	}
	
	PbsPacketFilter::~PbsPacketFilter ()
	{
	}

	void
	PbsPacketFilter::SetNodePointer (Ptr<Node> nodeptr)
	{
		m_nodeptr = nodeptr;
	}

	void
	PbsPacketFilter::PrintStats (std::ofstream& stream)
	{
		stream << "================================================================================\n";
		stream << "Alpha: " << m_alpha << "\n";
		stream << "Total Bytes: " << m_totalBytes << "\n\n";
		for (auto stats_it = m_flowStats.begin(); stats_it != m_flowStats.end(); stats_it++)
		{
			FlowId flowId = stats_it->first;
			FlowStats &ref = stats_it->second;

			stream << "FlowID: " << flowId << ",\tPackets Sent: " << ref.txPackets
			       << ",\tBytes Sent: " << ref.txBytes
			       << ",\tFlow Age: " << ref.flowAge.GetNanoSeconds() << " ns\n"
			       ;

			stream << "Priority Limits: ";
			for (int i = 0; i < 8; i++)
			{
				stream << m_prioLimits[i] << ", ";
			}
			stream << "\n";

			stream << "Priority History: " << "\n";
			for (auto prio_it = ref.prioHistory.begin(); prio_it != ref.prioHistory.end(); prio_it++)
			{
				stream << "Priority: " << prio_it->first << ", \%-txBytes: " 
				       << prio_it->second * 1.0 / ref.txBytes << "\n";
			}
			stream << "\n";	

			stream << "Raw Priority History: " << "\n";
			for (auto raw_it = ref.rawPrioHistory.begin(); raw_it != ref.rawPrioHistory.end(); raw_it++)
			{
				stream << std::get<0>(*raw_it) << "," << std::get<1>(*raw_it) << ","
				       << std::get<2>(*raw_it) << "\n";
			}
			stream << "\n\n";	
		}

		stream.flush();
	}

	void
	PbsPacketFilter::StreamRawPrioToCsv (std::ofstream& csv)
	{
		for (auto stats_it = m_flowStats.begin(); stats_it != m_flowStats.end(); stats_it++)
		{
			FlowId flowId = stats_it->first;
			FlowStats &ref = stats_it->second;

			csv << flowId << ",";
			for (auto raw_it = ref.rawPrioHistory.begin(); raw_it != ref.rawPrioHistory.end(); raw_it++)
			{
				csv << std::get<0>(*raw_it) << "," << std::get<1>(*raw_it) << ","
				    << std::get<2>(*raw_it) << ",";
			}
			csv << "\n";	
		}
	}
	void
	PbsPacketFilter::StreamPacketsToCsv (std::ofstream& csv)
	{
		for (auto stats_it = m_flowStats.begin(); stats_it != m_flowStats.end(); stats_it++)
		{
			FlowId flowId = stats_it->first;
			FlowStats &ref = stats_it->second;

			csv << flowId << ",";
			for (auto prio_it = ref.prioPacketHistory.begin(); prio_it != ref.prioPacketHistory.end(); prio_it++)
			{
				// number of packets from flow going into this priority bin
				csv << prio_it->second << ",";
			}
			csv << "\n";	
		}
	}
	
	void
	PbsPacketFilter::StreamToCsv (std::ofstream& csv)
	{
		for (auto stats_it = m_flowStats.begin(); stats_it != m_flowStats.end(); stats_it++)
		{
			FlowId flowId = stats_it->first;
			FlowStats &ref = stats_it->second;

			csv << flowId << ",";
			for (auto prio_it = ref.prioHistory.begin(); prio_it != ref.prioHistory.end(); prio_it++)
			{
				// raw bytes of flow going into this priority bin
				csv << prio_it->second << ",";
			}
			csv << "\n";	
		}
	}

	uint64_t
	PbsPacketFilter::GetTotalBytes (void)
	{
		return m_totalBytes;
	}

	inline PbsPacketFilter::FlowStats&
	PbsPacketFilter::GetStatsForFlow (FlowId flowId)
	{
		FlowStatsContainerI iter;
		iter = m_flowStats.find(flowId);
		if (iter == m_flowStats.end() )
		{
			PbsPacketFilter::FlowStats &ref = m_flowStats[flowId];
			ref.timeFirstTxPacket = Seconds (0);
			ref.timeLastTxPacket = Seconds (0);
			ref.flowAge = Seconds (0);
			ref.txBytes = 0;
			ref.flowSize = 0;
			ref.txPackets = 0;
			ref.firstTx = true;
			for (uint16_t prio = 0; prio < 8; prio++)
			{
				ref.prioHistory[prio] = 0;
				ref.prioPacketHistory[prio] = 0;
			}
			return ref;
		}
		else
		{
			return iter->second;
		}
	}

	void
	PbsPacketFilter::MakePrioLimits (void)
	{
		std::vector<double> limits;

		switch (m_profile) {
			case 1: // W1 (alpha = 10)
				limits = {2.5e-19, 1e-21, 4.4e-24, 1.9e-26, 7.8e-29, 3.3e-31, 1.4e-33, 5.8e-36};
				break;
			case 2: // W2 (alpha = 10)
				limits = {6.1e-20, 1.5e-23, 3.6e-27, 8.7e-31, 2.1e-34, 5.1e-38, 1.2e-41, 2.9e-45};
				break;
			case 3: // W3 (alpha = 10)
				limits = {5.8e-21, 7.3e-26, 9e-31, 1.1e-35, 1.3e-40, 1.6e-45, 2e-50, 2.5e-55};
				break;
			case 4: // W4 (alpha = 10)
				limits = {2.6e-21, 1.2e-26, 5.3e-32, 2.4e-37, 1.1e-42, 4.9e-48, 2.2e-53, 1e-58};
				break;
			case 5: // W5 (alpha = 10)
				limits = {1.2e-21, 2.7e-27, 6e-33, 1.3e-38, 3e-44, 6.5e-50, 1.4e-55, 3.2e-61};
				break;
			case 6: // Incast 
				limits = {2.6e-21, 1.2e-26, 5.3e-32, 2.4e-37, 1.1e-42, 4.9e-48, 2.2e-53, 1e-58};
				break;
			case 7: // background flows
				limits = {1.2e-21, 2.7e-27, 6e-33, 1.3e-38, 3e-44, 6.5e-50, 1.4e-55, 3.2e-61};
				break;
			default:
				std::cout << m_profile << std::endl;
				NS_FATAL_ERROR("invalid profile specified.");
		}
		for (int i=0; i<8; i++) {
			if (m_nonBlind) {
				m_prioLimits[i] = limits[8 - i - 1]; // bytes remaining inversely related to txBytes 
			} else {
				m_prioLimits[i] = limits[i];
			}
		}
	}

	std::map<uint64_t, uint64_t>
	PbsPacketFilter::PeekLoadAtTime (void)
	{
		return (std::map<uint64_t, uint64_t>)m_loadAtTime;
	}

	inline uint64_t&
	PbsPacketFilter::GetLoadStats (void)
	{
		uint64_t currTime;
		SwitchLoadContainerI iter;
		currTime = Simulator::Now().GetNanoSeconds ();
		iter = m_loadAtTime.find(currTime);
		if ( iter == m_loadAtTime.end() )
		{
			uint64_t &ref = m_loadAtTime[currTime];
			ref = 0;
			return ref;
		}
		return iter->second;
	}
	
	int32_t
	PbsPacketFilter::DoClassify (Ptr<QueueDiscItem> item) const
	{
		FlowId flowId = item->Hash();
		double raw_prio;
		uint8_t bin_prio;
		uint32_t packetsize = item->GetSize();

		const_cast<PbsPacketFilter*>(this)->m_totalBytes += packetsize;
		const_cast<PbsPacketFilter*>(this)->GetLoadStats () += packetsize;

		FlowStats &ref = const_cast<PbsPacketFilter*>(this)->GetStatsForFlow(flowId);

		if (ref.firstTx == true)
		{
			ref.timeFirstTxPacket = Simulator::Now();
			ref.firstTx = false;
		}
		ref.txBytes += packetsize;
		ref.txPackets++;
		ref.timeLastTxPacket = Simulator::Now();
		ref.flowAge = ref.timeLastTxPacket - ref.timeFirstTxPacket;

		if (!m_usePbs) {
			bin_prio = 0; // No PBS, all packets on Q-0
		} else {
			const_cast<PbsPacketFilter*>(this)->MakePrioLimits();
			if (m_nonBlind) {
				UintegerValue flowsize = UintegerValue(ref.txBytes + 1);
				if (ref.flowSize == 0) {
					for (uint32_t i = 0; i < m_nodeptr->GetNApplications(); i++) {
						Ptr<BulkSendApplication> app = DynamicCast<BulkSendApplication>(m_nodeptr->GetApplication (i));
						Address source;
						AddressValue destination;
						if (app == 0)
							continue;
						Ptr<Socket> socket = app->GetSocket ();
						if (socket == 0)
							continue;
						socket->GetSockName (source);
						app->GetAttribute("Remote", destination);
						InetSocketAddress src = InetSocketAddress(InetSocketAddress::ConvertFrom(source));
						InetSocketAddress dst = InetSocketAddress(InetSocketAddress::ConvertFrom(destination.Get()));
						Ipv4Address src_ip = src.GetIpv4 ();
						uint16_t src_port = src.GetPort ();
						Ipv4Address dst_ip = dst.GetIpv4 ();
						uint16_t dst_port = dst.GetPort ();
						// 5-tuple hash
						uint8_t buf[17];
						src_ip.Serialize(buf);
						dst_ip.Serialize(buf + 4);
						buf[8] = 6;
						buf[9] = (src_port >> 8) & 0xff;
						buf[10] = src_port & 0xff;
						buf[11] = (dst_port >> 8) & 0xff;
						buf[12] = dst_port & 0xff;
						buf[13] = 0;
						buf[14] = 0;
						buf[15] = 0;
						buf[16] = 0;
						// compare application hash to packet hash
						if (Hash32 ((char *)buf, 17) == item->Hash ()) {
							app->GetAttribute("MaxBytes", flowsize);
							break;
						}
					}
					ref.flowSize = flowsize.Get ();
				} else { // flowSize already discovered
					flowsize = UintegerValue (ref.flowSize);
				}
				uint32_t bytes_remaining = flowsize.Get () - ref.txBytes;
				raw_prio = ref.flowAge.GetNanoSeconds () / pow (bytes_remaining, m_alpha);
			} else {
				raw_prio = ref.flowAge.GetNanoSeconds () / pow (ref.txBytes, m_alpha);
			}
			ref.rawPrioHistory.push_back(std::make_tuple(raw_prio, ref.txBytes, ref.flowAge.GetNanoSeconds()));
			if (!ref.firstTx && ref.flowAge != Seconds(0)) {
				for (bin_prio = 7; bin_prio >= 0; bin_prio--)
				{
					if (raw_prio <= m_prioLimits[bin_prio])
						break;
				}
			} else {
				bin_prio = 0;
			}
		}
		bin_prio = std::max(bin_prio, uint8_t(0));
		ref.prioHistory[bin_prio] += packetsize;
		ref.prioPacketHistory[bin_prio]++;

		PrioTag prioTag;
		prioTag.SetPrioValue (bin_prio);
		Ptr<Packet> pkt = item->GetPacket();
		pkt->AddPacketTag (prioTag);

		return bin_prio;
	}

	bool
	PbsPacketFilter::CheckProtocol (Ptr<QueueDiscItem> item) const
	{
		return true;
	}

} // namespace ns3

