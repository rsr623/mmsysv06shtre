// Copyright (c) 2023. ByteDance Inc. All rights reserved.


#pragma once

#include <cstdint>
#include <chrono>
#include "utils/thirdparty/quiche/rtt_stats.h"
#include "basefw/base/log.h"
#include "utils/rttstats.h"
#include "utils/transporttime.h"
#include "utils/defaultclock.hpp"
#include "sessionstreamcontroller.hpp"
#include "packettype.h"

struct CrossSessionData{
    std::map<basefw::ID, uint32_t> SessionCwndMap;
    std::map<basefw::ID, uint32_t> SessionRttMap;
    std::map<basefw::ID, uint32_t> SessionLossMap;

    void UpdateSelfData(const basefw::ID& sessionid, uint32_t cwnd, uint32_t rtt, uint32_t loss)
    {
        SessionCwndMap[sessionid] = cwnd;
        SessionRttMap[sessionid] = rtt;
        SessionLossMap[sessionid] = loss;
    }
};

enum class CongestionCtlType : uint8_t
{
    none = 0,
    reno = 1,
    cubic = 2,
    olia = 3
};

struct LossEvent
{
    bool valid{ false };
    // There may be multiple timeout events at one time
    std::vector<InflightPacket> lossPackets;
    Timepoint losttic{ Timepoint::Infinite() };

    std::string DebugInfo() const
    {
        std::stringstream ss;
        ss << "valid: " << valid << " "
           << "lossPackets:{";
        for (const auto& pkt: lossPackets)
        {
            ss << pkt;
        }

        ss << "} "
           << "losttic: " << losttic.ToDebuggingValue() << " ";
        return ss.str();
    }
};

struct AckEvent
{
    /** since we receive packets one by one, each packet carries only one data piece*/
    bool valid{ false };
    DataPacket ackPacket;
    Timepoint sendtic{ Timepoint::Infinite() };
    Timepoint losttic{ Timepoint::Infinite() };

    std::string DebugInfo() const
    {
        std::stringstream ss;
        ss << "valid: " << valid << " "
           << "ackpkt:{"
           << "seq: " << ackPacket.seq << " "
           << "dataid: " << ackPacket.pieceId << " "
           << "} "
           << "sendtic: " << sendtic.ToDebuggingValue() << " "
           << "losttic: " << losttic.ToDebuggingValue() << " ";
        return ss.str();
    }
};

/** This is a loss detection algorithm interface
 *  similar to the GeneralLossAlgorithm interface in Quiche project
 * */
class LossDetectionAlgo
{

public:
    /** @brief this function will be called when loss detection may happen, like timer alarmed or packet acked
     * input
     * @param downloadingmap all the packets inflight, sent but not acked or lost
     * @param eventtime timpoint that this function is called
     * @param ackEvent  ack event that trigger this function, if any
     * @param maxacked max sequence number that has acked
     * @param rttStats RTT statics module
     * output
     * @param losses loss event
     * */
    virtual void DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime,
            const AckEvent& ackEvent, uint64_t maxacked, LossEvent& losses, RttStats& rttStats)
    {
    };

    virtual ~LossDetectionAlgo() = default;

};

class DefaultLossDetectionAlgo : public LossDetectionAlgo
{/// Check loss event based on RTO
public:
    void DetectLoss(const InFlightPacketMap& downloadingmap, Timepoint eventtime, const AckEvent& ackEvent,
            uint64_t maxacked, LossEvent& losses, RttStats& rttStats) override
    {
        SPDLOG_TRACE("inflight: {} eventtime: {} ackEvent:{} ", downloadingmap.DebugInfo(),
                eventtime.ToDebuggingValue(), ackEvent.DebugInfo());
        /** RFC 9002 Section 6
         * */
        Duration maxrtt = std::max(rttStats.previous_srtt(), rttStats.latest_rtt());
        if (maxrtt == Duration::Zero())
        {
            SPDLOG_DEBUG(" {}", maxrtt == Duration::Zero());
            maxrtt = rttStats.SmoothedOrInitialRtt();
        }
        Duration loss_delay = maxrtt + (maxrtt * (5.0 / 4.0));
        loss_delay = std::max(loss_delay, Duration::FromMicroseconds(1));
        loss_delay = std::min(loss_delay, max_loss_delay);
        SPDLOG_TRACE(" maxrtt: {}, loss_delay: {}", maxrtt.ToDebuggingValue(), loss_delay.ToDebuggingValue());
        for (const auto& pkt_itor: downloadingmap.inflightPktMap)
        {
            const auto& pkt = pkt_itor.second;
            if (Timepoint(pkt.sendtic + loss_delay) <= eventtime)
            {
                losses.lossPackets.emplace_back(pkt);
            }
        }
        if (!losses.lossPackets.empty())
        {
            losses.losttic = eventtime;
            losses.valid = true;
            SPDLOG_DEBUG("losses: {}", losses.DebugInfo());
        }
    }

