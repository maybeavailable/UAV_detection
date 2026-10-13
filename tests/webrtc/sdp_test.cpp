#include"webrtc/sdp.h"

#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

std::string fingerprint(char value = 'A')
{
    std::string result;
    for (unsigned i = 0; i < 32; ++i) { if (i) result += ':'; result += value; result += value; }
    return result;
}
std::string transportAttributes(std::string_view role = "actpass")
{
    return "a=ice-ufrag:abcd\n"
           "a=ice-pwd:abcdefghijklmnopqrstuv1234\n"
           "a=ice-options:trickle renomination\n"
           "a=fingerprint:sha-256 " + fingerprint() + "\n"
           "a=setup:" + std::string(role) + "\n";
}
std::string browserOffer()
{
    return "v=0\n"
           "o=- 4611731400430051336 2 IN IP4 127.0.0.1\n"
           "s=-\n"
           "t=0 0\n"
           "a=group:BUNDLE 0 1 2\n"
           "a=extmap-allow-mixed\n"
           "a=msid-semantic: WMS stream\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 126\n"
           "c=IN IP4 0.0.0.0\n"
           "a=mid:0\n"
           "a=sendrecv\n" + transportAttributes() +
           "a=rtcp:9 IN IP4 0.0.0.0\n"
           "a=rtcp-mux\n"
           "a=rtcp-rsize\n"
           "a=rtpmap:111 opus/48000/2\n"
           "a=fmtp:111 minptime=10;useinbandfec=1\n"
           "a=rtpmap:0 PCMU/8000\n"
           "a=rtpmap:8 PCMA/8000\n"
           "a=rtpmap:126 telephone-event/8000\n"
           "a=fmtp:126 0-16\n"
           "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\n"
           "a=msid:stream audio-track\n"
           "a=ssrc:1234 cname:audio-cname\n"
           "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 127\n"
           "c=IN IP4 0.0.0.0\n"
           "b=AS:2048\n"
           "b=TIAS:2000000\n"
           "a=mid:1\n"
           "a=recvonly\n" + transportAttributes() +
           "a=rtcp:9 IN IP4 0.0.0.0\n"
           "a=rtcp-mux\n"
           "a=rtcp-rsize\n"
           "a=rtpmap:96 VP8/90000\n"
           "a=rtpmap:97 rtx/90000\n"
           "a=fmtp:97 apt=96\n"
           "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\n"
           "a=rtpmap:102 H264/90000\n" // fmtp 先于 rtpmap
           "a=rtpmap:103 rtx/90000\n"
           "a=fmtp:103 apt=102\n"
           "a=rtpmap:127 H264/90000\n"
           "a=fmtp:127 profile-level-id=640c1f;packetization-mode=1\n"
           "a=rtcp-fb:96 nack\n"
           "a=rtcp-fb:102 nack pli\n"
           "a=rtcp-fb:102 ccm fir\n"
           "a=rtcp-fb:* transport-cc\n"
           "a=extmap:4/recvonly urn:ietf:params:rtp-hdrext:sdes:mid\n"
           "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\n"
           "a=extmap:14 urn:ietf:params:rtp-hdrext:encrypt urn:example:encrypted-extension\n"
           "a=rid:q recv\n"
           "a=rid:h recv\n"
           "a=simulcast:recv q;h\n"
           "a=candidate:1 1 UDP 2130706431 camera.local 5000 typ host generation 0 ufrag abcd\n"
           "a=candidate:2 1 UDP 1694498815 198.51.100.1 6000 typ srflx raddr 0.0.0.0 rport 9\n"
           "a=end-of-candidates\n"
           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\n"
           "c=IN IP4 0.0.0.0\n"
           "a=mid:2\n" + transportAttributes() +
           "a=sctp-port:5000\n"
           "a=max-message-size:262144\n"
           "a=x-vendor:opaque:value\n"
           "a=x-flag\n"
           "a=x-empty:\n";
}
std::string bundledAnswer()
{
    return "v=0\n"
           "o=- 123456789 3 IN IP4 127.0.0.1\n"
           "s=UAV preview\n"
           "c=IN IP4 0.0.0.0\n"
           "t=0 0\n"
           "a=group:BUNDLE 1 2\n"
           "a=ice-lite\n"
           "a=msid-semantic: WMS camera\n"
           "m=audio 0 UDP/TLS/RTP/SAVPF 0\n"
           "a=mid:0\n"
           "a=inactive\n"
           "m=video 9 UDP/TLS/RTP/SAVPF 102 103\n"
           "a=mid:1\n"
           "a=sendonly\n" + transportAttributes("passive") +
           "a=rtcp-mux\n"
           "a=rtcp-rsize\n"
           "a=rtpmap:102 H264/90000\n"
           "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0KAHtoCgPaE,aM48gA==\n"
           "a=rtpmap:103 rtx/90000\n"
           "a=fmtp:103 apt=102\n"
           "a=rtcp-fb:102 nack pli\n"
           "a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\n"
           "a=msid:camera video-track\n"
           "a=ssrc:123456 cname:camera-cname\n"
           "a=ssrc:123457 cname:camera-cname\n"
           "a=ssrc-group:FID 123456 123457\n"
           "a=candidate:1 1 UDP 2130706431 192.0.2.1 5000 typ host\n"
           "a=end-of-candidates\n"
           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\n"
           "a=mid:2\n"
           "a=sctp-port:5000\n"
           "a=max-message-size:0\n"; // 非 tag 媒体不重复 ICE/DTLS
}
SdpSession parsed(std::string_view text)
{
    SdpSession result;
    std::string reason = "previous";
    if (!SdpSession::parse(text, result, &reason)) throw std::runtime_error(reason);
    CHECK(reason.empty() && result.validate());
    return result;
}
void replace(std::string& text, const std::string& oldValue, const std::string& newValue)
{
    const auto at = text.find(oldValue);
    CHECK(at != std::string::npos);
    text.replace(at, oldValue.size(), newValue);
}
template<class Function>
void expectInvalid(Function call)
{
    bool threw = false;
    try { call(); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
void roundTripTests()
{
    auto offer = parsed(browserOffer());
    CHECK(offer.validateWebRtc(SdpDescriptionType::Offer));
    CHECK(!offer.validateWebRtc(SdpDescriptionType::Answer));
    CHECK(offer.origin.sessionId == 4611731400430051336ULL && offer.media.size() == 3);
    const auto* video = offer.findMedia("1");
    CHECK(video && video->isRtp() && !video->isSctp() && !video->isRejected());
    CHECK(video->formats == std::vector<std::string>({"96", "97", "102", "103", "127"}));
    CHECK(video->codecsNamed("h264").size() == 2);
    CHECK(video->codec(102)->parameter("PROFILE-LEVEL-ID") == "42e01f");
    CHECK(video->codec(102)->parameter("packetization-mode") == "1");
    CHECK(!video->codec(102)->parameter("absent"));
    CHECK(video->rtxPayloadType(102) == 103 && video->rtxPayloadType(96) == 97 && !video->rtxPayloadType(127));
    CHECK(video->feedback.size() == 4 && !video->feedback.back().payloadType);
    CHECK(video->common.extmaps[0].direction == SdpDirection::RecvOnly);
    CHECK(video->common.extmaps[2].attributes == "urn:example:encrypted-extension");
    CHECK(video->candidates.size() == 2 && video->candidates[0].isMdns() && video->endOfCandidates);
    CHECK(video->attribute("simulcast")->value == "recv q;h");
    CHECK(offer.findMedia("0")->codec(111)->channels == 2);
    CHECK(offer.findMedia("0")->codec(126)->parameters == "0-16");
    CHECK(!offer.findMedia("0")->codec(126)->parameter("apt"));
    CHECK(offer.findMedia("2")->sctpPort == 5000 && offer.findMedia("2")->maxMessageSize == 262144);
    CHECK(!offer.findMedia("2")->attribute("x-flag")->value);
    CHECK(offer.findMedia("2")->attribute("x-empty")->value == "");
    CHECK(offer.findMedia("2")->attribute("x-vendor")->value == "opaque:value");
    CHECK(!offer.findMedia("missing") && offer.bundleGroup("1")->mids.front() == "0");
    const auto text = offer.toSdp();
    for (std::size_t i = 0; i < text.size(); ++i) if (text[i] == '\n') CHECK(i > 0 && text[i - 1] == '\r');
    CHECK(parsed(text).toSdp() == text);
    auto noEnding = browserOffer(); noEnding.pop_back();
    CHECK(parsed(noEnding).toSdp() == text);

    const auto answer = parsed(bundledAnswer());
    CHECK(answer.validateWebRtc(SdpDescriptionType::Answer));
    CHECK(!answer.validateWebRtc(SdpDescriptionType::Offer));
    CHECK(answer.iceLite && answer.findMedia("0")->isRejected());
    CHECK(answer.effectiveDirection(*answer.findMedia("1")) == SdpDirection::SendOnly);
    CHECK(answer.effectiveConnection(*answer.findMedia("2"))->address == "0.0.0.0");
    CHECK(answer.findMedia("2")->maxMessageSize.has_value() && *answer.findMedia("2")->maxMessageSize == 0);
    CHECK(!answer.effectiveAttributes(*answer.findMedia("2")).iceUfrag);
    CHECK(answer.transportMedia(*answer.findMedia("2"), SdpDescriptionType::Answer).mid == "1");
    CHECK(parsed(answer.toSdp()).validateWebRtc(SdpDescriptionType::Answer));
}
void inheritanceAndGenerationTests()
{
    SdpSession session;
    session.origin.sessionId = std::numeric_limits<std::uint64_t>::max();
    session.origin.sessionVersion = std::numeric_limits<std::uint64_t>::max();
    session.origin.connection = {"IP6", "2001:db8::1"};
    session.connection = SdpConnection{"IP6", "::"};
    session.sessionName = "无人机预览";
    session.common.iceUfrag = "abcd";
    session.common.icePwd = "abcdefghijklmnopqrstuv1234";
    session.common.fingerprints = {{"sha-256", fingerprint('a')}};
    session.common.setup = SdpSetupRole::ActPass;
    session.common.direction = SdpDirection::RecvOnly;
    session.common.extmapAllowMixed = true;
    session.common.extmaps.push_back({1, std::nullopt, "urn:ietf:params:rtp-hdrext:sdes:mid", std::nullopt});
    SdpMedia video;
    video.mid = "v";
    video.rtcpMux = true;
    video.formats = {"102"};
    video.codecs = {{102, "H264", 90000, 1, "packetization-mode=1;profile-level-id=42e01f"}};
    session.media.push_back(video);
    CHECK(session.validateWebRtc(SdpDescriptionType::Offer));
    const auto canonical = parsed(session.toSdp());
    const auto effective = canonical.effectiveAttributes(canonical.media[0]);
    CHECK(effective.iceUfrag == "abcd" && effective.setup == SdpSetupRole::ActPass);
    CHECK(effective.fingerprints[0].value == fingerprint('A') && effective.extmaps.size() == 1);
    CHECK(canonical.effectiveDirection(canonical.media[0]) == SdpDirection::RecvOnly);
    session.media[0].common.direction = SdpDirection::SendOnly;
    session.media[0].common.iceUfrag = "wxyz";
    session.media[0].common.icePwd = "abcdefghijklmnopqrstuv5678";
    CHECK(session.effectiveDirection(session.media[0]) == SdpDirection::SendOnly);
    CHECK(session.effectiveAttributes(session.media[0]).iceUfrag == "wxyz");
    CHECK(session.validateWebRtc(SdpDescriptionType::Offer));

    auto bundled = parsed(browserOffer());
    auto& only = bundled.media[1];
    only.port = 0; only.bundleOnly = true;
    only.common.iceUfrag.reset(); only.common.icePwd.reset(); only.common.fingerprints.clear(); only.common.setup.reset();
    only.rtcpMux = false; only.rtcp.reset(); only.rtcpRsize = false; only.candidates.clear(); only.endOfCandidates = false;
    CHECK(!only.isRejected());
    CHECK(bundled.validateWebRtc(SdpDescriptionType::Offer));
    CHECK(bundled.transportMedia(only, SdpDescriptionType::Offer).mid == "0");
    CHECK(parsed(bundled.toSdp()).validateWebRtc(SdpDescriptionType::Offer));

    // application 也可以是 BUNDLE tag，代表 RTP 媒体承载 rtcp-mux。
    auto dataTag = parsed(bundledAnswer());
    dataTag.groups[0].mids = {"2", "1"};
    dataTag.media[2].common = dataTag.media[1].common;
    dataTag.media[2].common.direction.reset();
    dataTag.media[2].rtcpMux = true;
    dataTag.media[1].common = {};
    dataTag.media[1].common.direction = SdpDirection::SendOnly;
    dataTag.media[1].rtcpMux = false;
    CHECK(dataTag.validateWebRtc(SdpDescriptionType::Answer));
    CHECK(parsed(dataTag.toSdp()).validateWebRtc(SdpDescriptionType::Answer));

    SdpSession simple;
    CHECK(parsed(simple.toSdp()).media.empty());
    simple.information = "session info"; simple.uri = "https://example.com/sdp";
    simple.emails = {"operator@example.com"}; simple.phones = {"+123456789"};
    simple.timings = {{1, 10, {"7d 1h 0 25h"}}, {20, 30, {}}}; simple.timeZones = "2882844526 -1h";
    simple.key = "prompt";
    CHECK(parsed(simple.toSdp()).toSdp() == simple.toSdp());
}
void helperTests()
{
    CHECK(answerDirection(SdpDirection::SendOnly, true, true) == SdpDirection::RecvOnly);
    CHECK(answerDirection(SdpDirection::RecvOnly, true, true) == SdpDirection::SendOnly);
    CHECK(answerDirection(SdpDirection::SendRecv, true, true) == SdpDirection::SendRecv);
    CHECK(answerDirection(SdpDirection::SendRecv, true, false) == SdpDirection::SendOnly);
    CHECK(answerDirection(SdpDirection::SendRecv, false, true) == SdpDirection::RecvOnly);
    CHECK(answerDirection(SdpDirection::Inactive, true, true) == SdpDirection::Inactive);
    CHECK(answerDirection(SdpDirection::RecvOnly, false, true) == SdpDirection::Inactive);
    CHECK(answerSetupRole(SdpSetupRole::ActPass) == SdpSetupRole::Active);
    CHECK(answerSetupRole(SdpSetupRole::ActPass, false) == SdpSetupRole::Passive);
    CHECK(answerSetupRole(SdpSetupRole::Active) == SdpSetupRole::Passive);
    CHECK(answerSetupRole(SdpSetupRole::Passive) == SdpSetupRole::Active);
    expectInvalid([] { answerSetupRole(SdpSetupRole::HoldConn); });
    expectInvalid([] { answerDirection(static_cast<SdpDirection>(99), true, true); });
}
void invalidInputTests()
{
    const auto fixture = browserOffer();
    std::vector<std::string> bad{"", "v=1\n", "o=- 1 1 IN IP4 0.0.0.0\n", fixture + "\n", std::string(256 * 1024 + 1, 'x')};
    const std::vector<std::pair<std::string, std::string>> mutations{
        {"v=0", "v=1"}, {"v=0\n", "v=0\nv=0\n"}, {"s=-\n", "s=-\ns=duplicate\n"},
        {"t=0 0\n", ""}, {"t=0 0", "t=10 9"}, {"4611731400430051336", "18446744073709551616"},
        {"o=- 4611731400430051336 2 IN IP4 127.0.0.1", "o=- 1 2 IN IP6 127.0.0.1"},
        {"a=mid:1", "a=mid:0"}, {"a=mid:1", "a=mid:1\na=mid:another"},
        {"a=group:BUNDLE 0 1 2", "a=group:BUNDLE 0 1 9"}, {"a=group:BUNDLE 0 1 2", "a=group:BUNDLE 0 1 1"},
        {"m=video 9", "m=video 65536"}, {"m=video 9", "m=video -1"}, {"m=video 9", "m=video 9/0"},
        {"96 97 102 103 127", "96 97 102 103 128"}, {"96 97 102 103 127", "96 96 102 103 127"},
        {"a=rtpmap:96 VP8/90000", "a=rtpmap:98 VP8/90000"},
        {"a=rtpmap:96 VP8/90000", "a=rtpmap:96 VP8/0"},
        {"a=rtpmap:96 VP8/90000", "a=rtpmap:96 VP8/4294967296"},
        {"a=rtpmap:96 VP8/90000\n", ""},
        {"a=rtpmap:96 VP8/90000", "a=rtpmap:96 VP8/90000\na=rtpmap:96 VP8/90000"},
        {"a=fmtp:103 apt=102", "a=fmtp:103 apt=103"}, {"a=fmtp:103 apt=102", "a=fmtp:103 apt=101"},
        {"a=fmtp:103 apt=102", "a=fmtp:103 apt=97"}, {"a=fmtp:103 apt=102", "a=fmtp:103 apt=102;apt=96"},
        {"a=rtcp-fb:102 nack pli", "a=rtcp-fb:101 nack pli"},
        {"a=ice-ufrag:abcd", "a=ice-ufrag:abc"}, {"a=ice-pwd:abcdefghijklmnopqrstuv1234", "a=ice-pwd:short"},
        {"a=ice-options:trickle renomination", "a=ice-options:  "},
        {"a=setup:actpass", "a=setup:unknown"}, {"a=recvonly", "a=recvonly\na=sendonly"},
        {"a=rtcp-mux", "a=rtcp-mux:yes"}, {"a=rtcp:9 IN IP4 0.0.0.0", "a=rtcp:65536 IN IP4 0.0.0.0"},
        {"a=extmap:4/recvonly", "a=extmap:0/recvonly"}, {"a=extmap:4/recvonly", "a=extmap:257/recvonly"},
        {"a=extmap:4/recvonly", "a=extmap:4/wrong"}, {"a=extmap:5 http:", "a=extmap:4 http:"},
        {fingerprint(), "AA:BB"}, {"c=IN IP4 0.0.0.0", "c=IN IP4 999.1.1.1"},
        {"c=IN IP4 0.0.0.0", "c=IN IP6 0.0.0.0"},
        {"a=candidate:1 1 UDP", "a=candidate:1 0 UDP"},
        {"a=sctp-port:5000", "a=sctp-port:0"}, {"a=sctp-port:5000", "a=sctp-port:50000\na=sctp-port:5000"},
        {"a=max-message-size:262144", "a=max-message-size:18446744073709551616"},
        {"a=max-message-size:262144", "a=max-message-size:01"},
        {"a=max-message-size:262144", "a=max-message-size:0\na=max-message-size:1"},
        {"a=mid:1", "a=mid:1\na=ice-lite"}, {"a=group:BUNDLE 0 1 2", "a=group:BUNDLE 0 1 2\na=mid:bad"}
    };
    for (const auto& mutation : mutations) { auto changed = fixture; replace(changed, mutation.first, mutation.second); bad.push_back(std::move(changed)); }
    auto nul = fixture; nul.insert(nul.begin() + 30, '\0'); bad.push_back(nul);
    auto cr = fixture; cr += '\r'; bad.push_back(cr);
    auto line = fixture; line += "a=x:" + std::string(8192, 'a') + "\n"; bad.push_back(line);
    auto many = fixture; for (unsigned i = 0; i < 4096; ++i) many += "a=x\n"; bad.push_back(many);
    auto sentinel = parsed(bundledAnswer());
    const auto expected = sentinel.toSdp();
    for (const auto& value : bad)
    {
        std::string reason;
        CHECK(!SdpSession::parse(value, sentinel, &reason));
        CHECK(!reason.empty() && sentinel.toSdp() == expected);
    }
    auto wrong = parsed(fixture);
    wrong.media[1].common.attributes.push_back({"rtpmap", "99 fake/90000"});
    CHECK(!wrong.validate()); expectInvalid([&] { wrong.toSdp(); });
    wrong = parsed(fixture); wrong.sessionName = "inject\r\na=ice-lite";
    CHECK(!wrong.validate()); expectInvalid([&] { wrong.toSdp(); });
    wrong = parsed(fixture); wrong.media[0].common.setup = static_cast<SdpSetupRole>(99);
    CHECK(!wrong.validate());
    wrong = parsed(fixture); wrong.media[0].common.extmaps[0].id = 4096;
    wrong.media[0].common.extmaps.push_back({4096, std::nullopt, "urn:alternative", std::nullopt});
    CHECK(wrong.validate()); // 协商扩展范围允许同 ID 的替代项
    wrong = parsed(fixture); wrong.common.extmaps.push_back({1, std::nullopt, "urn:session", std::nullopt});
    CHECK(!wrong.validate());
}
void webRtcValidationTests()
{
    auto offer = parsed(browserOffer());
    offer.media[1].rtcpMux = false;
    CHECK(offer.validate() && !offer.validateWebRtc(SdpDescriptionType::Offer));
    offer = parsed(browserOffer()); offer.media[1].common.icePwd.reset();
    CHECK(offer.validate() && !offer.validateWebRtc(SdpDescriptionType::Offer));
    offer = parsed(browserOffer()); offer.media[1].common.setup = SdpSetupRole::Active;
    CHECK(offer.validate() && !offer.validateWebRtc(SdpDescriptionType::Offer));
    offer = parsed(browserOffer()); offer.media[2].sctpPort.reset();
    CHECK(offer.validate() && !offer.validateWebRtc(SdpDescriptionType::Offer));
    offer = parsed(browserOffer()); offer.media[0].formats.push_back("72");
    CHECK(offer.validate() && !offer.validateWebRtc(SdpDescriptionType::Offer));
    offer = parsed(browserOffer()); offer.media[1].mid.reset(); offer.groups.clear();
    CHECK(offer.validate() && !offer.validateWebRtc(SdpDescriptionType::Offer));
    auto answer = parsed(bundledAnswer()); answer.groups[0].mids.insert(answer.groups[0].mids.begin(), "0");
    CHECK(answer.validate() && !answer.validateWebRtc(SdpDescriptionType::Answer));
    answer = parsed(bundledAnswer()); answer.media[2].port = 0; answer.media[2].bundleOnly = true;
    CHECK(answer.validate() && !answer.validateWebRtc(SdpDescriptionType::Answer));
    answer = parsed(bundledAnswer()); answer.media[2].common.iceUfrag = "wrong";
    CHECK(answer.validate() && !answer.validateWebRtc(SdpDescriptionType::Answer));
    answer = parsed(bundledAnswer()); answer.media[2].common.fingerprints = {{"sha-256", fingerprint('B')}};
    CHECK(answer.validate() && !answer.validateWebRtc(SdpDescriptionType::Answer));
    answer = parsed(bundledAnswer()); answer.media[2].common.fingerprints = answer.media[1].common.fingerprints;
    CHECK(answer.validateWebRtc(SdpDescriptionType::Answer));
    offer = parsed(browserOffer()); offer.media[0].common.fingerprints.push_back({"sha-256", fingerprint('B')});
    CHECK(offer.validateWebRtc(SdpDescriptionType::Offer)); // 多证书可以有相同 hash 算法
    CHECK(parsed(offer.toSdp()).validateWebRtc(SdpDescriptionType::Offer));
    offer.media[0].common.fingerprints.push_back(offer.media[0].common.fingerprints[0]);
    CHECK(!offer.validate());
}

int main()
{
    try
    {
        roundTripTests(); inheritanceAndGenerationTests(); helperTests(); invalidInputTests(); webRtcValidationTests();
        std::cout << "PASS: SDP browser offer/answer, BUNDLE, ICE/DTLS, RTP/RTX, SCTP, inheritance and validation\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
