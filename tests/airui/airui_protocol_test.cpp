#include"camera/airui_protocol.h"
#include"camera/thermal_camera.h"
#include"log/log.h"

#include<fcntl.h>
#include<poll.h>
#include<sys/ioctl.h>
#include<termios.h>
#include<unistd.h>
#include<algorithm>
#include<atomic>
#include<chrono>
#include<cmath>
#include<cstdlib>
#include<fstream>
#include<future>
#include<limits>
#include<mutex>
#include<numeric>
#include<sstream>
#include<stdexcept>
#include<thread>

using airui::Bytes;
using airui::Command;
using airui::Operation;
using namespace std::chrono_literals;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(#x)+" at line "+std::to_string(__LINE__)); } while(0)

Bytes hex(const std::string& text)
{
    Bytes out; std::istringstream input(text); unsigned value;
    while(input>>std::hex>>value) { CHECK(value<=255); out.push_back(value); }
    return out;
}
Bytes wire(std::uint8_t head,Bytes body)
{
    Bytes out{head,static_cast<std::uint8_t>(body.size()+1)};
    out.insert(out.end(),body.begin(),body.end());
    out.push_back(static_cast<std::uint8_t>(std::accumulate(out.begin(),out.end(),0u)));
    out.insert(out.end(),{0xEB,0xAA}); return out;
}
Bytes response(const airui::Frame& command,const Bytes& value)
{
    Bytes body;
    if(command.commandSet==airui::CommandSet::Lens || command.commandSet==airui::CommandSet::Temperature)
        body.push_back(static_cast<std::uint8_t>(*command.commandSet));
    body.insert(body.end(),{command.command,0x33}); body.insert(body.end(),value.begin(),value.end());
    return wire(0x55,std::move(body));
}
Bytes alarm(bool fire)
{
    Bytes body{1,8,1,static_cast<std::uint8_t>(fire?3:1)};
    body.resize(24,0);
    if(fire) { body[4]=10; body[6]=20; body[8]=0x80; body[9]=2; body[10]=0xFF; body[11]=1; }
    else body[4]=1;
    return wire(0xAA,std::move(body));
}

void codecTests(const std::string& path)
{
    CHECK(airui::encodeCommand(Command::SensorWidth,Operation::Read)==hex("AA 04 01 72 00 21 EB AA"));
    CHECK(airui::encodeCommand(Command::ZoomPosition,Operation::Read,{0})==hex("AA 05 08 33 00 00 EA EB AA"));
    CHECK(airui::encodeCommand(Command::FireThreshold,Operation::Write,{0x10,0x27})==hex("AA 06 07 31 01 10 27 20 EB AA"));
    CHECK(airui::encodeCommand(Command::StretchLow,Operation::Read)==hex("AA 04 07 1D 00 D2 EB AA"));
    CHECK(airui::littleEndian16(0xAAEB)==hex("EB AA"));
    CHECK(airui::littleEndian32(0x80000001)==hex("01 00 00 80"));
    CHECK(airui::readLittleEndian32(hex("78 56 34 12").data())==0x12345678);
    airui::Frame frame;
    CHECK(!airui::decodeFrame(nullptr,0,frame));
    const auto ambiguous=hex("55 06 08 33 33 01 00 CA EB AA");
    CHECK(airui::decodeFrame(ambiguous.data(),ambiguous.size(),frame));
    CHECK(frame.commandSet==airui::CommandSet::Lens && frame.command==0x33 && frame.parameters==hex("01 00"));
    const auto common=hex("55 07 08 33 00 8B 06 58 80 EB AA");
    CHECK(airui::decodeFrame(common.data(),common.size(),frame));
    CHECK(!frame.commandSet && frame.command==8 && frame.parameters.size()==4);
    for(const auto& bad : {"AA 06 07 31 01 10 27 E8 EB AA", "55 08 07 1D 33 01 B2 EB AA", "AA 05 07 1D 00 D2 EB AA"})
    {
        const auto data=hex(bad); CHECK(!airui::decodeFrame(data.data(),data.size(),frame));
    }
    for(const auto& args : std::vector<Bytes>{{},{0,1},{0,1,2}})
    {
        bool threw=false;
        try { airui::encodeCommand(Command::LensType,Operation::Read,args); }
        catch(const std::invalid_argument&) { threw=true; }
        CHECK(threw);
    }

    std::ifstream input(path); CHECK(input.good());
    std::string line; unsigned count=0, commandCount=0, notificationCount=0;
    while(std::getline(input,line))
    {
        if(line.empty() || line[0]=='#') continue;
        std::istringstream record(line); unsigned page; record>>page;
        std::string raw; std::getline(record,raw); const auto bytes=hex(raw);
        CHECK(airui::decodeFrame(bytes.data(),bytes.size(),frame)); CHECK(frame.wire==bytes);
        airui::FrameParser parser; std::vector<airui::Frame> parsed;
        for(const auto value:bytes)
        {
            const auto more=parser.feed(&value,1); parsed.insert(parsed.end(),more.begin(),more.end());
        }
        CHECK(parsed.size()==1 && parsed[0].wire==bytes);
        if(!frame.response)
        {
            airui::AlarmReport report;
            if(airui::decodeAlarmReport(frame,report)) ++notificationCount;
            else
            {
                const auto code=static_cast<Command>((static_cast<unsigned>(*frame.commandSet)<<8)|frame.command);
                CHECK(airui::encodeCommand(code,static_cast<Operation>(frame.operation),frame.parameters)==bytes);
                ++commandCount;
            }
        }
        ++count;
    }
    CHECK(count==418 && commandCount==257 && notificationCount==2);

    // 校验错误、噪音、粘包、报文内部 EB AA：只能按长度切帧。
    auto corrupt=common; corrupt[corrupt.size()-3]^=1;
    Bytes stream(10000,0x11); stream.insert(stream.end(),corrupt.begin(),corrupt.end());
    stream.insert(stream.end(),ambiguous.begin(),ambiguous.end());
    const auto embedded=wire(0x55,{0x71,0x33,0xEB,0xAA,0x55,0xAA});
    stream.insert(stream.end(),embedded.begin(),embedded.end());
    airui::FrameParser parser; const auto parsed=parser.feed(stream.data(),stream.size());
    CHECK(parsed.size()==2 && parsed[0].wire==ambiguous && parsed[1].wire==embedded);
    for(bool fire:{false,true})
    {
        const auto bytes=alarm(fire); CHECK(airui::decodeFrame(bytes.data(),bytes.size(),frame));
        airui::AlarmReport report; CHECK(airui::decodeAlarmReport(frame,report));
        if(fire) CHECK(report.fireCenters[0].x==10 && report.fireCenters[0].y==20 && report.fireCenters[1].x==640);
        else CHECK(report.shutterClosed && report.type==airui::AlarmType::StrongLight);
    }
    std::cout<<"PASS: 418 PDF golden frames, command encoding, stream framing and alarms"<<std::endl;
}

class FakeCore
{
    int master{-1};
    int slaveGuard{-1};
    std::atomic<bool> running{false};
    std::thread worker;
    mutable std::mutex mutex;
    std::mutex writeMutex;
    Bytes last;
    std::vector<airui::Frame> history;
    std::function<Bytes(const airui::Frame&)> handler;
    bool split{false};
public:
    std::string port;
    std::atomic<unsigned> count{0};
    FakeCore()
    {
        master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK); CHECK(master>=0);
        CHECK(grantpt(master)==0 && unlockpt(master)==0);
        const char* name=ptsname(master); CHECK(name); port=name;
        slaveGuard=::open(port.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK); CHECK(slaveGuard>=0);
    }
    ~FakeCore() { closeLink(); }
    void start()
    {
        running=true;
        worker=std::thread([this] {
            airui::FrameParser parser;
            while(running.load())
            {
                pollfd p{master,POLLIN,0}; if(::poll(&p,1,20)<=0) continue;
                std::uint8_t data[512]; const auto size=::read(master,data,sizeof(data));
                if(size<=0) { std::this_thread::sleep_for(2ms); continue; }
                for(const auto& command:parser.feed(data,size))
                {
                    std::function<Bytes(const airui::Frame&)> reply;
                    bool fragment;
                    { std::lock_guard<std::mutex> lock(mutex); last=command.wire; history.push_back(command); reply=handler; fragment=split; }
                    ++count;
                    if(reply) send(reply(command),fragment);
                }
            }
        });
    }
    void setHandler(std::function<Bytes(const airui::Frame&)> value,bool fragment=false)
    {
        std::lock_guard<std::mutex> lock(mutex); handler=std::move(value); split=fragment;
    }
    void payload(Bytes value,bool fragment=false)
    {
        setHandler([value=std::move(value)](const auto& command) { return response(command,value); },fragment);
    }
    Bytes lastCommand() const { std::lock_guard<std::mutex> lock(mutex); return last; }
    std::vector<airui::Frame> commands() const { std::lock_guard<std::mutex> lock(mutex); return history; }
    void clearHistory() { std::lock_guard<std::mutex> lock(mutex); history.clear(); }
    void checkConfiguration() const
    {
        termios value{}; CHECK(::tcgetattr(slaveGuard,&value)==0);
        CHECK(::cfgetispeed(&value)==B115200 && ::cfgetospeed(&value)==B115200);
        CHECK((value.c_cflag&CSIZE)==CS8 && !(value.c_cflag&(PARENB|CSTOPB)));
        CHECK(!(value.c_iflag&INPCK));
    }
    void send(const Bytes& bytes,bool fragment=false)
    {
        std::lock_guard<std::mutex> lock(writeMutex);
        for(std::size_t offset=0;offset<bytes.size();)
        {
            const auto size=fragment?std::min<std::size_t>(3,bytes.size()-offset):bytes.size()-offset;
            const auto n=::write(master,bytes.data()+offset,size);
            if(n>0) offset+=n;
            else { pollfd p{master,POLLOUT,0}; ::poll(&p,1,100); }
            if(fragment) std::this_thread::sleep_for(1ms);
        }
    }
    void allowReopen() { CHECK(::ioctl(slaveGuard,TIOCNXCL)==0); } // 伪终端保持打开，需在从端解除独占
    void closeLink()
    {
        running=false; if(worker.joinable()) worker.join();
        if(master>=0) { ::close(master); master=-1; }
        if(slaveGuard>=0) { ::close(slaveGuard); slaveGuard=-1; }
    }
};