    ~DefaultLossDetectionAlgo() override
    {
    }

private:
    Duration max_loss_delay{ Duration::FromMilliseconds(500) };
};


class CongestionCtlAlgo
{
public:

    virtual ~CongestionCtlAlgo() = default;

    virtual CongestionCtlType GetCCtype() = 0;

    /////  Event
    virtual void OnDataSent(const InflightPacket& sentpkt) = 0;

    virtual void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) = 0;

    /////
    virtual uint32_t GetCWND() = 0;

//    virtual uint32_t GetFreeCWND() = 0;

};


/// config or setting for specific cc algo
/// used for pass parameters to CongestionCtlAlgo
struct CongestionCtlConfig
{
    uint32_t minCwnd{ 1 };
    uint32_t maxCwnd{ 64 };
    uint32_t ssThresh{ 32 };/** slow start threshold*/
};

class RenoCongestionContrl : public CongestionCtlAlgo
{
public:

    explicit RenoCongestionContrl(const CongestionCtlConfig& ccConfig, std::shared_ptr<CrossSessionData> sessionData, basefw::ID sessionID)
    {
        m_sessionID = sessionID;
        m_sessionData = sessionData;
        m_ssThresh = ccConfig.ssThresh;
        m_minCwnd = ccConfig.minCwnd;
        m_maxCwnd = ccConfig.maxCwnd;
        SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
    }

    ~RenoCongestionContrl() override
    {
        SPDLOG_DEBUG("");
    }

    CongestionCtlType GetCCtype() override
    {
        return CongestionCtlType::reno;
    }

    void OnDataSent(const InflightPacket& sentpkt) override
    {
        SPDLOG_TRACE("");
    }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    {
        SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
        if (lossEvent.valid)
        {
            OnDataLoss(lossEvent);
        }

        if (ackEvent.valid)
        {
            OnDataRecv(ackEvent, rttstats);
        }

    }

    /////
    uint32_t GetCWND() override
    {
        SPDLOG_TRACE(" {}", m_cwnd);
        return m_cwnd;
    }

//    virtual uint32_t GetFreeCWND() = 0;

private:

    bool InSlowStart()
    {
        bool rt = false;
        if (m_cwnd < m_ssThresh)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
        return rt;
    }

