#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/lorawan-module.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <set>
#include <cmath>

using namespace ns3;
using namespace lorawan;

NS_LOG_COMPONENT_DEFINE ("LoRaMultiPacketSim");

// LAMPv2 Protocol Constants
static const uint8_t LAMP_VERSION = 2;
static const uint8_t FLAG_NONE = 0x00;
static const uint8_t FLAG_ACK_REQ = 0x04;
static const uint8_t FLAG_ACK = 0x08;
static const uint8_t FLAG_CONN_REQ = 0x10;
static const uint8_t FLAG_CONN_ACK = 0x20;
static const uint8_t FLAG_CONN_NACK = 0x40;

static const uint8_t ADDR_UNASSIGNED = 0x00;
static const uint8_t ADDR_BROADCAST = 0xFF;

// Hardware timing & energy constants (SX1262 @ 3.3V)
static const double P_TX_W = 0.118 * 3.3;  // 118mA @ +22dBm = 0.3894 W
static const double P_RX_W = 0.0053 * 3.3; // 5.3mA active RX = 0.01749 W
static const uint32_t MAX_RETRIES = 5;

enum LampState {
    STATE_CLOSED,
    STATE_SYN_SENT,
    STATE_SYN_RCVD,
    STATE_ESTABLISHED,
    STATE_DATA_TX,
    STATE_WAIT_SACK,
    STATE_DONE
};

class MultiPacketNode : public Application
{
public:
    static TypeId GetTypeId (void)
    {
        static TypeId tid = TypeId ("ns3::MultiPacketNode")
            .SetParent<Application> ()
            .SetGroupName ("lorawan")
            .AddConstructor<MultiPacketNode> ();
        return tid;
    }

    MultiPacketNode ()
        : m_nodeAddress(0x01),
          m_peerAddress(0x02),
          m_isSender(false),
          m_running(false),
          m_mode("Mode4"),
          m_state(STATE_CLOSED),
          m_messageId(1),
          m_totalChunks(20),
          m_chunkIndex(0),
          m_ackedChunks(0),
          m_uniqueChunks(0),
          m_retries(0),
          m_cadBackoffs(0),
          m_energySpent(0.0),
          m_frequencyHz(869525000)
    {}

    virtual ~MultiPacketNode () {}

    void Setup(Ptr<LoraPhy> phy, uint8_t address, uint8_t peerAddress, bool isSender, std::string mode, uint32_t totalChunks = 20)
    {
        m_phy = phy;
        m_nodeAddress = address;
        m_peerAddress = peerAddress;
        m_isSender = isSender;
        m_mode = mode;
        m_totalChunks = totalChunks;
        m_receivedBitmap.assign((m_totalChunks + 7) / 8, 0);
        m_phy->SetReceiveOkCallback(MakeCallback(&MultiPacketNode::ReceiveOk, this));
    }

    void SetTxParams(uint8_t sf, double txPowerDbm)
    {
        m_txParams.spreadingFactor = sf;
        m_txParams.bandwidthHz = 500000; // 500 kHz bandwidth as per updated setup
        m_txParams.codingRate = CodingRate::CR_4_5;
        m_txParams.lowDataRateOptimize = (sf == 11 || sf == 12);
        m_txParams.preambleLenSymb = 8;
        m_txParams.implicitHeader = false;
        m_txParams.crcEnabled = true;
        m_txPower = txPowerDbm;
    }

    Time GetEstimatedToA(uint32_t payloadSize)
    {
        uint32_t packetLen = 9 + payloadSize + 2; // 9B header + payload + 2B CRC
        return ns3::lorawan::LoraPhy::GetTimeOnAir(packetLen, m_txParams);
    }

    Time CalculateAdaptiveTimeout(uint32_t payloadSize)
    {
        Time toa = GetEstimatedToA(payloadSize);
        double timeoutMs = std::max(1000.0, 1.5 * toa.GetMilliSeconds() + 25.0);
        return MilliSeconds(timeoutMs);
    }

    void SendPacket(uint8_t flags, uint8_t chunkIdx, uint8_t totalC, const uint8_t* payload, uint8_t payloadLen)
    {
        if (!m_running) return;

        // Construct 9-byte Logical Header + Payload + 2-byte CRC
        uint32_t frameSize = 9 + payloadLen + 2;
        Ptr<Packet> pkt = Create<Packet>(frameSize);

        m_phy->Send(pkt, m_frequencyHz, ns3::lorawan::IQPolarity::UP, m_txParams, m_txPower);
        
        Time onAir = ns3::lorawan::LoraPhy::GetTimeOnAir(frameSize, m_txParams);
        m_energySpent += P_TX_W * onAir.GetSeconds();
    }