template<class F> void expect(FakeCore& fake,F call,const char* expected,Bytes payload={1},bool split=false)
{
    fake.payload(std::move(payload),split); CHECK(call()); CHECK(fake.lastCommand()==hex(expected));
}

void uartTests()
{
    FakeCore fake;
    AiruiCore core(fake.port,115200,airui::DeviceType::Radiometric);
    CHECK(!core.isConnected()); CHECK(core.connect()); CHECK(core.isConnected());
    fake.checkConfiguration(); fake.start();
    CHECK(core.setResponseTimeout(250));
    std::uint16_t word=0; std::uint8_t small=0; double number=0; float angle=0;
    Bytes sn(64,0); const std::string serial="B3371004"; std::copy(serial.begin(),serial.end(),sn.begin());
    std::string actual;
    expect(fake,[&] { return core.getSerialNumber(actual); },"AA 04 01 71 00 20 EB AA",sn,true); CHECK(actual==serial);
    expect(fake,[&] { return core.getSensorWidth(word); },"AA 04 01 72 00 21 EB AA",{0x80,2}); CHECK(word==640);
    expect(fake,[&] { return core.getSensorHeight(word); },"AA 04 01 73 00 22 EB AA",{0,2}); CHECK(word==512);
    expect(fake,[&] { return core.backgroundCorrection(); },"AA 06 01 02 02 00 02 B7 EB AA");
    expect(fake,[&] { return core.shutterCorrection(); },"AA 06 01 02 02 01 01 B7 EB AA");
    expect(fake,[&] { return core.setAutoShutter(true); },"AA 05 01 01 01 01 B3 EB AA");
    expect(fake,[&] { return core.setShutterTimeInterval(3); },"AA 05 01 03 01 03 B7 EB AA");
    expect(fake,[&] { return core.getShutterTimeInterval(small); },"AA 04 01 03 00 B2 EB AA",{3}); CHECK(small==3);
    expect(fake,[&] { return core.setShutterFpaInterval(.5); },"AA 05 01 04 01 05 BA EB AA");
    expect(fake,[&] { return core.getShutterFpaInterval(number); },"AA 04 01 04 00 B3 EB AA",{5}); CHECK(number==.5);
    expect(fake,[&] { return core.setShutterCoreInterval(2); },"AA 05 01 0D 01 14 D2 EB AA");
    expect(fake,[&] { return core.getCoreTemperature(number); },"AA 04 01 7C 00 2B EB AA",{0x95,0x0B}); CHECK(number==29.65);
    expect(fake,[&] { return core.getFpaTemperature(number); },"AA 04 01 C3 00 72 EB AA",{0x87,0x0B}); CHECK(number==29.51);
    expect(fake,[&] { return core.saveSettings(); },"AA 04 01 7F 02 30 EB AA");
    expect(fake,[&] { return core.factoryReset(); },"AA 05 01 82 02 00 34 EB AA");
    expect(fake,[&] { return core.setDigitalZoom(3.,640,512); },"AA 0C 01 40 02 D5 00 AB 00 A9 01 54 01 78 EB AA");
    expect(fake,[&] { return core.setDigitalZoom(1.3,640,512); },"AA 0C 01 40 02 4A 00 3B 00 35 02 C3 01 79 EB AA");
    expect(fake,[&] { return core.setImageFlip(airui::ImageFlip::Vertical); },"AA 05 01 4C 01 04 01 EB AA");
    expect(fake,[&] { return core.setBootLogo(true); },"AA 05 01 49 02 80 7B EB AA");
    expect(fake,[&] { return core.setPalette(airui::Palette::IronRed); },"AA 05 01 42 02 04 F8 EB AA");
    airui::Palette palette;
    expect(fake,[&] { return core.getPalette(palette); },"AA 05 01 42 00 00 F2 EB AA",{4}); CHECK(palette==airui::Palette::IronRed);
    expect(fake,[&] { return core.setWarningThreshold(airui::WarningColor::Green,200); },"AA 06 01 4B 01 C8 01 C6 EB AA");
    expect(fake,[&] { return core.setDigitalVideo(airui::DigitalVideo::Bt601); },"AA 06 01 5D 02 05 20 35 EB AA");
    expect(fake,[&] { return core.setVideoSource(airui::VideoSource::Drc); },"AA 05 01 5C 01 02 0F EB AA");
    expect(fake,[&] { return core.setImageMode(airui::ImageMode::Classic); },"AA 05 02 1A 01 00 CC EB AA");
    airui::ImageMode mode;
    expect(fake,[&] { return core.getImageMode(mode); },"AA 04 02 1A 00 CA EB AA",{2,0,0,0}); CHECK(mode==airui::ImageMode::Forest);
    expect(fake,[&] { return core.setContrast(5); },"AA 05 01 37 01 05 ED EB AA");
    expect(fake,[&] { return core.getContrast(small); },"AA 04 01 37 00 E6 EB AA",{50}); CHECK(small==50);
    expect(fake,[&] { return core.setBrightness(17); },"AA 05 01 36 01 11 F8 EB AA");
    expect(fake,[&] { return core.setDetail(5); },"AA 05 01 38 01 05 EE EB AA");
    expect(fake,[&] { return core.setSpatialFilter(5); },"AA 05 01 39 01 05 EF EB AA");
    expect(fake,[&] { return core.setTemporalFilter(5); },"AA 05 01 3A 01 05 F0 EB AA");

    expect(fake,[&] { return core.setFocusTemperatureInterval(5); },"AA 06 08 08 01 F4 01 B6 EB AA");
    expect(fake,[&] { return core.getFocusTemperatureInterval(number); },"AA 05 08 08 00 00 BF EB AA",{0xF4,1}); CHECK(number==5);
    expect(fake,[&] { return core.autofocus(); },"AA 05 08 2F 01 00 E7 EB AA");
    expect(fake,[&] { return core.driveFocus(airui::FocusDirection::Near,airui::Adjustment::Fine); },"AA 06 08 21 01 01 01 DC EB AA");
    expect(fake,[&] { return core.stopFocus(); },"AA 05 08 22 01 00 DA EB AA");
    expect(fake,[&] { return core.setFocusSpeed(10); },"AA 05 08 24 01 0A E6 EB AA");
    expect(fake,[&] { return core.getFocusSpeed(small); },"AA 05 08 24 00 00 DB EB AA",{10}); CHECK(small==10);
    expect(fake,[&] { return core.driveZoom(airui::ZoomDirection::Wide); },"AA 06 08 31 01 01 00 EB EB AA");
    expect(fake,[&] { return core.getZoomPosition(word); },"AA 05 08 33 00 00 EA EB AA",{1,0}); CHECK(word==1);
    expect(fake,[&] { return core.getZoomSpeed(small); },"AA 05 08 34 00 00 EB EB AA",{10}); CHECK(small==10);
    airui::MotorTravel travel;
    expect(fake,[&] { return core.getZoomTravel(travel); },"AA 05 08 35 00 00 EC EB AA",{3,0,0xE8,3}); CHECK(travel.minimum==3 && travel.maximum==1000);
    expect(fake,[&] { return core.writePreset(airui::Motor::Zoom,0); },"AA 06 08 83 01 00 00 3C EB AA");
    expect(fake,[&] { return core.readPreset(airui::Motor::Zoom,0,word); },"AA 06 08 83 00 00 00 3B EB AA",{0x9B,0x0E}); CHECK(word==0x0E9B);
    expect(fake,[&] { return core.callPreset(airui::Motor::Zoom,0); },"AA 06 08 87 01 00 00 40 EB AA");
    expect(fake,[&] { return core.setFocalLength(90); },"AA 06 08 8E 01 84 03 CE EB AA");
    expect(fake,[&] { return core.getFocalLength(number); },"AA 05 08 8B 00 00 42 EB AA",{0xDC,5}); CHECK(number==150);
    expect(fake,[&] { return core.getFieldOfView(airui::Axis::Horizontal,angle); },"AA 05 01 31 00 00 E1 EB AA",{0xB8,0x1E,0x8B,0x41}); CHECK(std::abs(angle-17.39f)<.0001f);
    std::cout<<"PASS: common/image/lens controls, physical units, FOV float and motor queries"<<std::endl;

    expect(fake,[&] { return core.setTemperatureRange(airui::TemperatureRange::Auto); },"AA 05 07 01 01 03 BB EB AA");
    airui::TemperatureUnit unit;
    expect(fake,[&] { return core.getTemperatureUnit(unit); },"AA 05 07 02 00 00 B8 EB AA",{1,0,0,0}); CHECK(unit==airui::TemperatureUnit::Kelvin);
    expect(fake,[&] { return core.setReflectedTemperature(25); },"AA 08 07 0F 01 90 D0 03 00 2C EB AA");
    expect(fake,[&] { return core.getReflectedTemperature(number); },"AA 05 07 0F 00 00 C5 EB AA",{0x90,0xD0,3,0}); CHECK(number==25);
    expect(fake,[&] { return core.setAtmosphericTemperature(25); },"AA 08 07 10 01 90 D0 03 00 2D EB AA");
    expect(fake,[&] { return core.setEmissivity(1); },"AA 08 07 12 01 10 27 00 00 03 EB AA");
    expect(fake,[&] { return core.setTargetDistance(.2); },"AA 08 07 13 01 D0 07 00 00 A4 EB AA");
    expect(fake,[&] { return core.setHumidity(.4); },"AA 08 07 11 01 A0 0F 00 00 7A EB AA");
    expect(fake,[&] { return core.applyEnvironment(); },"AA 05 07 18 01 00 CF EB AA");
    expect(fake,[&] { return core.getPointTemperature({10,20},number); },"AA 08 07 1F 00 0A 00 14 00 F6 EB AA",{0x83,0xFF,0xFF,0xFF}); CHECK(number==-12.5);
    airui::CenterTemperature center;
    expect(fake,[&] { return core.getCenterTemperature(center); },"AA 05 07 2C 00 00 E2 EB AA",{0xFF,0x3F,0,0,0x40,1,0,1});
    CHECK(center.temperature==1638.3 && center.position.x==320 && center.position.y==256);
    expect(fake,[&] { return core.setFireThreshold(10000); },"AA 06 07 31 01 10 27 20 EB AA");
    expect(fake,[&] { return core.getFireThreshold(word); },"AA 04 07 31 00 E6 EB AA",{0x10,0x27}); CHECK(word==10000);
    expect(fake,[&] { return core.setStretchLow(20); },"AA 08 07 1D 01 40 0D 03 00 27 EB AA");
    expect(fake,[&] { return core.getStretchLow(number); },"AA 04 07 1D 00 D2 EB AA",{0x40,0x0D,3,0}); CHECK(number==20);
    expect(fake,[&] { return core.setStretchHigh(40); },"AA 08 07 1E 01 80 1A 06 00 78 EB AA");
    expect(fake,[&] { return core.setIsotherm(true); },"AA 05 07 20 01 01 D8 EB AA");
    expect(fake,[&] { return core.setAlarmLow(20); },"AA 08 07 2E 01 C8 00 00 00 B0 EB AA");
    expect(fake,[&] { return core.setAlarmHigh(40); },"AA 08 07 2F 01 90 01 00 00 7A EB AA");
    expect(fake,[&] { return core.calibrateLensTemperature(10); },"AA 06 07 6F 02 0A 00 32 EB AA");
    expect(fake,[&] { return core.saveLensCorrection(); },"AA 05 07 6A 02 00 22 EB AA");
    bool saved=false;
    expect(fake,[&] { return core.hasLensCorrectionData(saved); },"AA 04 07 6A 00 1F EB AA",{1}); CHECK(saved);
    airui::StrongLightProtection protection;
    expect(fake,[&] { return core.getStrongLightProtection(protection); },"AA 05 01 08 00 00 B8 EB AA",{0,0x8B,6,88}); CHECK(protection.threshold==1675 && protection.seconds==88);
    expect(fake,[&] { return core.setStrongLightProtection({true,1675,88}); },"AA 08 01 08 01 01 8B 06 58 A6 EB AA");
    expect(fake,[&] { return core.moveBlindPixelCursor(airui::CursorMove::Up20); },"AA 05 01 44 02 81 77 EB AA");
    expect(fake,[&] { return core.calibrateLensK(airui::LensKAction::Calculate); },"AA 05 01 A0 01 0C 5D EB AA");
    expect(fake,[&] { return core.compensateImage(airui::CompensationAction::Save); },"AA 05 01 A1 01 01 53 EB AA");
    airui::Synchronization sync;
    expect(fake,[&] { return core.getSynchronization(sync); },"AA 05 01 A3 00 01 54 EB AA",{0,50}); CHECK(sync.frequencyHz==50);
    expect(fake,[&] { return core.setSynchronization({airui::SyncMode::Self,50}); },"AA 06 01 A3 01 00 32 87 EB AA");

    // 所有已定义命令/操作都经过真实串口链路，防止遗漏某一组的匹配规则。
    for(const auto& info:airui::commandCatalog())
    {
        for(const auto op:{Operation::Read,Operation::Write,Operation::Action})
        {
            const int size=op==Operation::Read?info.readBytes:(op==Operation::Write?info.writeBytes:info.actionBytes);
            if(size<0) continue;
            fake.payload(op==Operation::Read?Bytes(info.responseBytes,0):Bytes{1});
            CHECK(core.transact(info.command,op,Bytes(size,0)));
        }
    }
    std::cout<<"PASS: temperature scaling, calibration, corrected manual entries and all 81 command IDs"<<std::endl;

    std::atomic<unsigned> alarmCount{0};
    core.setAlarmCallback([&](const auto&) { ++alarmCount; });
    fake.setHandler([](const auto& command) {
        Bytes data=alarm(true); const auto strong=alarm(false); data.insert(data.end(),strong.begin(),strong.end());
        const auto reply=response(command,{0,0x8B,6,88}); data.insert(data.end(),reply.begin(),reply.end()); return data;
    },true);
    CHECK(core.getStrongLightProtection(protection)); CHECK(alarmCount==2);
    fake.setHandler([](const auto& command) {
        auto bad=response(command,{0x80,2}); bad[bad.size()-3]^=1;
        auto wrong=wire(0x55,{0x73,0x33,0,2}); bad.insert(bad.end(),wrong.begin(),wrong.end());
        const auto good=response(command,{0x80,2}); bad.insert(bad.end(),good.begin(),good.end()); return bad;
    },true);
    CHECK(core.getSensorWidth(word) && word==640);
    fake.payload({0}); CHECK(!core.saveSettings()); CHECK(core.lastResult().error==airui::Error::DeviceRejected);
    fake.payload({2}); CHECK(!core.saveSettings()); CHECK(core.lastResult().error==airui::Error::MalformedResponse);
    fake.payload({0x80}); word=1234; CHECK(!core.getSensorWidth(word)); CHECK(word==1234);
    fake.setHandler([](const auto&) { return wire(0x55,{0xFF,0x33,0xFD}); });
    CHECK(!core.autofocus()); CHECK(core.lastResult().error==airui::Error::ProtocolError && core.lastResult().deviceError==airui::DeviceError::Checksum);

    const auto before=fake.count.load();
    CHECK(!core.setBrightness(101)); CHECK(!core.setFocusSpeed(33)); CHECK(!core.writePreset(airui::Motor::Focus,10));
    CHECK(!core.setDigitalZoom(1.15,640,512)); CHECK(!core.setDigitalZoom(2,0,512));
    CHECK(!core.setEmissivity(1.1)); CHECK(!core.setHumidity(-.1)); CHECK(!core.setTargetDistance(-1));
    CHECK(!core.setTargetDistance(-.00001)); CHECK(!core.setFocusTemperatureInterval(-.001));
    CHECK(!core.setShutterFpaInterval(26)); CHECK(!core.setFocalLength(std::numeric_limits<double>::infinity()));
    CHECK(fake.count.load()==before && core.lastResult().error==airui::Error::InvalidArgument);
    CHECK(core.setResponseTimeout(40)); fake.setHandler(nullptr);
    CHECK(!core.getSensorWidth(word)); CHECK(core.lastResult().error==airui::Error::Timeout);
    const auto afterTimeout=fake.count.load(); fake.send(wire(0x55,{0x72,0x33,0x80,2}));
    CHECK(!core.getSensorWidth(word)); CHECK(core.lastResult().error==airui::Error::NeedsReconnect);
    CHECK(fake.count.load()==afterTimeout);
    fake.allowReopen(); CHECK(core.reconnect()); fake.checkConfiguration(); CHECK(core.setResponseTimeout(250));
    fake.setHandler([](const auto& command) { return response(command,command.command==0x72?Bytes{0x80,2}:Bytes{0,2}); });
    std::vector<std::future<bool>> concurrent;
    for(int i=0;i<8;++i) concurrent.push_back(std::async(std::launch::async,[&,i] {
        std::uint16_t result=0; return i%2?(core.getSensorWidth(result) && result==640):(core.getSensorHeight(result) && result==512);
    }));
    for(auto& f:concurrent) CHECK(f.get());

    fake.setHandler(nullptr); CHECK(core.setResponseTimeout(3000));
    auto requests=fake.count.load();
    auto waiting=std::async(std::launch::async,[&] { std::uint16_t result; return core.getSensorWidth(result); });
    for(int i=0;i<100 && fake.count.load()==requests;++i) std::this_thread::sleep_for(2ms);
    CHECK(fake.count.load()>requests);
    const auto begin=std::chrono::steady_clock::now(); core.disconnect(); CHECK(!waiting.get());
    CHECK(std::chrono::steady_clock::now()-begin<500ms); CHECK(!core.isConnected());
    CHECK(core.lastResult().error==airui::Error::ConnectionLost);
    fake.allowReopen(); CHECK(core.connect());
    requests=fake.count.load();
    auto unplugged=std::async(std::launch::async,[&] { std::uint16_t result; return core.getSensorWidth(result); });
    for(int i=0;i<100 && fake.count.load()==requests;++i) std::this_thread::sleep_for(2ms);
    CHECK(fake.count.load()>requests); fake.closeLink(); CHECK(!unplugged.get()); CHECK(!core.isConnected());
    core.disconnect();
    std::cout<<"PASS: mixed alarms/responses, corrupt frames, errors, concurrency, timeout, disconnect and unplug"<<std::endl;

    FakeCore imagingFake; AiruiCore imaging(imagingFake.port); CHECK(imaging.connect()); imagingFake.start();
    imagingFake.payload({1}); CHECK(imaging.setFireAlarm(true)); CHECK(imaging.setFireThreshold(10000));
    CHECK(imaging.setStrongLightProtection({true,1675,88}));
    CHECK(!imaging.setTemperatureDisplay(true)); CHECK(imaging.lastResult().error==airui::Error::UnsupportedDevice);
    CHECK(!imaging.getPointTemperature({10,20},number)); CHECK(!imaging.setLensTemperatureCorrection(true));
    CHECK(imagingFake.count.load()==3); imaging.disconnect();
    std::cout<<"PASS: imaging/radiometric capability rules"<<std::endl;
}

