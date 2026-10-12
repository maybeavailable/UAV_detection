#include"rtsp/rtspserver.h"
#include"rtsp/rtp_source.h"
#include"rtsp/file_source.h"
#include"log/log.h"
#include<csignal>

class LiveSource : public RtpSource
{
    std::string name;
public:
    explicit LiveSource(std::string path):name(std::move(path)) {}
    std::string path() const override { return name; }
    std::string makeSdp(const std::string& host,int) const override
    {
        return "v=0\r\no=- 0 1 IN IP4 "+host+"\r\ns=test\r\nt=0 0\r\na=control:*\r\n"
               "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\na=control:track1\r\n";
    }
};

int main(int argc,char** argv)
{
    if(argc!=2) return 2;
    boost::asio::io_context io;
    RtspServer server(io,0);
    auto a=std::make_shared<LiveSource>("/live/a"), b=std::make_shared<LiveSource>("/live/b");
    auto file=std::make_shared<FileRtpSource>("/file",argv[1],25);
    server.registerSource(a); server.registerSource(b); server.registerSource(file);
    server.setFeedbackCallback([](const RtcpFeedback& f) { std::cout << "feedback " << f.ssrc << " " << f.highestSeq << std::endl; });
    std::atomic<bool> stop{false};
    boost::asio::signal_set signals(io,SIGINT,SIGTERM);
    signals.async_wait([&](auto,auto) { server.stop(); stop.store(true); io.stop(); });
    std::thread producer([&] {
        std::vector<std::uint8_t> data(100005,0xaa); data[0]=0; data[1]=0; data[2]=0; data[3]=1; data[4]=0x65;
        std::uint64_t index=0;
        while(!stop.load())
        {
            a->pushFrame(data.data(),data.size(),index*20000);
            b->pushFrame(data.data(),data.size(),index*20000);
            ++index;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    std::cout << "ready " << server.port() << " " << a->ssrc() << " " << b->ssrc() << std::endl;
    io.run(); stop.store(true); producer.join(); server.stop(); file.reset();
    io.restart(); io.poll(); LogManager::getInstance().stop();
}