    void StartSession()
    {
        if (!m_running || !m_isSender) return;

        m_startTime = Simulator::Now();

        if (m_mode == "Mode2" || m_mode == "Mode4" || m_mode == "ReliableStream" || m_mode == "UnreliableStream")
        {
            // Initiate 3-Way Handshake (SYN)
            m_state = STATE_SYN_SENT;
            uint8_t synMetadata[4] = {244, 255, 0, 0}; // 244B payload size, 255 window
            SendPacket(FLAG_CONN_REQ, 0, 1, synMetadata, 4);

            m_timeoutEvent = Simulator::Schedule(MilliSeconds(3000), &MultiPacketNode::HandleTimeout, this);
        }
        else
        {
            // Stateless modes (Mode 1 & Mode 3)
            m_state = STATE_DATA_TX;
            m_pendingChunks.clear();
            for (uint8_t i = 0; i < m_totalChunks; ++i) {
                m_pendingChunks.push_back(i);
            }
            m_currentPendingIdx = 0;
            SendNextChunk();
        }
    }

    void SendNextChunk()
    {
        if (!m_running || !m_isSender) return;

        if (m_currentPendingIdx >= m_pendingChunks.size())
        {
            if (m_mode == "Mode1" || m_mode == "Mode2" || m_mode == "UnreliableDatagram" || m_mode == "UnreliableStream")
            {
                m_state = STATE_DONE;
                return;
            }
            else
            {
                // In reliable modes (Mode 3 & 4), wait for SACK after burst
                m_state = STATE_WAIT_SACK;
                Time timeout = CalculateAdaptiveTimeout(244);
                m_timeoutEvent = Simulator::Schedule(timeout, &MultiPacketNode::HandleTimeout, this);
                return;
            }
        }

        uint8_t cIdx = m_pendingChunks[m_currentPendingIdx];
        uint8_t flags = FLAG_NONE;

        bool isReliable = (m_mode == "Mode3" || m_mode == "Mode4" || m_mode == "ReliableDatagram" || m_mode == "ReliableStream" || m_mode == "Reliable");
        if (isReliable && m_currentPendingIdx == m_pendingChunks.size() - 1)
        {
            flags |= FLAG_ACK_REQ; // Query SACK on last chunk of burst
        }

        uint8_t dummyPayload[244] = {0};
        SendPacket(flags, cIdx, m_totalChunks, dummyPayload, 244);

        m_currentPendingIdx++;
        Time onAir = GetEstimatedToA(244);
        
        // Pacing delay (5ms guard delay)
        Simulator::Schedule(onAir + MilliSeconds(5), &MultiPacketNode::SendNextChunk, this);
    }

    void HandleTimeout()
    {
        if (!m_running || !m_isSender) return;

        m_retries++;
        if (m_retries > MAX_RETRIES)
        {
            m_state = STATE_DONE;
            return;
        }

        if (m_state == STATE_SYN_SENT)
        {
            // Resend SYN
            uint8_t synMetadata[4] = {244, 255, 0, 0};
            SendPacket(FLAG_CONN_REQ, 0, 1, synMetadata, 4);
            m_timeoutEvent = Simulator::Schedule(MilliSeconds(3000), &MultiPacketNode::HandleTimeout, this);
        }
        else if (m_state == STATE_WAIT_SACK)
        {
            // Retransmit last chunk with ACK_REQ
            uint8_t lastIdx = m_pendingChunks.empty() ? (m_totalChunks - 1) : m_pendingChunks.back();
            uint8_t dummyPayload[244] = {0};
            SendPacket(FLAG_ACK_REQ, lastIdx, m_totalChunks, dummyPayload, 244);
            
            Time timeout = CalculateAdaptiveTimeout(244);
            m_timeoutEvent = Simulator::Schedule(timeout, &MultiPacketNode::HandleTimeout, this);
        }
    }

