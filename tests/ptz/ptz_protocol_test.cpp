#include"ptz/ptz_protocol.h"

#include<iostream>
#include<limits>
#include<sstream>
#include<stdexcept>
#include<string>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

using namespace ptz;
using Bytes = std::vector<std::uint8_t>;

template<std::size_t N>
void expect(const std::array<std::uint8_t, N>& frame, const char* text)
{
    std::istringstream input(text);
    Bytes expected;
    unsigned value;
    while (input >> std::hex >> value)
    {
        CHECK(value <= 255);
        expected.push_back(static_cast<std::uint8_t>(value));
    }
    CHECK(Bytes(frame.begin(), frame.end()) == expected);
}
template<class Function>
void expectInvalid(Function function)
{
    bool threw = false;
    try { function(); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void pelcoGoldenFrames()
{
    // 按主文档第 1~40 项逐项校验；期望值直接来自表格并手算校验和。
    expect(makeMove(HorizontalDirection::Right, 64, VerticalDirection::None, 0), "FF 01 00 02 40 00 43");
    expect(makeMove(HorizontalDirection::Left, 64, VerticalDirection::None, 0), "FF 01 00 04 40 00 45");
    expect(makeMove(HorizontalDirection::None, 0, VerticalDirection::Down, 32), "FF 01 00 10 00 20 31");
    expect(makeMove(HorizontalDirection::None, 0, VerticalDirection::Up, 32), "FF 01 00 08 00 20 29");
    expect(makeSetPreset(1), "FF 01 00 03 00 01 05");
    expect(makeCallPreset(1), "FF 01 00 07 00 01 09");
    expect(makeClearPreset(1), "FF 01 00 05 00 01 07");
    expect(makeAuxiliary(true, 255), "FF 01 00 09 00 FF 09");
    expect(makeAuxiliary(false, 255), "FF 01 00 0B 00 FF 0B");
    expect(makeLineScan(PelcoCommand::SetLineScanStart, 7), "FF 01 00 11 00 07 19");
    expect(makeLineScan(PelcoCommand::SetLineScanEnd, 7), "FF 01 00 13 00 07 1B");
    expect(makeLineScan(PelcoCommand::LineScanOn, 7), "FF 01 00 1B 00 07 23");
    expect(makeLineScan(PelcoCommand::LineScanOff, 7), "FF 01 00 1D 00 07 25");
    expect(makeApplePeelScan(PelcoCommand::SetPreset), "FF 01 00 03 00 5D 61");
    expect(makeApplePeelScan(PelcoCommand::ClearPreset), "FF 01 00 05 00 5D 63");
    expect(makeApplePeelScan(PelcoCommand::CallPreset), "FF 01 00 07 00 5D 65");
    expect(makeApplePeelStep(5), "FF 01 00 33 01 F4 29");
    expect(makeSpeed(PelcoCommand::SetScanSpeed, 64, 32), "FF 01 00 31 40 20 92");
    expect(makeCallCruise(100), "FF 01 00 07 00 64 6C");
    expect(makeCruiseDwell(CruiseDwell::Seconds10), "FF 01 00 03 00 65 69");
    expect(makeSpeed(PelcoCommand::SetCruiseSpeed, 64, 32), "FF 01 00 67 40 20 C8");
    expect(makeWatch(true), "FF 01 00 07 00 70 78");
    expect(makeWatch(false), "FF 01 00 07 00 71 79");
    expect(makeWatchAction(WatchAction::Preset1), "FF 01 00 03 00 75 79");
    expect(makeWatchAction(WatchAction::Cruise1), "FF 01 00 05 00 75 7B");
    expect(makeWatchAction(WatchAction::LineScan1), "FF 01 00 07 00 75 7D");
    expect(makeWatchTime(WatchTime::Minutes5), "FF 01 00 03 00 53 57");
    expect(makeAngle(Axis::Horizontal, 10), "FF 01 00 4B 03 E8 37");
    expect(makeAngle(Axis::Vertical, 350), "FF 01 00 4D 88 B8 8E");
    expect(makeQueryAngle(Axis::Horizontal), "FF 01 00 51 00 00 52");
    expect(makePelcoFrame(PelcoCommand::HorizontalAngleReport, 0x03, 0xE8), "FF 01 00 59 03 E8 45");
    expect(makeQueryAngle(Axis::Vertical), "FF 01 00 53 00 00 54");
    expect(makePelcoFrame(PelcoCommand::VerticalAngleReport, 0x88, 0xB8), "FF 01 00 5B 88 B8 9C");
    expect(makeReboot(), "FF 01 00 07 00 78 80");
    expect(makeSelfCheck(), "FF 01 00 07 00 77 7F");
    expect(makeRealtimeReport(true), "FF 01 00 09 00 05 0F");
    expect(makeRealtimeReport(false), "FF 01 00 0B 00 05 11");
    expect(makeSpeed(PelcoCommand::SetPositionSpeed, 64, 32), "FF 01 00 5F 40 20 C0");
    expect(makeStop(), "FF 01 00 00 00 00 01");
    expect(makeMove(HorizontalDirection::Left, 64, VerticalDirection::Up, 32, 2), "FF 02 00 0C 40 20 6E");
    expect(encodePelco(255, 255, 255, 255, 255), "FF FF FF FF FF FF FB");
    expect(makeStop(0), "FF 00 00 00 00 00 00");
    expect(makeAngle(Axis::Horizontal, 360), "FF 01 00 4B 8C A0 78");
    expect(makeAngle(Axis::Vertical, 359.99), "FF 01 00 4D 8C 9F 79");

    for (unsigned address = 0; address <= 255; ++address)
    {
        const auto frame = makeStop(static_cast<std::uint8_t>(address));
        CHECK(frame[1] == address && frame[6] == address);
    }
    for (std::uint16_t preset : {1, 80, 131, 255})
        CHECK(makeSetPreset(preset)[5] == preset);
    for (unsigned preset = 100; preset <= 107; ++preset)
    {
        CHECK(makeCallCruise(preset)[5] == preset);
        CHECK(makeCruiseDwell(static_cast<CruiseDwell>(preset))[5] == preset);
    }
    for (unsigned time = 81; time <= 90; ++time)
        CHECK(makeWatchTime(static_cast<WatchTime>(time))[5] == time);
    for (double degrees : {0.0, 90.0, 270.0, 359.99})
        CHECK(makeAngle(Axis::Vertical, degrees)[3] == 0x4D);
}

void extendedGoldenFrames()
{
    // 两轴使用不同数值，防止方位/俯仰或高/低字节被互换。
    expect(makeVariableSpeed(HorizontalDirection::Left, 2.5, VerticalDirection::Up, 1),
           "6E 01 10 AA 0B 01 01 00 00 00 64 02 00 00 00 FA 00 E6");
    expect(makeVariableSpeedRaw(HorizontalDirection::Right, 65535, VerticalDirection::Down, 0),
           "6E 01 10 AA 0B 01 02 00 00 00 00 01 00 00 FF FF 00 E6");
    expect(makePosition(10, 20),
           "6E 01 10 AA 0B 02 00 00 00 03 E8 00 00 00 07 D0 00 E6");
    expect(makePositionRaw(0x1234, 0xABCD),
           "6E 01 10 AA 0B 02 00 00 00 12 34 00 00 00 AB CD 00 E6");
    expect(makePositionWithSpeed({true, 1, 10}, {false, 2.5, 20}),
           "6E 01 10 AA 0B 03 01 00 64 03 E8 00 00 FA 07 D0 00 E6");
    expect(makePositionWithSpeedRaw({false, 0x1234, 0x5678}, {true, 0x9ABC, 0xDEF0}),
           "6E 01 10 AA 0B 03 00 12 34 56 78 01 9A BC DE F0 00 E6");
    CHECK(toHundredths(0) == 0);
    CHECK(toHundredths(0.01) == 1);
    CHECK(toHundredths(655.35) == 65535);
    CHECK(toHundredths(1.234) == 123);
    CHECK(toHundredths(1.236) == 124);
    const auto position = makePosition(10, 20);
    CHECK(readBigEndian16(position.data() + 9) == 1000);
    CHECK(readBigEndian16(position.data() + 14) == 2000);
    CHECK(fromHundredths(1000) == 10);
}

void validationTests()
{
    for (std::uint16_t preset : {0, 81, 93, 100, 130, 256, 2098, 65535})
    {
        expectInvalid([&] { makeSetPreset(preset); });
        expectInvalid([&] { makeCallPreset(preset); });
        expectInvalid([&] { makeClearPreset(preset); });
    }
    expectInvalid([] { makeMove(HorizontalDirection::Right, 65, VerticalDirection::Up, 0); });
    expectInvalid([] { makeMove(HorizontalDirection::Right, 0, VerticalDirection::Up, 65); });
    expectInvalid([] { makeMove(static_cast<HorizontalDirection>(3), 0, VerticalDirection::Up, 0); });
    expectInvalid([] { makeMove(HorizontalDirection::Right, 0, static_cast<VerticalDirection>(3), 0); });
    expectInvalid([] { makeSpeed(PelcoCommand::Stop, 0, 0); });
    expectInvalid([] { makeSpeed(PelcoCommand::SetScanSpeed, 65, 0); });
    expectInvalid([] { makeSpeed(PelcoCommand::SetPositionSpeed, 0, 65); });
    expectInvalid([] { makeLineScan(PelcoCommand::LineScanOn, 8); });
    expectInvalid([] { makeLineScan(PelcoCommand::Stop); });
    expectInvalid([] { makeApplePeelScan(PelcoCommand::Stop); });
    expectInvalid([] { makeCallCruise(99); });
    expectInvalid([] { makeCallCruise(108); });
    expectInvalid([] { makeCruiseDwell(static_cast<CruiseDwell>(99)); });
    expectInvalid([] { makeWatchTime(static_cast<WatchTime>(91)); });
    expectInvalid([] { makeWatchAction(static_cast<WatchAction>(3)); });
    expectInvalid([] { makeQueryAngle(static_cast<Axis>(2)); });
    expectInvalid([] { makeAngle(static_cast<Axis>(2), 10); });
    for (double degrees : {-1.0, 360.01})
        expectInvalid([&] { makeAngle(Axis::Horizontal, degrees); });
    for (double degrees : {-0.01, 90.01, 180.0, 269.99, 360.0})
        expectInvalid([&] { makeAngle(Axis::Vertical, degrees); });
    for (double value : {-0.01, 655.36, std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        expectInvalid([&] { toHundredths(value); });
        expectInvalid([&] { makeApplePeelStep(value); });
        expectInvalid([&] { makePosition(value, 0); });
        expectInvalid([&] { makePosition(0, value); });
        expectInvalid([&] { makeVariableSpeed(HorizontalDirection::Right, value, VerticalDirection::Up, 0); });
        expectInvalid([&] { makePositionWithSpeed({true, value, 0}, {true, 0, 0}); });
    }
    expectInvalid([] { makeAngle(Axis::Horizontal, std::numeric_limits<double>::quiet_NaN()); });
    expectInvalid([] { makeVariableSpeedRaw(HorizontalDirection::None, 0, VerticalDirection::Up, 0); });
    expectInvalid([] { makeVariableSpeedRaw(HorizontalDirection::Right, 0, VerticalDirection::None, 0); });
}

void receiveTests()
{
    const PelcoFrame horizontal{0xFF, 0x01, 0x00, 0x59, 0x03, 0xE8, 0x45};
    const PelcoFrame vertical{0xFF, 0x02, 0x00, 0x5B, 0x88, 0xB8, 0x9D};
    PelcoMessage message;
    AngleReport report;
    CHECK(decodeAngleReport(horizontal.data(), horizontal.size(), report));
    CHECK(report.address == 1 && report.axis == Axis::Horizontal && report.hundredths == 1000 && report.degrees == 10);
    CHECK(decodeAngleReport(vertical.data(), vertical.size(), report));
    CHECK(report.address == 2 && report.axis == Axis::Vertical && report.hundredths == 35000 && report.degrees == 350);
    CHECK(!decodePelcoFrame(nullptr, 7, message));
    CHECK(!decodePelcoFrame(nullptr, 0, message));
    CHECK(!decodePelcoFrame(horizontal.data(), 6, message));
    CHECK(!decodePelcoFrame(horizontal.data(), 8, message));
    auto bad = horizontal;
    bad[0] = 0xFE;
    CHECK(!decodePelcoFrame(bad.data(), bad.size(), message));
    bad = horizontal;
    bad[6] ^= 1;
    CHECK(!decodeAngleReport(bad.data(), bad.size(), report));
    CHECK(report.axis == Axis::Vertical && report.hundredths == 35000); // 失败不覆盖输出
    const auto stop = makeStop();
    CHECK(!decodeAngleReport(stop.data(), stop.size(), report));
    const auto nonzeroCommand1 = encodePelco(1, 1, 0x59, 0, 0);
    CHECK(!decodeAngleReport(nonzeroCommand1.data(), nonzeroCommand1.size(), report));

    Bytes pair(horizontal.begin(), horizontal.end());
    pair.insert(pair.end(), vertical.begin(), vertical.end());
    for (std::size_t split = 1; split < pair.size(); ++split)
    {
        PelcoParser parser;
        auto first = parser.feed(pair.data(), split);
        const auto second = parser.feed(pair.data() + split, pair.size() - split);
        first.insert(first.end(), second.begin(), second.end());
        CHECK(first.size() == 2);
        CHECK(decodeAngleReport(first[0], report) && report.degrees == 10);
        CHECK(decodeAngleReport(first[1], report) && report.degrees == 350);
    }
    PelcoParser parser;
    unsigned count = 0;
    for (const auto byte : pair) count += static_cast<unsigned>(parser.feed(&byte, 1).size());
    CHECK(count == 2);
    CHECK(parser.feed(nullptr, 0).empty());
    expectInvalid([&] { parser.feed(nullptr, 1); });
    CHECK(parser.feed(horizontal.data(), 3).empty());
    parser.reset();
    CHECK(parser.feed(vertical.data(), vertical.size()).size() == 1);

    // 噪音、坏校验、截断帧、同步字节出现在数据内时仍能恢复。
    const PelcoFrame embeddedSync{0xFF, 0xFF, 0x00, 0x59, 0x00, 0xFF, 0x57};
    Bytes noise(10000, 0x12);
    noise.insert(noise.end(), bad.begin(), bad.end());
    noise.insert(noise.end(), horizontal.begin(), horizontal.begin() + 3);
    noise.insert(noise.end(), horizontal.begin(), horizontal.end());
    noise.insert(noise.end(), embeddedSync.begin(), embeddedSync.end());
    noise.insert(noise.end(), vertical.begin(), vertical.end());
    const auto messages = parser.feed(noise.data(), noise.size());
    CHECK(messages.size() == 3);
    CHECK(decodeAngleReport(messages[0], report) && report.degrees == 10);
    CHECK(decodeAngleReport(messages[1], report) && report.hundredths == 255 && report.address == 255);
    CHECK(decodeAngleReport(messages[2], report) && report.degrees == 350);
}

int main()
{
    try
    {
        pelcoGoldenFrames();
        extendedGoldenFrames();
        validationTests();
        receiveTests();
        std::cout << "PASS: documented PTZ frames, parameter boundaries and receive parsing\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
