#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/lorawan-module.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <set>
#include <cmath>
#include <iomanip>

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

// Mirrors LoRaMultiPacketConfig (components/LoRaMultiPacket/include/ProtocolConfig.hpp)
static const uint32_t MAX_RETRIES = 5;               // R_max: repair-round budget
static const uint32_t SYN_TIMEOUT_MS = 3000;         // DEFAULT_SYN_TIMEOUT_MS
static const double MIN_ACK_TIMEOUT_MS = 1000.0;     // MIN_ACK_TIMEOUT_MS
static const double ACK_TIMEOUT_SAFETY_FACTOR = 1.5; // ACK_TIMEOUT_SAFETY_FACTOR
static const double GUARD_MS = 25.0;                 // SACK_PREAMBLE_GUARD_DELAY_MS = ACK_TIMEOUT_GUARD_MS
static const uint32_t POST_TX_GUARD_MS = 5;          // POST_TX_GUARD_DELAY_MS
static const uint32_t CHUNK_PAYLOAD = 244;           // LORA_MAX_PAYLOAD_SIZE
static const uint32_t CRC_BYTES = 2;

enum LampState {
    STATE_CLOSED,
    STATE_SYN_SENT,
    STATE_DATA_TX,
    STATE_WAIT_SACK, // waiting for SACK bitmap (Mode 3/4) or per-frame ACK (stop-and-wait)
    STATE_DONE
};

enum class Proto { BEST_EFFORT, SACK, STOP_AND_WAIT };

struct ModeSpec {
    bool stateful = false;     // 3-way handshake before data
    Proto proto = Proto::SACK;
    bool swOptimized = false;  // stop-and-wait with a tight ACK timeout
};

// Mode1..4 are the LAMP modes; SWBase/SWOpt[Conn] are stop-and-wait baselines
// (one ACK per frame, same R_max per chunk); "Conn" adds the 3WHS.
static bool ParseMode(const std::string &m, ModeSpec &s)
{
    if (m == "Mode1" || m == "UnreliableDatagram" || m == "Unreliable") { s = {false, Proto::BEST_EFFORT, false}; return true; }
    if (m == "Mode2" || m == "UnreliableStream")                       { s = {true,  Proto::BEST_EFFORT, false}; return true; }
    if (m == "Mode3" || m == "ReliableDatagram" || m == "Reliable")    { s = {false, Proto::SACK, false};        return true; }
    if (m == "Mode4" || m == "ReliableStream")                         { s = {true,  Proto::SACK, false};        return true; }
    if (m.rfind("SW", 0) == 0) {
        s.proto = Proto::STOP_AND_WAIT;
        s.swOptimized = m.find("Opt") != std::string::npos;
        s.stateful = m.size() >= 4 && m.compare(m.size() - 4, 4, "Conn") == 0;
        return true;
    }
    return false;
}

