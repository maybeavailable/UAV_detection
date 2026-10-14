#include"webrtc/srtp_ctl.h"
#include"webrtc/srtp_session.h"

#include<utility>

namespace webrtc
{
SrtpCtl::SrtpCtl() : SrtpCtl(Config{}) {}
SrtpCtl::SrtpCtl(Config config) : session_(std::make_unique<detail::SrtpSession>(std::move(config), false)) {}
SrtpCtl::~SrtpCtl() = default;
SrtpCtl::ErrorCode SrtpCtl::setKeys(const DtlsSrtpKeyMaterial& keys) { return session_->setKeys(keys); }
SrtpCtl::ErrorCode SrtpCtl::protect(Bytes& packet, std::optional<std::uint64_t> generation) { return session_->process(packet, true, generation); }
SrtpCtl::ErrorCode SrtpCtl::unprotect(Bytes& packet, std::optional<std::uint64_t> generation) { return session_->process(packet, false, generation); }
SrtpCtl::ErrorCode SrtpCtl::reset(std::uint64_t generation) { return session_->reset(generation); }
void SrtpCtl::invalidate() { session_->invalidate(); }
void SrtpCtl::close() { session_->close(); }
SrtpCtl::Snapshot SrtpCtl::snapshot() const { return session_->snapshot(); }
} // namespace webrtc