    bool LostCheckRecovery(Timepoint largestLostSentTic)
    {
        SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
                largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
        /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
         * (plus a 10ms correction), this session is in Recovery phase.
         * */
        if (lastLagestLossPktSentTic.IsInitialized() &&
            (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
        {
            SPDLOG_DEBUG("In Recovery");
            return true;
        }
        else
        {
            // a new timelost
            lastLagestLossPktSentTic = largestLostSentTic;
            SPDLOG_DEBUG("new loss");
            return false;
        }

    }

    void ExitSlowStart()
    {
        SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
        m_ssThresh = m_cwnd;
    }


    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    {
        // SPDLOG_INFO("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);
        SPDLOG_INFO("sessionID:{},eventtime:{},m_cwnd:{}", m_sessionID.ToLogStr().substr(36,4), Clock::GetClock()->Now().ToDebuggingValue(), m_cwnd);
        SPDLOG_INFO("rtt:{}", rttstats.latest_rtt().ToDebuggingValue());
        if (InSlowStart())
        {
            /// add 1 for each ack event
            m_cwnd += 1;

            if (m_cwnd >= m_ssThresh)
            {
                ExitSlowStart();
            }
            SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
        }
        else
        {
            /// add cwnd for each RTT
            m_cwndCnt++;
            if (m_cwndCnt >= m_cwnd)
            {
                m_cwndCnt = 0;
                m_cwnd ++;
            }
            SPDLOG_DEBUG("not in slow start state,new m_cwndCnt:{} new m_cwnd:{}",
                    m_cwndCnt, ackEvent.DebugInfo(), m_cwnd);

        }
        m_cwnd = BoundCwnd(m_cwnd);

        SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
    }

    void OnDataLoss(const LossEvent& lossEvent)
    {
        // SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
        SPDLOG_DEBUG("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(),m_cwnd);
        Timepoint maxsentTic{ Timepoint::Zero() };

        for (const auto& lostpkt: lossEvent.lossPackets)
        {
            maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
        }

        /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
         *  Otherwise, cwnd will cut half.
         * */
        if (InSlowStart())
        {
            // loss in slow start, just cut half
            m_cwnd = m_cwnd / 2;
            m_cwnd = BoundCwnd(m_cwnd);

        }
        else //if (!LostCheckRecovery(maxsentTic))
        {
            // Not In slow start and not inside Recovery state
            // Cut half
            m_cwnd = m_cwnd / 2;
            m_cwnd = BoundCwnd(m_cwnd);
            m_ssThresh = m_cwnd;
            // enter Recovery state
        }
        SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
    }


    uint32_t BoundCwnd(uint32_t trySetCwnd)
    {
        return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    }

    uint32_t m_cwnd{ 1 };
    uint32_t m_cwndCnt{ 0 }; /** in congestion avoid phase, used for counting ack packets*/
    Timepoint lastLagestLossPktSentTic{ Timepoint::Zero() };
    std::shared_ptr<CrossSessionData> m_sessionData;
    basefw::ID m_sessionID;

    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 64 };
    uint32_t m_ssThresh{ 32 };/** slow start threshold*/
};


class CubicCongestionContrl : public CongestionCtlAlgo
{
public:

    explicit CubicCongestionContrl(const CongestionCtlConfig& ccConfig, std::shared_ptr<CrossSessionData> sessionData, basefw::ID sessionID)
    {
        m_sessionID = sessionID;
        m_sessionData = sessionData;
        m_ssThresh = ccConfig.ssThresh;
        m_minCwnd = ccConfig.minCwnd;
        m_maxCwnd = ccConfig.maxCwnd;
        SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
    }

    ~CubicCongestionContrl() override
    {
        SPDLOG_DEBUG("");
    }

    CongestionCtlType GetCCtype() override
    {
        return CongestionCtlType::cubic;
    }

    void OnDataSent(const InflightPacket& sentpkt) override
    {
        SPDLOG_TRACE("");
    }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    {
        SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
        if (lossEvent.valid)
        {
            loss_1 = loss_2;
            loss_2 = 0;
            uint32_t loss = std::max(loss_1, loss_2);
            uint32_t rtt = rttstats.SmoothedOrInitialRtt().ToMilliseconds();
            m_sessionData->UpdateSelfData(m_sessionID, m_cwnd, rtt, loss);
            OnDataLoss(lossEvent);
        }

        if (ackEvent.valid)
        {
            loss_2 = loss_2 + 1;
            uint32_t loss = std::max(loss_1, loss_2);
            uint32_t rtt = rttstats.SmoothedOrInitialRtt().ToMilliseconds();
            m_sessionData->UpdateSelfData(m_sessionID, m_cwnd, rtt, loss);
            OnDataRecv(ackEvent, rttstats);
        }

    }

    /////
    uint32_t GetCWND() override
    {
        SPDLOG_TRACE(" {}", m_cwnd);
        return m_cwnd;
    }

private:

    bool LostCheckRecovery(Timepoint largestLostSentTic)
    {
        SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
                largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
        /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
         * (plus a 10ms correction), this session is in Recovery phase.
         * */
        if (lastLagestLossPktSentTic.IsInitialized() &&
            (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
        {
            SPDLOG_DEBUG("In Recovery");
            return true;
        }
        else
        {
            // a new timelost
            lastLagestLossPktSentTic = largestLostSentTic;
            SPDLOG_DEBUG("new loss");
            return false;
        }

    }
    
    bool InSlowStart()
    {
        bool rt = false;
        if (m_cwnd < m_ssThresh)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
        return rt;
    }

    void ExitSlowStart()
    {
        SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
        m_ssThresh = m_cwnd;
    }

    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    {
        SPDLOG_INFO("sessionID:{},eventtime:{},m_cwnd:{}", m_sessionID.ToLogStr().substr(36,4), Clock::GetClock()->Now().ToDebuggingValue(), m_cwnd);
        SPDLOG_INFO("rtt:{}", rttstats.latest_rtt().ToDebuggingValue());
        if (InSlowStart())
        {
            /// add 1 for each ack event
            m_cwnd += 1;

            if (m_cwnd >= m_ssThresh)
            {
                ExitSlowStart();
            }
            SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
        }
        else
        {
            /// add cwnd for each RTT
            uint32_t cnt = CubicUpdate(ackEvent, rttstats);
            // SPDLOG_INFO("cnt:{}", cnt);
            if ( m_cwnd_cnt > cnt )
            {
                m_cwnd ++;
                m_cwnd_cnt = 0;
            }
            else m_cwnd_cnt ++;
            SPDLOG_DEBUG("not in slow start state,new m_cwnd_cnt:{} new m_cwnd:{}",
                    m_cwnd_cnt, ackEvent.DebugInfo(), m_cwnd);

        }
        m_cwnd = BoundCwnd(m_cwnd);

        SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
    }

    void OnDataLoss(const LossEvent& lossEvent)
    {
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(),m_cwnd);
        Timepoint maxsentTic{ Timepoint::Zero() };

        epoch_start = Timepoint::Zero();
        if(InSlowStart())
        {
            m_cwnd = m_cwnd / 2;
            m_cwnd = BoundCwnd(m_cwnd);
            m_ssThresh = m_cwnd;
        }
        else if(!LostCheckRecovery(maxsentTic))
        {
            if (m_cwnd < m_last_max_cwnd && m_fast_convergence)
                m_last_max_cwnd = m_cwnd * (2-cubic_beta)/2;
            else
                m_last_max_cwnd = m_cwnd;

            m_cwnd = m_cwnd * (1-cubic_beta);
            m_cwnd = BoundCwnd(m_cwnd);
            m_ssThresh = m_cwnd;
        }
        else
        {
            // In Recovery state
            m_cwnd -= 1;
            m_cwnd = BoundCwnd(m_cwnd);
        }
        SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
    }

    uint32_t CubicUpdate(const AckEvent& ackEvent, RttStats& rttstats)
    {
        if (epoch_start <= Timepoint::Zero())
        {
            epoch_start = Timepoint( ackEvent.sendtic + rttstats.latest_rtt() );
            if (m_cwnd < m_last_max_cwnd)
            {
                bic_K = Duration::FromMilliseconds( uint32_t( pow((m_last_max_cwnd-m_cwnd)/cubic_C, 1.0/3)*1000) );
                origin_point = m_last_max_cwnd;
            }
            else
            {
                bic_K = Duration::FromMilliseconds( 0 );
                origin_point = m_cwnd;
            }
            m_ack_cnt = 1;
            m_tcp_cwnd = m_cwnd;
        }
        Duration time_offset = Duration::Zero(); // |t-K|
        Duration rec_time = ackEvent.sendtic + rttstats.latest_rtt() + rttstats.MinOrInitialRtt() - epoch_start;
        SPDLOG_DEBUG("bic_K:{},rec_time:{}", bic_K.ToMilliseconds(), rec_time.ToMilliseconds());
        if (rec_time < bic_K)		
            time_offset = bic_K - rec_time;
        else
            time_offset = rec_time - bic_K;
        
        uint32_t target;
        if (rec_time < bic_K)
            target = origin_point - cubic_C * time_offset.ToMilliseconds()/1000.0 * time_offset.ToMilliseconds()/1000.0 * time_offset.ToMilliseconds()/1000.0;
        else
            target = origin_point + cubic_C * time_offset.ToMilliseconds()/1000.0 * time_offset.ToMilliseconds()/1000.0 * time_offset.ToMilliseconds()/1000.0;
        SPDLOG_DEBUG("target:{},m_cwnd:{}", target, m_cwnd);
        uint32_t cnt;
        if (target > m_cwnd)
        {
            cnt = m_cwnd / ( target - m_cwnd + (m_olia ? GetAlpha() : 0));
        }
        else 
            cnt = 100 * m_cwnd;
        
        if (m_tcp_friendliness)
        {    
            m_tcp_cwnd += 3*cubic_beta/(2-cubic_beta) * m_ack_cnt/m_cwnd;
            m_ack_cnt = 0;
            if (m_tcp_cwnd > m_cwnd)
            {
                uint32_t max_cnt = m_cwnd / ( m_tcp_cwnd - m_cwnd );
                if (cnt > max_cnt) cnt = max_cnt; 
            }
        }

        return cnt;
    }

    void GetMaxCwndSession(std::shared_ptr<std::vector<basefw::ID>> sessionIDList)
    {
        uint32_t session_max_cwnd;
        for (auto&& id_cwnd = m_sessionData->SessionCwndMap.begin(); id_cwnd != m_sessionData->SessionCwndMap.end(); id_cwnd++)
        {
            if (id_cwnd->second > session_max_cwnd)
            {
                session_max_cwnd = id_cwnd->second;
                sessionIDList->clear();
                sessionIDList->push_back(id_cwnd->first);
            }
            else if (id_cwnd->second == session_max_cwnd)
            {
                sessionIDList->push_back(id_cwnd->first);
            }
        }
    }

    void GetBestSession(std::shared_ptr<std::vector<basefw::ID>> sessionIDList)
    {
        uint32_t max_rtt;
        uint32_t max_loss;
        for (auto&& id_rtt = m_sessionData->SessionRttMap.begin(); id_rtt != m_sessionData->SessionRttMap.end(); id_rtt++)
        {
            uint32_t tmp_loss = m_sessionData->SessionLossMap[id_rtt->first];
            uint32_t tmp_rtt = id_rtt->second;
            if ( tmp_loss * tmp_loss * max_rtt > max_loss * max_loss * tmp_rtt)
            {
                max_rtt = tmp_rtt;
                max_loss = tmp_loss;
                sessionIDList->clear();
                sessionIDList->push_back(id_rtt->first);
            }
            else if (tmp_loss * tmp_loss * max_rtt == max_loss * max_loss * tmp_rtt)
            {
                sessionIDList->push_back(id_rtt->first);
            }
        }
    }

    double GetAlpha()
    {
        double alpha;
        std::shared_ptr<std::vector<basefw::ID>> M_sessionIDList = std::make_shared<std::vector<basefw::ID>>();
        std::shared_ptr<std::vector<basefw::ID>> B_sessionIDList = std::make_shared<std::vector<basefw::ID>>();

        GetMaxCwndSession(M_sessionIDList);
        GetBestSession(B_sessionIDList);
        
        uint32_t session_all = m_sessionData->SessionCwndMap.size();
        //compute the number of sessionID is in best path but not in max cwnd path
        uint32_t session_collected = 0;
        for (auto&& id = B_sessionIDList->begin(); id != B_sessionIDList->end(); id++)
        {
            bool tmp_in_M = false;
            for (auto&& id2 = M_sessionIDList->begin(); id2 != M_sessionIDList->end(); id2++)
            {
                if (id == id2)
                {
                    tmp_in_M = true;
                    break;
                }
            }
            if (!tmp_in_M) session_collected ++;
        }
        
        // whether sessionID is in best path / max cwnd path
        bool in_B = false;
        bool in_M = false;
        for (uint32_t i=0;i<B_sessionIDList->size();i++)
        {
            if (B_sessionIDList->at(i) == m_sessionID) in_B = true;
        }
        for (uint32_t i=0;i<M_sessionIDList->size();i++)
        {
            if (M_sessionIDList->at(i) == m_sessionID) in_M = true;
        }
        
        //whether sessionID is in best path but not in max cwnd path
        if (in_B && !in_M) 
        {
            alpha = 1.0 / session_all /session_collected;
        }
        //whether sessionID is in max cwnd path and collected path != 0
        else if (in_M && session_collected)
        {
            alpha = - 1.0 / session_all / M_sessionIDList->size();
        }
        else
        {
            alpha = 0;
        }
        return alpha;
    }


    uint32_t BoundCwnd(uint32_t trySetCwnd)
    {
        return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    }

    uint32_t m_cwnd{ 1 };
    uint32_t m_tcp_cwnd{ 1 };
    uint32_t m_ack_cnt{ 0 }; /* used in tcp friendliness*/
    uint32_t m_cwnd_cnt{ 0 }; /* in congestion avoid phase, used for counting ack packets*/
    uint32_t m_last_max_cwnd{ 0 };
    uint32_t origin_point{ 1 };
    uint32_t loss_1{ 0 };
    uint32_t loss_2{ 0 };
    Timepoint lastLagestLossPktSentTic{ Timepoint::Zero() };
    std::shared_ptr<CrossSessionData> m_sessionData;
    basefw::ID m_sessionID;

    Timepoint epoch_start{ Timepoint::Zero() };
    Duration bic_K{ Duration::Zero() };
    double cubic_C{ 0.4 };
    double cubic_beta{ 0.2 };
    
    bool m_fast_convergence{ 0 };
    bool m_tcp_friendliness{ 0 };
    bool m_olia{ 1 };
    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 64 };
    uint32_t m_ssThresh{ 32 };/** slow start threshold*/
};


class OliaCongestionContrl : public CongestionCtlAlgo
{
public:

    explicit OliaCongestionContrl(const CongestionCtlConfig& ccConfig, std::shared_ptr<CrossSessionData> sessionData, basefw::ID sessionID)
    {
        m_sessionData = sessionData;
        m_sessionID = sessionID;
        m_ssThresh = ccConfig.ssThresh;
        m_minCwnd = ccConfig.minCwnd;
        m_maxCwnd = ccConfig.maxCwnd;
        SPDLOG_DEBUG("m_ssThresh:{}, m_minCwnd:{}, m_maxCwnd:{} ", m_ssThresh, m_minCwnd, m_maxCwnd);
    }

    ~OliaCongestionContrl() override
    {
        SPDLOG_DEBUG("");
    }

    CongestionCtlType GetCCtype() override
    {
        return CongestionCtlType::olia;
    }

    void OnDataSent(const InflightPacket& sentpkt) override
    {
        SPDLOG_TRACE("");
    }

    void OnDataAckOrLoss(const AckEvent& ackEvent, const LossEvent& lossEvent, RttStats& rttstats) override
    {
        SPDLOG_TRACE("ackevent:{}, lossevent:{}", ackEvent.DebugInfo(), lossEvent.DebugInfo());
        if (lossEvent.valid)
        {
            loss_1 = loss_2;
            loss_2 = 0;
            uint32_t loss = std::max(loss_1, loss_2);
            uint32_t rtt = rttstats.SmoothedOrInitialRtt().ToMilliseconds();
            m_sessionData->UpdateSelfData(m_sessionID, m_cwnd, rtt, loss);
            OnDataLoss(lossEvent);
        }

        if (ackEvent.valid)
        {
            loss_2 = loss_2 + 1;
            uint32_t loss = std::max(loss_1, loss_2);
            uint32_t rtt = rttstats.SmoothedOrInitialRtt().ToMilliseconds();
            m_sessionData->UpdateSelfData(m_sessionID, m_cwnd, rtt, loss);
            OnDataRecv(ackEvent, rttstats);
        }
    }

    /////
    uint32_t GetCWND() override
    {
        SPDLOG_TRACE(" {}", m_cwnd);
        return m_cwnd;
    }

//    virtual uint32_t GetFreeCWND() = 0;

private:

    bool InSlowStart()
    {
        bool rt = false;
        if (m_cwnd < m_ssThresh)
        {
            rt = true;
        }
        else
        {
            rt = false;
        }
        SPDLOG_TRACE(" m_cwnd:{}, m_ssThresh:{}, InSlowStart:{}", m_cwnd, m_ssThresh, rt);
        return rt;
    }

    bool LostCheckRecovery(Timepoint largestLostSentTic)
    {
        SPDLOG_DEBUG("largestLostSentTic:{},lastLagestLossPktSentTic:{}",
                largestLostSentTic.ToDebuggingValue(), lastLagestLossPktSentTic.ToDebuggingValue());
        /** If the largest sent tic of this loss event,is bigger than the last sent tic of the last lost pkt
         * (plus a 10ms correction), this session is in Recovery phase.
         * */
        if (lastLagestLossPktSentTic.IsInitialized() &&
            (largestLostSentTic + Duration::FromMilliseconds(10) > lastLagestLossPktSentTic))
        {
            SPDLOG_DEBUG("In Recovery");
            return true;
        }
        else
        {
            // a new timelost
            lastLagestLossPktSentTic = largestLostSentTic;
            SPDLOG_DEBUG("new loss");
            return false;
        }

    }

    void ExitSlowStart()
    {
        SPDLOG_DEBUG("m_ssThresh:{}, m_cwnd:{}", m_ssThresh, m_cwnd);
        m_ssThresh = m_cwnd;
    }


    void OnDataRecv(const AckEvent& ackEvent, RttStats& rttstats)
    {
        // SPDLOG_INFO("ackevent:{},m_cwnd:{}", ackEvent.DebugInfo(), m_cwnd);
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(), m_cwnd);
        SPDLOG_INFO("rtt:{}", rttstats.latest_rtt().ToDebuggingValue());
        if (InSlowStart())
        {
            /// add 1 for each ack event
            m_cwnd += 1;

            if (m_cwnd >= m_ssThresh)
            {
                ExitSlowStart();
            }
            SPDLOG_DEBUG("new m_cwnd:{}", m_cwnd);
        }
        else
        {
            /// add cwnd for each RTT
            double cnt = OliaUpdate();
            SPDLOG_INFO("cnt:{}", cnt);
            if (cnt >= 0)
            {
                if ( m_cwnd_cnt > cnt )
                {
                    m_cwnd ++;
                    m_cwnd_cnt = 0;
                }
                else m_cwnd_cnt ++;
            }
            else
            {
                if ( m_cwnd_cnt < cnt )
                {
                    m_cwnd --;
                    m_cwnd_cnt = 0;
                }
                else m_cwnd_cnt --;
            }
            
            SPDLOG_DEBUG("not in slow start state,new m_cwnd_cnt:{} new m_cwnd:{}",
                    m_cwnd_cnt, ackEvent.DebugInfo(), m_cwnd);

        }
        m_cwnd = BoundCwnd(m_cwnd);

        SPDLOG_DEBUG("after RX, m_cwnd={}", m_cwnd);
    }

    void OnDataLoss(const LossEvent& lossEvent)
    {
        // SPDLOG_DEBUG("lossevent:{}", lossEvent.DebugInfo());
        SPDLOG_INFO("eventtime:{},m_cwnd:{}", Clock::GetClock()->Now().ToDebuggingValue(),m_cwnd);
        Timepoint maxsentTic{ Timepoint::Zero() };

        for (const auto& lostpkt: lossEvent.lossPackets)
        {
            maxsentTic = std::max(maxsentTic, lostpkt.sendtic);
        }

        /** In Recovery phase, cwnd will decrease 1 pkt for each lost pkt
         *  Otherwise, cwnd will cut half.
         * */
        if (InSlowStart())
        {
            // loss in slow start, just cut half
            m_cwnd = m_cwnd / 2;
            m_cwnd = BoundCwnd(m_cwnd);

        }
        else if (!LostCheckRecovery(maxsentTic))
        {
            // Not In slow start and not inside Recovery state
            // Cut half
            m_cwnd = m_cwnd * 0.7;
            m_cwnd = BoundCwnd(m_cwnd);
            m_ssThresh = m_cwnd;
            // enter Recovery state
        }
        else
        {
            // In Recovery state
            m_cwnd -= 1;
            m_cwnd = BoundCwnd(m_cwnd);
        }
        SPDLOG_DEBUG("after Loss, m_cwnd={}", m_cwnd);
    }

    void GetMaxCwndSession(std::shared_ptr<std::vector<basefw::ID>> sessionIDList)
    {
        uint32_t session_max_cwnd;
        for (auto&& id_cwnd = m_sessionData->SessionCwndMap.begin(); id_cwnd != m_sessionData->SessionCwndMap.end(); id_cwnd++)
        {
            if (id_cwnd->second > session_max_cwnd)
            {
                session_max_cwnd = id_cwnd->second;
                sessionIDList->clear();
                sessionIDList->push_back(id_cwnd->first);
            }
            else if (id_cwnd->second == session_max_cwnd)
            {
                sessionIDList->push_back(id_cwnd->first);
            }
        }
    }

    void GetBestSession(std::shared_ptr<std::vector<basefw::ID>> sessionIDList)
    {
        uint32_t max_rtt;
        uint32_t max_loss;
        for (auto&& id_rtt = m_sessionData->SessionRttMap.begin(); id_rtt != m_sessionData->SessionRttMap.end(); id_rtt++)
        {
            uint32_t tmp_loss = m_sessionData->SessionLossMap[id_rtt->first];
            uint32_t tmp_rtt = id_rtt->second;
            if ( tmp_loss * tmp_loss * max_rtt > max_loss * max_loss * tmp_rtt)
            {
                max_rtt = tmp_rtt;
                max_loss = tmp_loss;
                sessionIDList->clear();
                sessionIDList->push_back(id_rtt->first);
            }
            else if (tmp_loss * tmp_loss * max_rtt == max_loss * max_loss * tmp_rtt)
            {
                sessionIDList->push_back(id_rtt->first);
            }
        }
    }

    double GetAlpha()
    {
        double alpha;
        std::shared_ptr<std::vector<basefw::ID>> M_sessionIDList = std::make_shared<std::vector<basefw::ID>>();
        std::shared_ptr<std::vector<basefw::ID>> B_sessionIDList = std::make_shared<std::vector<basefw::ID>>();

        GetMaxCwndSession(M_sessionIDList);
        GetBestSession(B_sessionIDList);
        
        uint32_t session_all = m_sessionData->SessionCwndMap.size();
        //compute the number of sessionID is in best path but not in max cwnd path
        uint32_t session_collected = 0;
        for (auto&& id = B_sessionIDList->begin(); id != B_sessionIDList->end(); id++)
        {
            bool tmp_in_M = false;
            for (auto&& id2 = M_sessionIDList->begin(); id2 != M_sessionIDList->end(); id2++)
            {
                if (id == id2)
                {
                    tmp_in_M = true;
                    break;
                }
            }
            if (!tmp_in_M) session_collected ++;
        }
        
        // whether sessionID is in best path / max cwnd path
        bool in_B = false;
        bool in_M = false;
        for (uint32_t i=0;i<B_sessionIDList->size();i++)
        {
            if (B_sessionIDList->at(i) == m_sessionID) in_B = true;
        }
        for (uint32_t i=0;i<M_sessionIDList->size();i++)
        {
            if (M_sessionIDList->at(i) == m_sessionID) in_M = true;
        }
        
        //whether sessionID is in best path but not in max cwnd path
        if (in_B && !in_M) 
        {
            alpha = 1.0 / session_all /session_collected;
        }
        //whether sessionID is in max cwnd path and collected path != 0
        else if (in_M && session_collected)
        {
            alpha = - 1.0 / session_all / M_sessionIDList->size();
        }
        else
        {
            alpha = 0;
        }
        return alpha;
    }

    int OliaUpdate(){
        double sum = 0;
        for (auto&& id_cwnd = m_sessionData->SessionCwndMap.begin(); id_cwnd != m_sessionData->SessionCwndMap.end(); id_cwnd++)
        {
            basefw::ID sessionID = id_cwnd->first;
            uint32_t session_cwnd = id_cwnd->second;
            uint32_t session_rtt = m_sessionData->SessionRttMap[sessionID];
            // SPDLOG_INFO("session_cwnd={}, session_rtt={}", session_cwnd, session_rtt);
            sum += session_cwnd*1.0 / session_rtt;
        }
        uint32_t rtt_self = m_sessionData->SessionRttMap[m_sessionID];
        double temp = m_cwnd / (rtt_self * rtt_self * sum * sum) + GetAlpha() / m_cwnd;
        SPDLOG_INFO("alpha={}, temp={}, rttself:{}, sum:{}", GetAlpha(), temp, rtt_self, sum);
        return int(1/temp);
    }

    uint32_t BoundCwnd(uint32_t trySetCwnd)
    {
        return std::max(m_minCwnd, std::min(trySetCwnd, m_maxCwnd));
    }

    uint32_t m_cwnd{ 1 };
    int m_cwnd_cnt{ 0 };
    uint32_t loss_1{ 0 };
    uint32_t loss_2{ 0 };
    Timepoint lastLagestLossPktSentTic{ Timepoint::Zero() };
    std::shared_ptr<CrossSessionData> m_sessionData;
    basefw::ID m_sessionID;

    uint32_t m_minCwnd{ 1 };
    uint32_t m_maxCwnd{ 64 };
    uint32_t m_ssThresh{ 32 };/** slow start threshold*/
};
