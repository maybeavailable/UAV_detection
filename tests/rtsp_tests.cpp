#include"rtsp/rtspserver.h"
#include"rtsp/rtp_source.h"
#include"rtsp/rtp_transport.h"
#include"rtsp/file_source.h"
#include"rtsp/h264_access_unit.h"
#include"common/rtp_packet.h"
#include"log/log.h"
#include<cmath>
#include<iterator>
#include<limits>
#include<stdexcept>

using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (0)

class TestSource : public RtpSource
{
public:
    std::string path() const override { return "/test"; }
    std::string makeSdp(const std::string&, int) const override { return {}; }
};

void put32(Bytes& b, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) b.at(at + i) = value >> (24 - 8 * i);
}
std::uint32_t get32(const Bytes& b, std::size_t at)
{
    return (std::uint32_t(b.at(at)) << 24) | (std::uint32_t(b.at(at+1)) << 16) |
           (std::uint32_t(b.at(at+2)) << 8) | b.at(at+3);
}
Bytes report(bool sender, unsigned count = 1)
{
    Bytes b((sender ? 28 : 8) + 24 * count, 0);
    b[0] = 0x80 | count; b[1] = sender ? RTCP_SR : RTCP_RR;
    b[3] = b.size() / 4 - 1;
    put32(b, 4, 0x11223344);
    if (count)
    {
        const auto offset = sender ? 28 : 8;
        put32(b, offset, 0xaabbccdd);
        b[offset + 4] = 3; b[offset + 5] = 0xff; b[offset + 6] = 0xff; b[offset + 7] = 0xfe;
        put32(b, offset + 8, 99); put32(b, offset + 12, 90);
    }
    return b;
}

void rtcpParsing()
{
    boost::asio::io_context io;
    RtpTransport::Config config; config.mode = RtpTransport::Mode::TCP; config.ssrc = 0xaabbccdd;
    auto t = std::make_shared<RtpTransport>(io, config, nullptr);
    unsigned callbacks = 0;
    t->setFeedbackCallback([&](const RtcpFeedback& f) { CHECK(f.ssrc == config.ssrc); ++callbacks; });
    for (bool sender : {false, true})
    {
        auto b = report(sender);
        RtcpPacketParser p(b.data(), b.size()); CHECK(p.next());
        const auto blocks = p.reportBlocks(); CHECK(blocks.size() == 1);
        CHECK(blocks[0].ssrc == config.ssrc && blocks[0].highestSeq == 99);
        CHECK(blocks[0].cumulativeLost == -2 && blocks[0].jitter == 90);
        t->handleRtcp(b.data(), b.size());
        b[0] = 0x80; // RC=0，尾部不能误认作报告块
        RtcpPacketParser empty(b.data(), b.size()); CHECK(empty.next()); CHECK(empty.reportBlocks().empty());
        b[0] = 0x82; // 声明两个块但只有一个
        RtcpPacketParser shortReport(b.data(), b.size()); CHECK(shortReport.next()); CHECK(shortReport.reportBlocks().empty());
    }
    CHECK(callbacks == 2);
    auto padded = report(false); padded.resize(36); padded[0] |= 0x20; padded[3] = 8; padded.back() = 4;
    RtcpPacketParser padding(padded.data(), padded.size()); CHECK(padding.next()); CHECK(padding.reportBlocks().size() == 1);
    CHECK(!RtcpPacketParser(padded.data(), padded.size()-1).next());
    CHECK(rtcpNtpNow(std::chrono::system_clock::time_point(500ms)) == (2208988800ULL << 32 | 0x80000000ULL));
    CHECK(std::abs(rtcpRttMs(10000, 655, 11966) - 20.0) < .05);
    CHECK(std::abs(rtcpRttMs(0xfffffff0u, 16, 655) - 10.0) < .05);
    RtcpFeedback feedback; CHECK(t->lastFeedback(feedback)); CHECK(feedback.highestSeq == 99);
}

std::shared_ptr<RtpTransport> collector(boost::asio::io_context& io, std::vector<Bytes>& packets)
{
    RtpTransport::Config c; c.mode = RtpTransport::Mode::TCP;
    auto t = std::make_shared<RtpTransport>(io, c, [&](int channel, const auto& packet) {
        if (channel == 0) packets.push_back(*packet);
    });
    t->setPlaying(true);
    return t;
}
void pump(boost::asio::io_context& io) { io.restart(); io.poll(); }

