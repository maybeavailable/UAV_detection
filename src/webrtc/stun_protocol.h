#pragma once

#include<boost/asio/ip/udp.hpp>

#include<array>
#include<cstddef>
#include<cstdint>
#include<optional>
#include<string>
#include<string_view>
#include<vector>

// RFC 8489 / RFC 5389 STUN 报文及 RFC 8445 ICE Binding 属性。
// 本模块只负责协议；UDP 收发由 UdpSocket 负责，重传/超时/候选对状态由 ICE agent 负责。
namespace webrtc
{
using StunBytes = std::vector<std::uint8_t>;
using StunTransactionId = std::array<std::uint8_t, 12>;
using StunEndpoint = boost::asio::ip::udp::endpoint;
constexpr std::uint32_t StunMagicCookie = 0x2112a442;
constexpr std::size_t StunHeaderSize = 20;
constexpr std::size_t StunMaxMessageSize = StunHeaderSize + 65532;

enum class StunClass : std::uint8_t { Request, Indication, SuccessResponse, ErrorResponse };
enum class StunMethod : std::uint16_t { Binding = 0x001 }; // 可转换其他 12 位扩展 method
enum class StunAttributeType : std::uint16_t
{
    MappedAddress = 0x0001, Username = 0x0006, MessageIntegrity = 0x0008,
    ErrorCode = 0x0009, UnknownAttributes = 0x000a, Realm = 0x0014, Nonce = 0x0015,
    MessageIntegritySha256 = 0x001c, PasswordAlgorithm = 0x001d, Userhash = 0x001e, XorMappedAddress = 0x0020,
    Priority = 0x0024, UseCandidate = 0x0025,
    PasswordAlgorithms = 0x8002, Software = 0x8022, AlternateServer = 0x8023, Fingerprint = 0x8028,
    IceControlled = 0x8029, IceControlling = 0x802a
};
enum class StunIntegrity { Sha1, Sha256, Sha1AndSha256 };
enum class StunPasswordAlgorithm : std::uint16_t { Md5 = 1, Sha256 = 2 }; // 长期 key 派生，与 HMAC 算法独立
enum class StunIceRole { Controlled, Controlling };
struct StunError { std::uint16_t code{0}; std::string reason; };

// 凭据须由调用方完成 Unicode OpaqueString/SASLprep 处理；ICE 的 ASCII 凭据无需处理。
StunBytes stunShortTermKey(std::string_view normalizedPassword);
StunBytes stunLongTermKey(std::string_view normalizedUsername, std::string_view normalizedRealm,
                         std::string_view normalizedPassword,
                         StunPasswordAlgorithm algorithm = StunPasswordAlgorithm::Md5);
StunBytes stunUserhash(std::string_view normalizedUsername, std::string_view normalizedRealm);
StunTransactionId generateStunTransactionId(); // OpenSSL CSPRNG；失败抛 runtime_error

struct StunAttribute
{
    StunAttributeType type{StunAttributeType::Software};
    StunBytes value; // 不含 TLV 头或 padding；未知属性可直接使用 enum 转换构造

    static StunAttribute text(StunAttributeType type, std::string_view text);
    static StunAttribute uint32(StunAttributeType type, std::uint32_t value);
    static StunAttribute uint64(StunAttributeType type, std::uint64_t value);
    static StunAttribute flag(StunAttributeType type);
    static StunAttribute address(StunAttributeType type, const StunEndpoint& endpoint,
                                 const StunTransactionId& transactionId);
    static StunAttribute error(std::uint16_t code, std::string_view reason);
    static StunAttribute unknownAttributes(const std::vector<std::uint16_t>& types);
    static StunAttribute passwordAlgorithm(StunPasswordAlgorithm algorithm);
    static StunAttribute passwordAlgorithms(const std::vector<StunPasswordAlgorithm>& algorithms);

    // 类型或长度不匹配返回 nullopt；字符串不包含 padding。
    std::optional<std::string> asText() const;
    std::optional<std::uint32_t> asUint32() const;
    std::optional<std::uint64_t> asUint64() const;
    std::optional<StunEndpoint> asAddress(const StunTransactionId& transactionId) const;
    std::optional<StunError> asError() const;
    std::optional<std::vector<std::uint16_t>> asUnknownAttributes() const;
    std::optional<StunPasswordAlgorithm> asPasswordAlgorithm() const; // 不支持的扩展算法返回 nullopt
};

struct StunEncodeOptions
{
    std::optional<StunIntegrity> integrity; // nullopt 为不认证，例如公网 STUN 收集请求
    StunBytes key; // 短期凭据为密码字节；长期凭据使用 stunLongTermKey
    bool fingerprint{false}; // ICE 连通性检查必须为 true；与 SDP 的 DTLS 证书指纹不同
};
class StunPacket;

// 可编辑的发送模型。安全属性由 encode 自动附加，不能手工放入 attributes。
struct StunMessage
{
    StunMethod method{StunMethod::Binding};
    StunClass messageClass{StunClass::Request};
    StunTransactionId transactionId{};
    std::vector<StunAttribute> attributes;