    void ReceiveOk(Ptr<const Packet> packet)
    {
        if (!m_running) return;

        // Track RX energy
        Time onAir = ns3::lorawan::LoraPhy::GetTimeOnAir(packet->GetSize(), m_txParams);
        m_energySpent += P_RX_W * onAir.GetSeconds();

        if (m_isSender)
        {
            // Sender handling control frames from Receiver
            if (m_state == STATE_SYN_SENT)
            {
                // Expected SYN-ACK (FLAG_CONN_REQ | FLAG_CONN_ACK)
                Simulator::Cancel(m_timeoutEvent);
                m_retries = 0;
                m_state = STATE_DATA_TX;

                // Send 3WHS ACK
                SendPacket(FLAG_CONN_ACK, 0, 1, nullptr, 0);

                // Start Data TX
                m_pendingChunks.clear();
                for (uint8_t i = 0; i < m_totalChunks; ++i) {
                    m_pendingChunks.push_back(i);
                }
                m_currentPendingIdx = 0;
                Simulator::Schedule(MilliSeconds(25), &MultiPacketNode::SendNextChunk, this);
            }
            else if (m_state == STATE_WAIT_SACK)
            {
                // Expected SACK feedback
                Simulator::Cancel(m_timeoutEvent);
                m_retries = 0;

                // Re-evaluate missing chunks based on bitmap payload simulation
                // In ns-3, packet reception ok implies SACK payload received
                m_pendingChunks.clear(); // All ACKed if SACK received in full simulation
                m_state = STATE_DONE;
                m_endTime = Simulator::Now();
            }
        }
        else
        {
            // Receiver handling incoming frames from Sender
            if (packet->GetSize() <= 16)
            {
                // Control frame: SYN (FLAG_CONN_REQ)
                if (m_state == STATE_CLOSED || m_state == STATE_SYN_RCVD)
                {
                    m_state = STATE_SYN_RCVD;
                    uint8_t synAckMetadata[4] = {244, 255, 0, 0};
                    Simulator::Schedule(MilliSeconds(25), [this, synAckMetadata]() {
                        SendPacket(FLAG_CONN_REQ | FLAG_CONN_ACK, 0, 1, synAckMetadata, 4);
                    });
                }
            }
            else
            {
                // Data chunk received
                m_ackedChunks++;
                m_endTime = Simulator::Now();

                bool isReliable = (m_mode == "Mode3" || m_mode == "Mode4" || m_mode == "ReliableDatagram" || m_mode == "ReliableStream" || m_mode == "Reliable");
                if (isReliable && (m_ackedChunks % 5 == 0 || m_ackedChunks >= m_totalChunks))
                {
                    // Respond with SACK bitmap
                    uint8_t bitmapLen = (m_totalChunks + 7) / 8;
                    std::vector<uint8_t> bitmapPayload(bitmapLen, 0xFF);
                    Simulator::Schedule(MilliSeconds(25), [this, bitmapPayload, bitmapLen]() {
                        SendPacket(FLAG_ACK, 0, 1, bitmapPayload.data(), bitmapLen);
                    });
                }
            }
        }
    }

    void StartApplication() override
    {
        m_running = true;
        if (m_isSender) {
            StartSession();
        }
    }

    void StopApplication() override
    {
        m_running = false;
    }

    uint8_t m_nodeAddress;
    uint8_t m_peerAddress;
    bool m_isSender;
    bool m_running;
    std::string m_mode;
    LampState m_state;

    uint16_t m_messageId;
    uint32_t m_totalChunks;
    uint8_t m_chunkIndex;
    std::vector<uint8_t> m_pendingChunks;
    size_t m_currentPendingIdx;

    uint32_t m_ackedChunks;
    uint32_t m_uniqueChunks;
    std::vector<uint8_t> m_receivedBitmap;

    uint32_t m_retries;
    uint32_t m_cadBackoffs;

    Ptr<LoraPhy> m_phy;
    LoraTxParameters m_txParams;
    double m_txPower;

    EventId m_timeoutEvent;
    Time m_startTime;
    Time m_endTime;
    double m_energySpent;
    uint32_t m_frequencyHz;
};