void packetization()
{
    boost::asio::io_context io;
    TestSource source;
    std::vector<Bytes> packets;
    auto t = collector(io, packets);
    const auto firstSeq = source.addSink(t);
    Bytes frame = {0,0,0,1,0x65,0x88,0xaa};
    for (const auto pts : {0ULL, 40000ULL, 80000ULL}) source.pushFrame(frame.data(), frame.size(), pts);
    pump(io); CHECK(packets.size() == 3);
    CHECK(RtpPacketParser(packets[0].data(), packets[0].size()).sequenceNumber() == firstSeq);
    for (unsigned i = 1; i < 3; ++i) CHECK(get32(packets[i],4) - get32(packets[i-1],4) == 3600);
    source.pushFrame(frame.data(), frame.size()); pump(io); CHECK(get32(packets.back(),4) - get32(packets[2],4) == 3600);
    source.pushFrame(frame.data(), frame.size(), 0); pump(io); CHECK(get32(packets.back(),4) - get32(packets[3],4) == 3600);
    source.pushFrame(frame.data(), frame.size(), 40000); pump(io); CHECK(get32(packets.back(),4) - get32(packets[4],4) == 3600);

    {
        TestSource largePts;
        std::vector<Bytes> largePackets; auto largeSink=collector(io,largePackets);
        largePts.addSink(largeSink);
        const std::uint64_t pts=(1ULL<<63)-20000;
        largePts.pushFrame(frame.data(),frame.size(),pts);
        largePts.pushFrame(frame.data(),frame.size(),pts+40000); pump(io);
        CHECK(largePackets.size()==2 && get32(largePackets[1],4)-get32(largePackets[0],4)==3600);
        largeSink->close(); pump(io);
    }

    Bytes big(100005, 0xaa); big[0]=0; big[1]=0; big[2]=0; big[3]=1; big[4]=0x65;
    packets.clear(); source.pushFrame(big.data(), big.size(), 120000); pump(io);
    Bytes restored{0x65};
    unsigned markers=0; std::uint32_t timestamp = get32(packets.front(),4);
    for (std::size_t i=0; i<packets.size(); ++i)
    {
        RtpPacketParser p(packets[i].data(), packets[i].size()); CHECK(p.valid()); CHECK(p.timestamp()==timestamp);
        CHECK(p.sequenceNumber() == std::uint16_t(RtpPacketParser(packets[0].data(),packets[0].size()).sequenceNumber()+i));
        CHECK((p.payload()[0]&31)==28); CHECK(bool(p.payload()[1]&0x80)==(i==0));
        CHECK(bool(p.payload()[1]&0x40)==(i+1==packets.size()));
        markers += p.marker(); restored.insert(restored.end(), p.payload()+2, p.payload()+p.payloadSize());
    }
    CHECK(markers==1 && restored == Bytes(big.begin()+4,big.end()));
    t->close(); pump(io);
}

void boundedDelivery()
{
    boost::asio::io_context io;
    RtpTransport::Config c; c.mode=RtpTransport::Mode::TCP; c.maxPendingBytes=100; c.maxPendingPackets=2;
    unsigned failures=0, writes=0;
    auto t=std::make_shared<RtpTransport>(io,c,[&](int,const auto&) { ++writes; });
    t->setFailureCallback([&] { ++failures; }); t->setPlaying(true);
    auto packet=std::make_shared<Bytes>(40,0); (*packet)[0]=0x80;
    for (unsigned i=0;i<100000;++i) { t->deliverRtp(packet); CHECK(t->pendingBytes()<=100); }
    pump(io); CHECK(!t->isOpen() && failures==1 && writes==0);
    std::vector<Bytes> packets; t=collector(io,packets); t->deliverRtp(packet); t->setPlaying(false);
    pump(io); CHECK(packets.empty() && t->pendingBytes()==0); t->close(); pump(io);
}

void senderReport()
{
    boost::asio::io_context io;
    RtpTransport::Config c; c.mode=RtpTransport::Mode::TCP; c.ssrc=0xaabbccdd; c.rtcpInterval=20ms;
    std::vector<Bytes> reports;
    auto t=std::make_shared<RtpTransport>(io,c,[&](int channel,const auto& p) { if(channel==1) reports.push_back(*p); });
    t->setPlaying(true);
    auto p=std::make_shared<Bytes>(112,0xaa); (*p)[0]=0x80; (*p)[1]=96; put32(*p,4,90000); put32(*p,8,c.ssrc);
    t->deliverRtp(p); io.run_for(45ms);
    CHECK(!reports.empty()); const auto& sr=reports[0];
    CHECK(get32(sr,4)==c.ssrc && get32(sr,20)==1 && get32(sr,24)==100);
    CHECK(get32(sr,12)!=0 && get32(sr,16)-90000 >= 900 && get32(sr,16)-90000 < 10000);
    RtcpPacketParser parser(sr.data(),sr.size()); CHECK(parser.next() && parser.type()==RTCP_SR);
    CHECK(parser.next() && parser.type()==RTCP_SDES); CHECK(parser.reportCount()==1);
    CHECK(get32(sr,32)==c.ssrc && sr[36]==1 && sr[37]>0 && sr[38+sr[37]]==0);
    CHECK(!parser.next()); t->close(); pump(io);
}

