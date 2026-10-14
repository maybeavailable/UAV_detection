#include"webrtc/srtcp_ctl.h"
#include"webrtc/srtp_session.h"

#include<utility>

namespace webrtc
{
SrtcpCtl::SrtcpCtl() : SrtcpCtl(Config{}) {}
SrtcpCtl::SrtcpCtl(Config config) : session_(std::make_unique<detail::SrtpSession>(std::move(config), true)) {}
SrtcpCtl::~SrtcpCtl() = default;
SrtcpCtl::ErrorCode SrtcpCtl::setKeys(const DtlsSrtpKeyMaterial& keys) { return session_->setKeys(keys); }
SrtcpCtl::ErrorCode SrtcpCtl::protect(Bytes& packet, std::optional<std::uint64_t> generation) { return session_->process(packet, true, generation); }
SrtcpCtl::ErrorCode SrtcpCtl::unprotect(Bytes& packet, std::optional<std::uint64_t> generation) { return session_->process(packet, false, generation); }
SrtcpCtl::ErrorCode SrtcpCtl::reset(std::uint64_t generation) { return session_->reset(generation); }
void SrtcpCtl::invalidate() { session_->invalidate(); }
void SrtcpCtl::close() { session_->close(); }
SrtcpCtl::Snapshot SrtcpCtl::snapshot() const { return session_->snapshot(); }
} // namespace webrtc