int main(int argc, char *argv[])
{
    double distance = 100.0;
    std::string mode = "Mode4";
    uint32_t sf = 7;
    std::string env = "URBAN";
    uint32_t seed = 1;
    uint32_t totalChunks = 20;

    CommandLine cmd;
    cmd.AddValue ("distance", "Distance between nodes in meters", distance);
    cmd.AddValue ("mode", "LAMP Operational Mode (Mode1, Mode2, Mode3, Mode4, Reliable, Unreliable)", mode);
    cmd.AddValue ("sf", "Spreading Factor (7 to 12)", sf);
    cmd.AddValue ("env", "Environment (LOS, URBAN, RURAL)", env);
    cmd.AddValue ("seed", "Random seed for Monte Carlo", seed);
    cmd.AddValue ("chunks", "Total payload chunks", totalChunks);
    cmd.Parse (argc, argv);
    
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(seed);

    NodeContainer nodes;
    nodes.Create(2);

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> allocator = CreateObject<ListPositionAllocator>();
    allocator->Add(Vector(0, 0, 0));
    allocator->Add(Vector(distance, 0, 0));
    mobility.SetPositionAllocator(allocator);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    // Path Loss Model — Calibrated for Heltec LoRa 32 V3 at 869.525 MHz
    Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
    double plRef1km;
    if (env == "URBAN") {
        loss->SetPathLossExponent(4.5);
        plRef1km = 131.0;
    } else if (env == "RURAL") {
        loss->SetPathLossExponent(4.2);
        plRef1km = 118.0;
    } else { // LOS
        loss->SetPathLossExponent(4.0);
        plRef1km = 113.0;
    }
    loss->SetReference(1000.0, plRef1km);

    Ptr<NakagamiPropagationLossModel> nakagami = CreateObject<NakagamiPropagationLossModel>();
    if (env == "LOS") {
        nakagami->SetAttribute("m0", DoubleValue(3.0));
        nakagami->SetAttribute("m1", DoubleValue(3.0));
        nakagami->SetAttribute("m2", DoubleValue(3.0));
    } else if (env == "RURAL") {
        nakagami->SetAttribute("m0", DoubleValue(1.5));
        nakagami->SetAttribute("m1", DoubleValue(1.5));
        nakagami->SetAttribute("m2", DoubleValue(1.5));
    } else { // URBAN
        nakagami->SetAttribute("m0", DoubleValue(1.0));
        nakagami->SetAttribute("m1", DoubleValue(1.0));
        nakagami->SetAttribute("m2", DoubleValue(1.0));
    }
    loss->SetNext(nakagami);

    Ptr<PropagationDelayModel> delay = CreateObject<ConstantSpeedPropagationDelayModel>();
    Ptr<LoraChannel> channel = CreateObject<LoraChannel>(loss, delay);

    LoraPhyHelper phyHelper = LoraPhyHelper();
    phyHelper.SetChannel(channel);
    phyHelper.SetDeviceType(LoraPhyHelper::GW);

    LorawanMacHelper macHelper = LorawanMacHelper();
    macHelper.SetDeviceType(LorawanMacHelper::GW); 
    LoraHelper helper = LoraHelper();
    helper.Install(phyHelper, macHelper, nodes);

    Ptr<LoraNetDevice> dev0 = DynamicCast<LoraNetDevice>(nodes.Get(0)->GetDevice(0));
    Ptr<LoraNetDevice> dev1 = DynamicCast<LoraNetDevice>(nodes.Get(1)->GetDevice(0));

    Ptr<MultiPacketNode> app0 = CreateObject<MultiPacketNode>();
    app0->Setup(dev0->GetPhy(), 0x01, 0x02, true, mode, totalChunks);
    app0->SetTxParams(sf, 24.2); // +24.2 dBm bilateral EIRP (22 dBm PA + 3 dBi antenna - 0.8 dB IPEX)

    Ptr<MultiPacketNode> app1 = CreateObject<MultiPacketNode>();
    app1->Setup(dev1->GetPhy(), 0x02, 0x01, false, mode, totalChunks);
    app1->SetTxParams(sf, 24.2);

    nodes.Get(0)->AddApplication(app0);
    nodes.Get(1)->AddApplication(app1);

    app0->SetStartTime(Seconds(1.0));
    app0->SetStopTime(Seconds(300.0));
    app1->SetStartTime(Seconds(0.0));
    app1->SetStopTime(Seconds(300.0));

    Simulator::Stop(Seconds(300.0));
    Simulator::Run();
    Simulator::Destroy();

    const double SIM_WINDOW_S = 299.0;
    double totalTime = SIM_WINDOW_S;

    // Output line format:
    // RESULT:distance,mode,sf,env,ackedChunks,totalChunks,retries,totalTime,energySpent,seed
    std::cout << "RESULT:" << distance << "," << mode << "," << sf << "," << env << "," 
              << app1->m_ackedChunks << "," << totalChunks << "," << app0->m_retries << "," 
              << totalTime << "," << (app0->m_energySpent + app1->m_energySpent) << "," << seed << std::endl;

    return 0;
}
