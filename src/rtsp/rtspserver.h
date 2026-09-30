#pragma once

#include"common/common.h"
#include"common/rtcp_packet.h"

#include<cstdint>
#include<deque>
#include<functional>
#include<unordered_map>
#include<unordered_set>

class RtspSession;
class RtpSource;
class RtpTransport;

// 极简 RTSP 服务器（拉流出口）：
//   - 内部推流源（encode 模块）注册到 registerSource，路径即"rtsp 拉流时对应的地址"；
//   - 客户端 OPTIONS/DESCRIBE/SETUP 完成握手，PLAY 之后服务器把源的 RTP 包
//     直接转发给该客户端（UDP 端口对 或 TCP interleaved 通道）。
class RtspServer
{
public:
    explicit RtspServer(boost::asio::io_context& ioContext);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // 注册媒体源：路径如 /live/uav，DESCRIBE/SETUP 用它查表
    void registerSource(const std::shared_ptr<RtpSource>& source);
    std::shared_ptr<RtpSource> findSource(const std::string& path) const;

    // 客户端 RTCP RR 反馈回调：对所有会话（含之后建立的）生效；
    // 回调在 io_context 线程调用，encode 模块可据此做码率自适应
    void setFeedbackCallback(RtcpFeedbackCallback callback);

    // 开始监听 accept
    void start();

private:
    void do_accept();

    boost::asio::io_context& m_ioContext_;
    boost::asio::ip::tcp::acceptor m_acceptor_;
    // 多会话集合：新连接不再顶掉旧会话
    std::unordered_set<std::shared_ptr<RtspSession>> m_sessions_;
    // 路径 → 媒体源
    std::unordered_map<std::string, std::shared_ptr<RtpSource>> m_sources_;
    // RR 反馈回调（分发给每个会话的每个传输）
    RtcpFeedbackCallback m_feedbackCallback_;
};

class RtspSession : public std::enable_shared_from_this<RtspSession>
{
public:
    // socket 按值持有（旧实现存引用 → 悬垂）；sourceLookup 用于按路径查媒体源
    RtspSession(boost::asio::ip::tcp::socket socket,
                boost::asio::io_context& io,
                std::function<std::shared_ptr<RtpSource>(const std::string&)> sourceLookup);

    void start();

    // 断开/出错时通知服务器把自己从会话集合里移除
    void setCloseCallback(std::function<void()> callback);
    void close();

    // RR 反馈回调：应用到已有与之后 SETUP 的所有传输（io 线程调用）
    void setFeedbackCallback(RtcpFeedbackCallback callback);

private:
    // 解析出来的一条 RTSP 请求
    struct Request
    {
        std::string method;
        std::string uri;
        std::string version;
        std::unordered_map<std::string, std::string> headers;   // key 已转小写
        std::string body;
    };

    void do_read();
    void on_read(std::size_t bytes);
    void on_read_error(const boost::system::error_code& ec);

    void ensure_capacity();                 // 缓冲区满时扩容
    void consume(std::size_t bytes);        // 消费后把剩余数据挪到缓冲头部
    int  parse_request(Request& request);   // 1=完整请求 / 0=数据不够 / -1=协议错误
    void handle_request(const Request& request);

    void handle_options(const Request& request);
    void handle_describe(const Request& request);
    void handle_setup(const Request& request);
    void handle_play(const Request& request);
    void handle_pause(const Request& request);
    void handle_teardown(const Request& request);

    void send_response(int status, const char* reason, const Request& request,
                       std::string extraHeaders = std::string(),
                       std::string body = std::string(),
                       const char* contentType = nullptr);
    void send_error_and_close(int status, const char* reason);
    void queue_write(std::string data);     // 串行化写：多个响应不交错
    void do_write();

    // 写 TCP interleaved 帧（RtpTransport 回调调用，仅在 io 线程）
    void writeInterleaved(int channel, const std::shared_ptr<const std::vector<std::uint8_t>>& data);

    std::string header(const Request& request, const char* name) const;
    std::string local_ip() const;
    std::string rtsp_base() const;
    std::string rtp_info() const;
    bool        session_valid(const Request& request) const;

    boost::asio::ip::tcp::socket m_socket_;
    boost::asio::io_context& m_io_;
    std::function<std::shared_ptr<RtpSource>(const std::string&)> m_sourceLookup_;

    std::vector<char> m_recvBuf_;          // 初始 4096，不够时翻倍扩容
    std::size_t       m_bufOffset_{0};     // 待解析数据长度；数据始终保持在 [0, m_bufOffset_)

    std::deque<std::string> m_writeQueue_;
    bool m_writeInProgress_{false};
    bool m_closed_{false};
    std::function<void()> m_onClose_;
    RtcpFeedbackCallback m_feedbackCallback_;

    // 握手状态
    std::string m_path_;                   // 展示路径，如 /live/uav（即媒体源注册地址）
    std::string m_sessionId_;              // SETUP 后分配，TEARDOWN 后清空
    std::unordered_map<std::string, std::shared_ptr<RtpTransport>> m_tracks_;
    bool m_playing_{false};
};