void lifecycle()
{
    using boost::asio::ip::udp; using boost::asio::ip::tcp;
    for (int i=0;i<20;++i)
    {
        boost::asio::io_context io;
        udp::socket peer(io,{udp::v4(),0});
        RtpTransport::Config c; c.serverRtp=0; c.serverRtcp=0; c.clientAddress="127.0.0.1";
        c.clientRtp=peer.local_endpoint().port(); c.clientRtcp=c.clientRtp;
        auto t=std::make_shared<RtpTransport>(io,c,nullptr); CHECK(t->isOpen()); t->start(); t->start();
        const udp::endpoint destination(boost::asio::ip::make_address("127.0.0.1"),t->config().serverRtcp);
        const auto rr=report(false);
        peer.send_to(boost::asio::buffer(rr),destination); peer.send_to(boost::asio::buffer(rr),destination);
        CHECK(io.run_one()==1); std::weak_ptr<RtpTransport> weak=t; t.reset(); CHECK(weak.expired());
        io.restart(); io.run_for(2ms);
        auto server=std::make_unique<RtspServer>(io,0); server->start(); server->start();
        tcp::socket a(io), b(io); const tcp::endpoint address(boost::asio::ip::make_address("127.0.0.1"),server->port());
        a.connect(address); b.connect(address); io.restart(); CHECK(io.run_one()==1); server.reset();
        io.restart(); io.run_for(2ms); // 成功但尚未分发的 accept/receive 回调必须安全
    }
}

void fileSource(const std::string& file)
{
    std::ifstream in(file,std::ios::binary); Bytes bytes((std::istreambuf_iterator<char>(in)),{});
    const auto units=parseH264AccessUnits(bytes.data(),bytes.size()); CHECK(units.size()==2);
    for (const auto& u : units)
    {
        unsigned slices=0;
        for (const auto& nal:splitAnnexB(u.data.data(),u.data.size()))
            slices += ((nal.data[0]&31)==1 || (nal.data[0]&31)==5);
        CHECK(slices==4);
    }
    for (double fps : {25.,29.97,.5})
    {
        boost::asio::io_context io;
        std::vector<Bytes> packets; auto t=collector(io,packets);
        const auto start=std::chrono::steady_clock::now();
        {
            FileRtpSource source("/file",file,fps);
            CHECK(source.makeSdp("127.0.0.1",0).find("a=fmtp:96 packetization-mode=1")!=std::string::npos);
            source.addSink(t);
            auto guard=boost::asio::make_work_guard(io);
            io.run_for(fps==.5?4100ms:150ms);
        }
        CHECK(std::chrono::steady_clock::now()-start < (fps==.5?4600ms:600ms));
        std::vector<std::uint32_t> timestamps;
        unsigned slices=0;
        for (const auto& p:packets)
        {
            const RtpPacketParser r(p.data(),p.size()); const auto type=r.payload()[0]&31;
            if (type==1 || type==5) ++slices;
            if (r.marker()) { CHECK(slices==4); slices=0; timestamps.push_back(r.timestamp()); }
        }
        CHECK(timestamps.size()>=2);
        for(std::size_t i=1;i<timestamps.size();++i)
            CHECK(std::abs(double(std::uint32_t(timestamps[i]-timestamps[i-1]))-90000./fps)<2);
        t->close(); pump(io);
    }
    for(double fps : {0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    {
        bool threw=false; try { FileRtpSource source("/file",file,fps); } catch(const std::invalid_argument&) { threw=true; }
        CHECK(threw);
    }
    bool threw=false; try { parseH264AccessUnits(bytes.data(),6); } catch(const std::invalid_argument&) { threw=true; }
    CHECK(threw);
}

void parserVariant(const std::string& path)
{
    std::ifstream in(path,std::ios::binary); Bytes bytes((std::istreambuf_iterator<char>(in)),{});
    const auto units=parseH264AccessUnits(bytes.data(),bytes.size()); CHECK(units.size()==8);
    auto slices=[](const Bytes& data) {
        std::vector<Bytes> out;
        for (const auto& nal:splitAnnexB(data.data(),data.size()))
            if ((nal.data[0]&31)==1 || (nal.data[0]&31)==5) out.emplace_back(nal.data,nal.data+nal.size);
        return out;
    };
    Bytes regrouped;
    for(const auto& unit:units) regrouped.insert(regrouped.end(),unit.data.begin(),unit.data.end());
    CHECK(slices(bytes)==slices(regrouped));
    std::ofstream output(path+".grouped.h264",std::ios::binary);
    output.write(reinterpret_cast<const char*>(regrouped.data()),regrouped.size());
}

int main(int argc,char** argv)
{
    try
    {
        CHECK(argc>=2);
        rtcpParsing(); packetization(); boundedDelivery(); senderReport(); lifecycle(); fileSource(argv[1]);
        for(int i=2;i<argc;++i) parserVariant(argv[i]);
        LogManager::getInstance().stop();
        std::cout << "PASS: RTCP parsing, PTS/FU-A, queue bounds, SR/SDES, shutdown lifecycle, multi-slice/fractional fps" << std::endl;
        return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