    // 全部多字节字段为网络字节序，padding 写 0；无效模型抛 invalid_argument。
    // HMAC/CRC 失败抛 runtime_error；使用双 HMAC 时 SHA1 在前，SHA256 在后。
    StunBytes encode(const StunEncodeOptions& options = {}) const;
    static StunMessage bindingRequest(std::string_view software = {});
    static StunMessage bindingIndication();
    // USERNAME=remoteUfrag:localUfrag；priority 是本地候选的 peer-reflexive 优先级。
    // 发往对端时 encode.key 使用对端的 ice-pwd；仅 controlling 可 useCandidate。
    static StunMessage iceBindingRequest(std::string_view localUfrag, std::string_view remoteUfrag,
                                         std::uint32_t priority, StunIceRole role,
                                         std::uint64_t tieBreaker, bool useCandidate = false);
    // 调用前先认证 request，响应沿用 transactionId；不会自动添加 USERNAME。
    static StunMessage bindingSuccess(const StunPacket& request, const StunEndpoint& mapped);
    static StunMessage bindingError(const StunPacket& request, std::uint16_t code,
                                    std::string_view reason,
                                    const std::vector<std::uint16_t>& unknown = {});
};

// 接收模型拥有原始报文字节，不允许修改，防止原始 HMAC 与可编辑字段脱节。
class StunPacket
{
public:
    // 快速判别头部和长度；不检查 TLV、FINGERPRINT 或 HMAC，不能用来认证。
    static bool looksLikeStun(const void* data, std::size_t size);
    // 完整 UDP 数据报，长度须精确匹配头部；失败不改变 out，reason 不包含凭据。
    // 验证结构/已知属性格式，不自动认证；非零 padding 可接收。
    // 重复普通属性取首项；重复/倒序完整性属性或非末尾 FINGERPRINT 拒绝。
    static bool parse(const void* data, std::size_t size, StunPacket& out, std::string* reason = nullptr);
    static bool parse(const StunBytes& bytes, StunPacket& out, std::string* reason = nullptr);

    StunMethod method() const;
    StunClass messageClass() const;
    const StunTransactionId& transactionId() const;
    const StunBytes& wire() const; // 原始报文，包括被忽略的属性和 padding，仅供诊断
    // 按 RFC 8489 忽略首个完整性属性之后的普通字段；安全属性仍可查询。
    const std::vector<StunAttribute>& attributes() const;
    const StunAttribute* find(StunAttributeType type) const;
    std::optional<StunEndpoint> mappedAddress() const; // 优先 XOR-MAPPED-ADDRESS，兼容 MAPPED-ADDRESS
    std::vector<std::uint16_t> unknownRequiredAttributes() const; // 去重排序，供 420 响应使用
    std::optional<StunIntegrity> preferredIntegrity() const; // SHA256 优先，仅表示存在，不表示认证

    bool verifyFingerprint(std::string* reason = nullptr) const; // 缺失返回 false
    // constant-time 比较。算法由使用场景/协商指定，缺失不降级；Both 必须两个都通过。
    // 默认拒绝截短 SHA256，仅明确允许截短的 STUN usage 可传 true。
    bool verifyIntegrity(const StunBytes& key, StunIntegrity algorithm = StunIntegrity::Sha1,
                         std::string* reason = nullptr, bool allowTruncatedSha256 = false) const;
    // 验证 Binding/USERNAME/PRIORITY/角色/USE-CANDIDATE/未知必选属性及 CRC、HMAC。
    // 在接收端传自己的 localUfrag、localKey 和对端的 remoteUfrag。
    // 不执行角色冲突决策、重放处理或候选对状态变更。
    bool validateIceBindingRequest(std::string_view localUfrag, std::string_view remoteUfrag,
                                   const StunBytes& localKey, std::string* reason = nullptr,
                                   StunIntegrity algorithm = StunIntegrity::Sha1) const;
    // 只匹配 method、transactionId 和响应 class；还须检查来源端点和完整性。
    bool isResponseTo(const StunMessage& request) const;

private:
    StunMethod method_{StunMethod::Binding};
    StunClass class_{StunClass::Request};
    StunTransactionId transactionId_{};
    StunBytes wire_;
    std::vector<StunAttribute> attributes_;
    std::optional<std::size_t> sha1Offset_, sha256Offset_, fingerprintOffset_;
};
} // namespace webrtc