/** 9-byte LAMPv2 logical header (CRC-16 is accounted for as 2 payload bytes). */
class LampHeader : public Header
{
public:
    static TypeId GetTypeId (void)
    {
        static TypeId tid = TypeId ("ns3::LampHeader").SetParent<Header> ().AddConstructor<LampHeader> ();
        return tid;
    }
    TypeId GetInstanceTypeId (void) const override { return GetTypeId (); }
    uint32_t GetSerializedSize (void) const override { return 9; }
    void Print (std::ostream &os) const override { os << "flags=" << (int)flags << " chunk=" << (int)chunkIdx << "/" << (int)total; }
    void Serialize (Buffer::Iterator i) const override
    {
        i.WriteU8 (LAMP_VERSION); i.WriteU8 (flags); i.WriteU8 (src); i.WriteU8 (dst);
        i.WriteU16 (msgId); i.WriteU8 (chunkIdx); i.WriteU8 (total); i.WriteU8 (payloadLen);
    }
    uint32_t Deserialize (Buffer::Iterator i) override
    {
        i.ReadU8 (); flags = i.ReadU8 (); src = i.ReadU8 (); dst = i.ReadU8 ();
        msgId = i.ReadU16 (); chunkIdx = i.ReadU8 (); total = i.ReadU8 (); payloadLen = i.ReadU8 ();
        return 9;
    }
    uint8_t flags = 0, src = 0, dst = 0, chunkIdx = 0, total = 0, payloadLen = 0;
    uint16_t msgId = 1;
};
NS_OBJECT_ENSURE_REGISTERED (LampHeader);

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

    MultiPacketNode () {}
    virtual ~MultiPacketNode () {}

    void Setup(Ptr<LoraPhy> phy, uint8_t address, uint8_t peerAddress, bool isSender, const ModeSpec &spec, uint32_t totalChunks)
    {
        m_phy = phy;
        m_nodeAddress = address;
        m_peerAddress = peerAddress;
        m_isSender = isSender;
        m_spec = spec;
        m_totalChunks = totalChunks;
        m_rxChunk.assign(totalChunks, false);
        m_phy->SetReceiveOkCallback(MakeCallback(&MultiPacketNode::ReceiveOk, this));
    }

    void SetTxParams(uint8_t sf, double txPowerDbm, uint32_t bandwidthHz)
    {
        m_txParams.spreadingFactor = sf;
        m_txParams.bandwidthHz = bandwidthHz;
        m_txParams.codingRate = CodingRate::CR_4_5;
        m_txParams.lowDataRateOptimize = (sf == 11 || sf == 12);
        m_txParams.preambleLenSymb = 8;
        m_txParams.implicitHeader = false;
        m_txParams.crcEnabled = true;
        m_txPower = txPowerDbm;
    }

    Time ToA(uint32_t payloadSize) const
    {
        return LoraPhy::GetTimeOnAir(9 + payloadSize + CRC_BYTES, m_txParams);
    }

    // Mirrors LoRaProtocol::calculateAckTimeoutMs(): armed after the burst has been transmitted.
    Time SackTimeout() const
    {
        double ms = ACK_TIMEOUT_SAFETY_FACTOR * (ToA(CHUNK_PAYLOAD).GetSeconds() * 1000.0 * m_totalChunks
                                                + ToA((m_totalChunks + 7) / 8).GetSeconds() * 1000.0)
                    + GUARD_MS + GUARD_MS;
        return Seconds(std::max(MIN_ACK_TIMEOUT_MS, ms) / 1000.0);
    }

    // Stop-and-wait: same rule applied to a single frame (base), or ACK airtime plus margin (optimized).
    Time SwTimeout() const
    {
        double ackMs = ToA(0).GetSeconds() * 1000.0;
        if (m_spec.swOptimized)
            return Seconds((ackMs + 2 * GUARD_MS) / 1000.0);
        double ms = ACK_TIMEOUT_SAFETY_FACTOR * (ToA(CHUNK_PAYLOAD).GetSeconds() * 1000.0 + ackMs) + GUARD_MS + GUARD_MS;
        return Seconds(std::max(MIN_ACK_TIMEOUT_MS, ms) / 1000.0);
    }

    // Builds header + payload (+2 B CRC placeholder), transmits, returns time on air.
    Time SendFrame(uint8_t flags, uint8_t chunkIdx, uint8_t total, const std::vector<uint8_t> &payload, uint32_t payloadLen)
    {
        std::vector<uint8_t> body(payloadLen + CRC_BYTES, 0);
        std::copy(payload.begin(), payload.begin() + std::min<size_t>(payload.size(), payloadLen), body.begin());
        Ptr<Packet> pkt = Create<Packet>(body.data(), body.size());
        LampHeader h;
        h.flags = flags; h.src = m_nodeAddress; h.dst = m_peerAddress;
        h.msgId = 1; h.chunkIdx = chunkIdx; h.total = total; h.payloadLen = payloadLen;
        pkt->AddHeader(h);

        m_phy->Send(pkt, m_frequencyHz, IQPolarity::UP, m_txParams, m_txPower);
        Time onAir = ToA(payloadLen);
        m_energySpent += P_TX_W * onAir.GetSeconds();
        m_txAirS += onAir.GetSeconds();
        return onAir;
    }

    void SendLater(Time delay, uint8_t flags, uint8_t chunkIdx, uint8_t total, std::vector<uint8_t> payload, uint32_t payloadLen)
    {
        Simulator::Schedule(delay, [this, flags, chunkIdx, total, payload, payloadLen]() {
            if (m_running) SendFrame(flags, chunkIdx, total, payload, payloadLen);
        });
    }

    // ---- sender ----------------------------------------------------------------

    void StartSession()
    {
        if (!m_running || !m_isSender) return;
        m_startTime = Simulator::Now();

        if (m_spec.stateful)
        {
            m_state = STATE_SYN_SENT;
            SendSyn();
        }
        else
        {
            StartData();
        }
    }

    void SendSyn()
    {
        Time onAir = SendFrame(FLAG_CONN_REQ, 0, 1, {CHUNK_PAYLOAD, 255, 0, 0}, 4);
        m_timeoutEvent = Simulator::Schedule(onAir + MilliSeconds(SYN_TIMEOUT_MS), &MultiPacketNode::HandleSynTimeout, this);
    }

    void HandleSynTimeout()
    {
        if (!m_running || m_state != STATE_SYN_SENT) return;
        if (m_synRetries >= MAX_RETRIES) { Finish(false); return; }
        m_synRetries++;
        SendSyn();
    }

    void StartData()
    {
        m_state = STATE_DATA_TX;
        m_pending.clear();
        for (uint32_t i = 0; i < m_totalChunks; ++i) m_pending.push_back(i);
        m_idx = 0;
        if (m_spec.proto == Proto::STOP_AND_WAIT) { m_swCur = 0; SendSwChunk(); }
        else SendNextBurstChunk();
    }

    void Finish(bool ok)
    {
        Simulator::Cancel(m_timeoutEvent);
        m_state = STATE_DONE;
        m_senderOk = ok;
        m_senderDoneTime = Simulator::Now();
    }

    // Burst (Mode 1-4): last chunk of the burst carries ACK_REQ in reliable modes.
    void SendNextBurstChunk()
    {
        if (!m_running || m_state != STATE_DATA_TX) return;

        if (m_idx >= m_pending.size())
        {
            if (m_spec.proto == Proto::BEST_EFFORT) { Finish(true); return; }
            m_state = STATE_WAIT_SACK;
            m_timeoutEvent = Simulator::Schedule(SackTimeout(), &MultiPacketNode::HandleSackTimeout, this);
            return;
        }

        bool last = (m_idx == m_pending.size() - 1);
        uint8_t flags = (m_spec.proto == Proto::SACK && last) ? FLAG_ACK_REQ : FLAG_NONE;
        Time onAir = SendFrame(flags, m_pending[m_idx], m_totalChunks, {}, CHUNK_PAYLOAD);
        m_idx++;
        Simulator::Schedule(onAir + MilliSeconds(POST_TX_GUARD_MS), &MultiPacketNode::SendNextBurstChunk, this);
    }

    // SACK timeout: re-send the last chunk of the message with ACK_REQ (LoRaProtocol::sendReliable).
    void HandleSackTimeout()
    {
        if (!m_running || m_state != STATE_WAIT_SACK) return;
        if (m_rounds >= MAX_RETRIES) { Finish(false); return; }
        m_rounds++;
        Time onAir = SendFrame(FLAG_ACK_REQ, m_totalChunks - 1, m_totalChunks, {}, CHUNK_PAYLOAD);
        m_timeoutEvent = Simulator::Schedule(onAir + MilliSeconds(POST_TX_GUARD_MS) + SackTimeout(), &MultiPacketNode::HandleSackTimeout, this);
    }

    // SACK received: complete, or selectively re-send the chunks whose bit is 0.
    // Repair rounds (timeouts and SACK-with-gaps) share one budget R_max, as in the paper and in LoRaProtocol::sendReliable.
    void OnSack(const std::vector<uint8_t> &bitmap)
    {
        Simulator::Cancel(m_timeoutEvent);
        std::vector<uint8_t> missing;
        for (uint32_t i = 0; i < m_totalChunks; ++i)
        {
            bool got = (i / 8 < bitmap.size()) && (bitmap[i / 8] & (1 << (i % 8)));
            if (!got) missing.push_back(i);
        }
        if (missing.empty()) { Finish(true); return; }
        if (m_rounds >= MAX_RETRIES) { Finish(false); return; }
        m_rounds++;
        m_pending = missing;
        m_idx = 0;
        m_state = STATE_DATA_TX;
        SendNextBurstChunk();
    }

    // Stop-and-wait baseline: one frame, one ACK, R_max retries per chunk.
    void SendSwChunk()
    {
        if (!m_running || m_state == STATE_DONE) return;
        m_state = STATE_WAIT_SACK;
        Time onAir = SendFrame(FLAG_ACK_REQ, m_swCur, m_totalChunks, {}, CHUNK_PAYLOAD);
        m_timeoutEvent = Simulator::Schedule(onAir + MilliSeconds(POST_TX_GUARD_MS) + SwTimeout(), &MultiPacketNode::HandleSwTimeout, this);
    }

    void HandleSwTimeout()
    {
        if (!m_running || m_state != STATE_WAIT_SACK) return;
        if (m_swRetries >= MAX_RETRIES) { Finish(false); return; }
        m_swRetries++;
        m_rounds++;
        SendSwChunk();
    }

    void OnSwAck(uint8_t chunkIdx)
    {
        if (m_state != STATE_WAIT_SACK || chunkIdx != m_swCur) return; // stale ACK
        Simulator::Cancel(m_timeoutEvent);
        m_swRetries = 0;
        m_swCur++;
        if (m_swCur >= m_totalChunks) { Finish(true); return; }
        Simulator::Schedule(MilliSeconds(POST_TX_GUARD_MS), &MultiPacketNode::SendSwChunk, this);
    }

    // ---- both ends -------------------------------------------------------------

    void ReceiveOk(Ptr<const Packet> packet)
    {
        if (!m_running) return;

        m_energySpent += P_RX_W * LoraPhy::GetTimeOnAir(packet->GetSize(), m_txParams).GetSeconds();

        Ptr<Packet> p = packet->Copy();
        LampHeader h;
        if (p->GetSize() < h.GetSerializedSize()) return;
        p->RemoveHeader(h);
        if (h.dst != m_nodeAddress) return;

        std::vector<uint8_t> body(p->GetSize(), 0);
        p->CopyData(body.data(), body.size());

        if (m_isSender)
        {
            if (m_state == STATE_SYN_SENT && (h.flags & FLAG_CONN_REQ) && (h.flags & FLAG_CONN_ACK))
            {
                Simulator::Cancel(m_timeoutEvent);
                SendFrame(FLAG_CONN_ACK, 0, 1, {}, 0);
                Simulator::Schedule(MilliSeconds(GUARD_MS), &MultiPacketNode::StartData, this);
            }
            else if ((h.flags & FLAG_ACK) && m_state == STATE_WAIT_SACK)
            {
                if (m_spec.proto == Proto::SACK)
                    OnSack(std::vector<uint8_t>(body.begin(), body.begin() + std::min<size_t>(h.payloadLen, body.size())));
                else if (m_spec.proto == Proto::STOP_AND_WAIT)
                    OnSwAck(h.chunkIdx);
            }
            return;
        }

        // Receiver
        if ((h.flags & FLAG_CONN_REQ) && !(h.flags & FLAG_CONN_ACK))
        {
            SendLater(MilliSeconds(GUARD_MS), FLAG_CONN_REQ | FLAG_CONN_ACK, 0, 1, {CHUNK_PAYLOAD, 255, 0, 0}, 4);
            return;
        }
        if (h.flags & (FLAG_CONN_ACK | FLAG_ACK | FLAG_CONN_NACK)) return; // handshake completion: nothing to do

        // Data chunk: count it once, whatever the number of (re)transmissions.
        if (h.chunkIdx < m_totalChunks && !m_rxChunk[h.chunkIdx])
        {
            m_rxChunk[h.chunkIdx] = true;
            m_uniqueChunks++;
            m_lastNewTime = Simulator::Now();
        }

        if (h.flags & FLAG_ACK_REQ)
        {
            if (m_spec.proto == Proto::SACK)
            {
                std::vector<uint8_t> bitmap((m_totalChunks + 7) / 8, 0);
                for (uint32_t i = 0; i < m_totalChunks; ++i)
                    if (m_rxChunk[i]) bitmap[i / 8] |= (1 << (i % 8));
                SendLater(MilliSeconds(GUARD_MS), FLAG_ACK, 0, 1, bitmap, bitmap.size());
            }
            else if (m_spec.proto == Proto::STOP_AND_WAIT)
            {
                SendLater(MilliSeconds(GUARD_MS), FLAG_ACK, h.chunkIdx, m_totalChunks, {}, 0);
            }
        }
    }

    void StartApplication() override
    {
        m_running = true;
        if (m_isSender) StartSession();
    }

    void StopApplication() override { m_running = false; }

    uint8_t m_nodeAddress = 0x01;
    uint8_t m_peerAddress = 0x02;
    bool m_isSender = false;
    bool m_running = false;
    ModeSpec m_spec;
    LampState m_state = STATE_CLOSED;

    uint32_t m_totalChunks = 20;
    std::vector<uint8_t> m_pending;
    size_t m_idx = 0;
    uint32_t m_swCur = 0;
    uint32_t m_swRetries = 0;
    uint32_t m_synRetries = 0;
    uint32_t m_rounds = 0;      // repair rounds used (timeouts + SACK-with-gaps, or stop-and-wait retries)
    bool m_senderOk = false;

    std::vector<bool> m_rxChunk;
    uint32_t m_uniqueChunks = 0; // distinct chunks received

    Ptr<LoraPhy> m_phy;
    LoraTxParameters m_txParams;
    double m_txPower = 0.0;

    EventId m_timeoutEvent;
    Time m_startTime;
    Time m_lastNewTime;
    Time m_senderDoneTime;
    double m_energySpent = 0.0;
    double m_txAirS = 0.0; // total time on air transmitted by this node
    uint32_t m_frequencyHz = 869525000;
};