template<class F> void controlPackets(FakeCore& fake,F call,const std::vector<Bytes>& bodies)
{
    fake.clearHistory(); CHECK(call()==1);
    const auto sent=fake.commands(); CHECK(sent.size()==bodies.size());
    for(std::size_t i=0;i<bodies.size();++i) CHECK(sent[i].wire==wire(0xAA,bodies[i]));
}

void thermalCameraTests()
{
    ThermalCamera defaults;
    CHECK(defaults.config().port=="/dev/ttyCH9344USB0" && defaults.config().baud==115200);
    CHECK(!defaults.isConnected() && defaults.setBrightness(50)==0);
    CHECK(defaults.lastResult().error==airui::Error::NotConnected);
    auto invalid=defaults.config(); invalid.responseTimeoutMs=0;
    bool threw=false;
    try { ThermalCamera bad(invalid); } catch(const std::invalid_argument&) { threw=true; }
    CHECK(threw);

    FakeCore fake;
    std::atomic<std::uint16_t> width{640},height{512};
    std::atomic<unsigned> reports{0},frames{0};
    ThermalCamera::Config config{fake.port,115200,airui::DeviceType::Radiometric,200};
    ThermalCamera camera(config);
    camera.setReportCallback([&](const auto&) { ++reports; });
    camera.setFrameCallback([&](const auto&) { ++frames; });
    CHECK(camera.connect()); fake.checkConfiguration(); fake.start();
    auto normal=[&](const airui::Frame& command) {
        if(command.operation!=0) return response(command,{1});
        const auto code=(static_cast<unsigned>(*command.commandSet)<<8)|command.command;
        switch(code)
        {
        case 0x0172: return response(command,airui::littleEndian16(width.load()));
        case 0x0173: return response(command,airui::littleEndian16(height.load()));
        case 0x0142: return response(command,{4});
        case 0x0136: return response(command,{50});
        case 0x071F: return response(command,{0x83,0xFF,0xFF,0xFF});
        case 0x070F: return response(command,airui::littleEndian32(250000));
        case 0x0710: return response(command,airui::littleEndian32(200000));
        case 0x0711: return response(command,airui::littleEndian32(4000));
        case 0x0712: return response(command,airui::littleEndian32(9500));
        case 0x0713: return response(command,airui::littleEndian32(1000000));
        default: return response(command,{0});
        }
    };
    fake.setHandler(normal);
    controlPackets(fake,[&] { return camera.zoomIn(100); },{{8,0x34,1,32},{8,0x31,1,2,0}});
    controlPackets(fake,[&] { return camera.zoomOut(1); },{{8,0x34,1,1},{8,0x31,1,1,0}});
    controlPackets(fake,[&] { return camera.zoomIn(); },{{8,0x31,1,2,0}});
    controlPackets(fake,[&] { return camera.continuousFocus(ThermalCamera::FOCUS_DIR_FAR,50,airui::Adjustment::Fine); },
                   {{8,0x24,1,16},{8,0x21,1,2,1}});
    controlPackets(fake,[&] { return camera.focusNear(1); },{{8,0x24,1,1},{8,0x21,1,1,0}});
    controlPackets(fake,[&] { return camera.continuousZoom(0,255,static_cast<airui::Adjustment>(99)); },{{8,0x32,1,0}});
    controlPackets(fake,[&] { return camera.focusStop(); },{{8,0x22,1,0}});
    controlPackets(fake,[&] { return camera.autoFocus(); },{{8,0x2F,1,0}});
    for(std::uint8_t i=0;i<4;++i)
        controlPackets(fake,[&] { return camera.setImageFlip(i); },{{1,0x4C,1,static_cast<std::uint8_t>(1<<i)}});
    controlPackets(fake,[&] { return camera.setPseudoColor(4); },{{1,0x42,2,4}});
    controlPackets(fake,[&] { return camera.setNoiseReduction(1,50); },{{1,0x3A,1,50}});
    controlPackets(fake,[&] { return camera.setVideoFreeze(true); },{{1,0x3E,2,1}});
    std::uint8_t color=0; CHECK(camera.getPseudoColor(color)==1 && color==4);

    fake.clearHistory();
    CHECK(camera.continuousZoom(3,50)==0 && camera.continuousFocus(1,101)==0);
    CHECK(camera.continuousFocus(1,50,static_cast<airui::Adjustment>(2))==0);
    CHECK(camera.setPseudoColor(20)==0 && camera.setImageFlip(4)==0 && camera.setNoiseReduction(2,50)==0);
    CHECK(camera.setDigitalZoom(1.15)==0 && camera.setDigitalZoom(std::numeric_limits<double>::quiet_NaN())==0);
    CHECK(camera.lastResult().error==airui::Error::InvalidArgument && fake.commands().empty());

    controlPackets(fake,[&] { return camera.setDigitalZoom(2.); },
                   {{1,0x72,0},{1,0x73,0},{1,0x40,2,0xA0,0,0x80,0,0xDF,1,0x7F,1}});
    width=384; height=288;
    controlPackets(fake,[&] { return camera.setDigitalZoom(2.); },
                   {{1,0x72,0},{1,0x73,0},{1,0x40,2,0x60,0,0x48,0,0x1F,1,0xD7,0}});
    controlPackets(fake,[&] { return camera.setDigitalZoom(airui::Rectangle{0,0,383,287}); },
                   {{1,0x72,0},{1,0x73,0},{1,0x40,2,0,0,0,0,0x7F,1,0x1F,1}});
    fake.clearHistory(); CHECK(camera.setDigitalZoom(airui::Rectangle{0,0,384,287})==0);
    CHECK(fake.commands().size()==2 && camera.lastResult().error==airui::Error::InvalidArgument);
    ThermalCamera::SensorSize size{99,88};
    width=0; CHECK(camera.getSensorSize(size)==0 && size.width==99 && size.height==88);
    CHECK(camera.lastResult().error==airui::Error::MalformedResponse); width=384;
    fake.setHandler([&](const auto& command) { return command.command==0x73?wire(0x55,{0xFF,0x33,0xFD}):normal(command); });
    CHECK(camera.getSensorSize(size)==0 && size.width==99 && size.height==88);
    CHECK(camera.lastResult().deviceError==airui::DeviceError::Checksum); fake.setHandler(normal);
    double temperature=123;
    controlPackets(fake,[&] { return camera.getPointTemperature({383,287},temperature); },
                   {{1,0x72,0},{1,0x73,0},{7,0x1F,0,0x7F,1,0x1F,1}});
    CHECK(temperature==-12.5);
    fake.clearHistory(); CHECK(camera.getPointTemperature({384,287},temperature)==0 && temperature==-12.5);
    CHECK(fake.commands().size()==2);

    ThermalCamera::Environment env{25,20,.95,100,.4};
    controlPackets(fake,[&] { return camera.setEnvironment(env); },
                   {{7,0x0F,1,0x90,0xD0,3,0},{7,0x10,1,0x40,0x0D,3,0},
                    {7,0x12,1,0x1C,0x25,0,0},{7,0x13,1,0x40,0x42,0x0F,0},
                    {7,0x11,1,0xA0,0x0F,0,0},{7,0x18,1,0}});
    ThermalCamera::Environment queried; CHECK(camera.getEnvironment(queried)==1);
    CHECK(queried.reflectedTemperature==25 && queried.atmosphericTemperature==20 &&
          queried.emissivity==.95 && queried.targetDistanceMeters==100 && queried.humidity==.4);
    fake.setHandler([&](const auto& command) { return command.command==0x13?response(command,{0}):normal(command); });
    CHECK(camera.getEnvironment(queried)==0 && queried.targetDistanceMeters==100 && queried.humidity==.4);
    CHECK(camera.lastResult().error==airui::Error::MalformedResponse);
    fake.setHandler([&](const auto& command) { return command.command==0x12?response(command,airui::littleEndian32(11000)):normal(command); });
    CHECK(camera.getEnvironment(queried)==0 && queried.emissivity==.95);
    CHECK(camera.lastResult().error==airui::Error::MalformedResponse); fake.setHandler(normal);
    auto badEnv=env; badEnv.humidity=2; fake.clearHistory();
    CHECK(camera.setEnvironment(badEnv)==0 && fake.commands().empty());
    badEnv=env; badEnv.atmosphericTemperature=std::numeric_limits<double>::infinity();
    CHECK(camera.setEnvironment(badEnv)==0 && fake.commands().empty());
    fake.setHandler([&](const auto& command) { return command.command==0x12?response(command,{0}):normal(command); });
    CHECK(camera.setEnvironment(env)==0 && fake.commands().size()==3);
    CHECK(camera.lastResult().error==airui::Error::DeviceRejected);
    fake.clearHistory(); fake.payload({0}); CHECK(camera.zoomIn(100)==0 && fake.commands().size()==1);
    CHECK(fake.lastCommand()==wire(0xAA,{8,0x34,1,32}));
    fake.setHandler(normal,true); fake.clearHistory();
    std::vector<std::future<std::int32_t>> moving;
    for(int i=0;i<8;++i) moving.push_back(std::async(std::launch::async,[&,i] { return i%2?camera.zoomIn(100):camera.zoomOut(1); }));
    for(auto& result:moving) CHECK(result.get()==1);
    const auto moves=fake.commands(); CHECK(moves.size()==16);
    for(std::size_t i=0;i<moves.size();i+=2)
    {
        CHECK(moves[i].command==0x34 && moves[i+1].command==0x31);
        CHECK(moves[i+1].parameters==Bytes({static_cast<std::uint8_t>(moves[i].parameters[0]==32?2:1),0}));
    }
    std::cout<<"PASS: ThermalCamera directions, speed, image controls, F384/F640 bounds, environment and transaction grouping"<<std::endl;

    auto withAlarms=[&](const airui::Frame& command) {
        auto data=alarm(true); const auto strong=alarm(false); data.insert(data.end(),strong.begin(),strong.end());
        const auto reply=normal(command); data.insert(data.end(),reply.begin(),reply.end()); return data;
    };
    fake.setHandler(withAlarms,true);
    std::uint16_t value; CHECK(camera.getSensorWidth(value)==1);
    for(int i=0;i<100 && (reports.load()<2 || frames.load()<2);++i) std::this_thread::sleep_for(2ms);
    CHECK(reports==2 && frames==2);
    auto badConfig=camera.config(); badConfig.port.clear();
    CHECK(!camera.init(badConfig) && camera.isConnected());
    CHECK(camera.setResponseTimeout(80)); CHECK(!camera.setResponseTimeout(0));
    CHECK(camera.config().responseTimeoutMs==80);
    FakeCore other;
    CHECK(camera.init(other.port)); CHECK(!camera.isConnected());
    CHECK(camera.config().type==airui::DeviceType::Radiometric && camera.config().responseTimeoutMs==80);
    fake.closeLink(); other.setHandler(withAlarms,true); CHECK(camera.connect()); other.start();
    CHECK(camera.getSensorWidth(value)==1);
    for(int i=0;i<100 && (reports.load()<4 || frames.load()<4);++i) std::this_thread::sleep_for(2ms);
    CHECK(reports==4 && frames==4);
    other.setHandler(nullptr); CHECK(camera.setResponseTimeout(40));
    CHECK(camera.getSensorWidth(value)==0 && camera.lastResult().error==airui::Error::Timeout);
    const auto count=other.count.load();
    CHECK(camera.getSensorWidth(value)==0 && camera.lastResult().error==airui::Error::NeedsReconnect);
    CHECK(other.count==count); other.allowReopen(); CHECK(camera.reconnect());
    other.setHandler(normal); CHECK(camera.getSensorWidth(value)==1 && value==384);

    other.setHandler(nullptr); CHECK(camera.setResponseTimeout(3000));
    const auto requests=other.count.load();
    auto waiting=std::async(std::launch::async,[&] { std::uint16_t word; return camera.getSensorWidth(word); });
    for(int i=0;i<100 && other.count.load()==requests;++i) std::this_thread::sleep_for(2ms);
    CHECK(other.count.load()>requests);
    const auto begin=std::chrono::steady_clock::now(); camera.disconnect();
    CHECK(waiting.get()==0 && std::chrono::steady_clock::now()-begin<500ms && !camera.isConnected());
    other.closeLink();

    FakeCore imagingFake; ThermalCamera imaging(imagingFake.port);
    CHECK(imaging.connect()); imagingFake.start(); imagingFake.payload({1});
    CHECK(imaging.setFireAlarm(true)==1 && imaging.setStrongLightProtection({true,1675,88})==1);
    imagingFake.clearHistory();
    CHECK(imaging.setEnvironment(env)==0 && imaging.getPointTemperature({1,1},temperature)==0);
    CHECK(imaging.setTemperatureUnit(airui::TemperatureUnit::Celsius)==0);
    CHECK(imaging.lastResult().error==airui::Error::UnsupportedDevice && imagingFake.commands().empty());
    imaging.disconnect(); imagingFake.closeLink();
    std::cout<<"PASS: ThermalCamera default config, callbacks, reconfiguration, timeout recovery, disconnect and capabilities"<<std::endl;
}

int main(int argc,char** argv)
{
    try
    {
        CHECK(argc==2);
        LogConfig config; config.level=LogLevel::ERROR; LogManager::configure(config);
        codecTests(argv[1]); uartTests(); thermalCameraTests(); LogManager::getInstance().stop(); return 0;
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