int main(int argc, char *argv[])
{
    double distance = 100.0;
    std::string mode = "Mode4";
    uint32_t sf = 7;
    std::string env = "URBAN";
    uint32_t seed = 1;
    uint32_t totalChunks = 20;
    uint32_t bandwidthHz = 125000; // ETSI EN 300 220 Band P (869.4-869.65 MHz) allows <= 250 kHz channels
    double txPowerDbm = 24.2;      // +24.2 dBm EIRP (22 dBm PA + 3 dBi antenna - 0.8 dB IPEX)
    // Optional channel overrides (negative = keep the per-environment default below)
    double nOverride = -1.0, plRefOverride = -1.0, mOverride = -1.0, sigmaOverride = -1.0;

    CommandLine cmd;
    cmd.AddValue ("distance", "Distance between nodes in meters", distance);
    cmd.AddValue ("mode", "Mode1..Mode4, or stop-and-wait baselines SWBase, SWOpt, SWBaseConn, SWOptConn", mode);
    cmd.AddValue ("sf", "Spreading Factor (7 to 12)", sf);
    cmd.AddValue ("env", "Environment (LOS, URBAN, RURAL)", env);
    cmd.AddValue ("seed", "Random seed for Monte Carlo", seed);
    cmd.AddValue ("chunks", "Total payload chunks", totalChunks);
    cmd.AddValue ("bw", "Bandwidth in Hz", bandwidthHz);
    cmd.AddValue ("tx", "TX power incl. antenna gain, dBm EIRP", txPowerDbm);
    cmd.AddValue ("n", "Path-loss exponent override", nOverride);
    cmd.AddValue ("pl1km", "Path loss at 1 km override, dB", plRefOverride);
    cmd.AddValue ("m", "Nakagami m override", mOverride);
    cmd.AddValue ("sigma", "Log-normal shadowing std-dev override, dB (0 = none)", sigmaOverride);
    cmd.Parse (argc, argv);

    ModeSpec spec;
    if (!ParseMode(mode, spec)) { std::cerr << "Unknown mode: " << mode << std::endl; return 1; }

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

    // Path loss: log-distance + per-run log-normal shadowing + Nakagami-m fast fading.
    // n / PL(1 km) / sigma are published 868 MHz fits (single-slope log-distance models):
    //   URBAN: Petajajarvi et al., ITST 2015, Table IV, car on ground, Oulu (n 2.32, 128.95 dB, sigma 7.8 dB)
    //   RURAL: Azevedo & Mendonca, Sensors 24(12):3877, Table 12, re-fit of Chall et al. (n 2.93, alpha 22.14 dB
    //          -> 110.0 dB at 1 km; sigma ~ fit RMSE 6.3 dB). Review authors' re-fit, not the original paper.
    //   LOS:   free space (n 2, 91.2 dB at 869.5 MHz, no shadowing)
    //   WATER: Petajajarvi et al., Table IV, boat on water (n 1.76, 126.43 dB, sigma 8.0 dB)
    // Nakagami m (3 / 1.5 / 1) is a modelling assumption: no LoRa measurement was found for it.
    // Stress tests use the overrides (e.g. --n=4.0 --pl1km=113).
    double n, plRef1km, m, sigma;
    if (env == "URBAN")      { n = 2.32; plRef1km = 128.95; m = 1.0; sigma = 7.8; }
    else if (env == "RURAL") { n = 2.93; plRef1km = 110.04; m = 1.5; sigma = 6.3; }
    else if (env == "WATER") { n = 1.76; plRef1km = 126.43; m = 3.0; sigma = 8.0; }
    else                     { n = 2.0;  plRef1km = 91.23;  m = 3.0; sigma = 0.0; } // LOS
    if (nOverride > 0) n = nOverride;
    if (plRefOverride > 0) plRef1km = plRefOverride;
    if (mOverride > 0) m = mOverride;
    if (sigmaOverride >= 0) sigma = sigmaOverride;

    // Shadowing is constant over a transfer (one draw per run/seed) and shifts the reference loss.
    double shadowDb = 0.0;
    if (sigma > 0)
    {
        Ptr<NormalRandomVariable> shadow = CreateObject<NormalRandomVariable>();
        shadow->SetAttribute("Mean", DoubleValue(0.0));
        shadow->SetAttribute("Variance", DoubleValue(sigma * sigma));
        shadowDb = shadow->GetValue();
        plRef1km += shadowDb;
    }

    Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
    loss->SetPathLossExponent(n);
    loss->SetReference(1000.0, plRef1km);

    Ptr<NakagamiPropagationLossModel> nakagami = CreateObject<NakagamiPropagationLossModel>();
    nakagami->SetAttribute("m0", DoubleValue(m));
    nakagami->SetAttribute("m1", DoubleValue(m));
    nakagami->SetAttribute("m2", DoubleValue(m));
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

    // Register 869.525 MHz frequency on PHY receivers
    Ptr<GatewayLoraPhy> gwPhy0 = DynamicCast<GatewayLoraPhy>(dev0->GetPhy());
    Ptr<GatewayLoraPhy> gwPhy1 = DynamicCast<GatewayLoraPhy>(dev1->GetPhy());
    if (gwPhy0) gwPhy0->AddFrequency(869525000);
    if (gwPhy1) gwPhy1->AddFrequency(869525000);

    Ptr<MultiPacketNode> app0 = CreateObject<MultiPacketNode>();
    app0->Setup(dev0->GetPhy(), 0x01, 0x02, true, spec, totalChunks);
    app0->SetTxParams(sf, txPowerDbm, bandwidthHz);

    Ptr<MultiPacketNode> app1 = CreateObject<MultiPacketNode>();
    app1->Setup(dev1->GetPhy(), 0x02, 0x01, false, spec, totalChunks);
    app1->SetTxParams(sf, txPowerDbm, bandwidthHz);

    nodes.Get(0)->AddApplication(app0);
    nodes.Get(1)->AddApplication(app1);

    // Worst case (SF10, 5 repair rounds) needs several hundred seconds; the run ends when events run out.
    const double SIM_END_S = 3600.0;
    app0->SetStartTime(Seconds(1.0));
    app0->SetStopTime(Seconds(SIM_END_S));
    app1->SetStartTime(Seconds(0.0));
    app1->SetStopTime(Seconds(SIM_END_S));

    Simulator::Stop(Seconds(SIM_END_S));
    Simulator::Run();
    Simulator::Destroy();

    // Nominal received SNR (no fading, no shadowing): path loss at this distance vs. thermal noise (NF 6 dB).
    // Effective SNR of this run = meanSnrDb - shadowDb.
    double plMean = (plRef1km - shadowDb) + 10.0 * n * std::log10(distance / 1000.0); // nominal, without this run's shadowing
    double rxMeanDbm = txPowerDbm - plMean;
    double meanSnrDb = rxMeanDbm - (-174.0 + 10.0 * std::log10((double)bandwidthHz) + 6.0);

    double lastNewS = app1->m_uniqueChunks > 0 ? (app1->m_lastNewTime - app0->m_startTime).GetSeconds() : -1.0;
    double senderDoneS = app0->m_state == STATE_DONE ? (app0->m_senderDoneTime - app0->m_startTime).GetSeconds() : -1.0;

    // Output line format:
    // RESULT:distance,mode,sf,env,uniqueChunks,totalChunks,rounds,lastNewS,energyJ,seed,senderOk,senderDoneS,meanSnrDb,bwHz,shadowDb,senderTxAirS
    //   uniqueChunks = distinct chunks reassembled at the receiver (duplicates not counted)
    //   lastNewS     = time from session start to the last new chunk at the receiver (-1 if none)
    //   senderDoneS  = time until the sender stopped (SACK-complete / best-effort burst end / give-up), -1 if never
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "RESULT:" << distance << "," << mode << "," << sf << "," << env << ","
              << app1->m_uniqueChunks << "," << totalChunks << "," << (app0->m_rounds + app0->m_synRetries) << ","
              << lastNewS << "," << (app0->m_energySpent + app1->m_energySpent) << "," << seed << ","
              << (app0->m_senderOk ? 1 : 0) << "," << senderDoneS << "," << meanSnrDb << "," << bandwidthHz << "," << shadowDb << "," << app0->m_txAirS << std::endl;

    return 0;
}
