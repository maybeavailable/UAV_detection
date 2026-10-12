#include"shihui_protocol.h"

#include<cstring>

#include"log/log.h"

// ---------------------------------------------------------------------------
// 世辉（Shihui）可见光/热成像相机 VISCA 类协议封装实现
//
// 移植说明：
//   * 串口层用本仓库的 Urt（src/common/urt.h，线程安全）替代原来的
//     NewUartObj/UartObjOpen/UartObjSet/UartObjRead/UartObjWrite/DeleteUartObj；
//     UartObjSet(obj, baud, 8, 'n', 1, 0, 20) 的"打开+配置"合并为
//     Urt::open(dev, baud, 8, 'N', 1, flow=false, exclusive=true)。
//   * 接收线程不再自己起：原来那套 recv_thread_/exit_flag_/recv_proc() 手动线程
//     已由 Urt 的 start_receive()/stop_receive() 接管，本类只提供接收回调
//     onReceive()（在 Urt 的接收线程里执行）做半帧累积与切帧。
//   * 日志用工程内的 log/log.h（UAV_LOG_*）替代 zlogger 的 ZINFO/ZWARN/ZERROR。
//   * 命令/应答的字节编码、帧解析、待确认命令队列等逻辑与原实现保持一致。
//
// 相比参考实现修正的问题（均为原代码里的确凿缺陷）：
//   1. parseResponseCode()：原来用 secondByte & 0xF0 去 switch 0x41/0x51/0x38...
//      永远匹配不上，任何应答都会被判为"未知响应码"。改为用完整第二字节比较。
//   2. getSharpness()：缓冲区 6 字节却判断 resp_len < 7，条件恒真 → 永远返回失败。
//      缓冲区改为 8 字节并按 7 字节帧判断。
//   3. getAutoICRThreshold()：同样是 6 字节缓冲 + resp_len < 7 恒真 → 恒失败。
//      改为 8 字节缓冲、按 5 字节帧判断。
//   4. getFocusSensitivity()：函数末尾缺少 return true，走到末尾是未定义行为。
//   5. getVersion()：8 字节缓冲区却读取 response[8]（越界）。缓冲区放大并做长度检查。
//   6. getZoomMode()：把输出参数 mode（未初始化）写进了查询命令，改为标准查询帧。
//   7. getCurrentZoomPosition()：三段 28 位解包中，第 3 段误写成 <<7（与第 2 段重叠），
//      改为 <<0，与同文件其它 28 位解包（getLensZoomInfo 等）保持一致。
//   8. getHorizontalResolution()/getVerticalResolution()：从 response[1]（Socket 字节）
//      开始解包且最后两段都 <<4；改为从 response[2] 起按 12/8/4/0 解包。
//   9. setZoomFocus()：对焦位置最低半字节重复取了 >>4（应为 >>0）。
//  10. setRoiFocus()：原实现丢了 y1/x2 的低字节、y2 高字节重复写两次；
//      改为 4 个坐标各 2 字节（x1h x1l y1h y1l x2h x2l y2h y2l）。
//      若设备协议文档另有约定，请只改这一处。
//  11. 发送路径的 PendingCommand 超时/完成时从队列中精确摘除，避免残留悬垂指针。
//  12. receiveResponse()/waitForResponse() 头文件里有声明但原实现缺失，本文件补齐。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
ShihuiCore::ShihuiCore(std::string port, uint64_t baud)
    : port_(std::move(port)), baud_(baud), uart_obj_(nullptr),
      auto_reconnect_(true), max_reconnect_attempts_(3), reconnect_delay_ms_(100),
      active_report_callback_(nullptr)
{
    frame_buffer_.reserve(1024);
}

ShihuiCore::~ShihuiCore()
{
    // 关闭串口（Urt 会在 close 里停掉自己的接收线程）
    disconnect();

    // 清理待确认命令队列
    std::lock_guard<std::mutex> lock(command_mutex_);
    while (!pending_commands_queue_.empty())
    {
        delete pending_commands_queue_.front();
        pending_commands_queue_.pop();
    }
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------
bool ShihuiCore::connect()
{
    if (isConnected())
    {
        UAV_LOG_WARN << "ShihuiCore 已连接，先断开再重连\n";
        disconnect();
    }

    uart_obj_ = std::make_unique<Urt>();
    if (uart_obj_ == nullptr)
    {
        UAV_LOG_ERROR << "创建串口对象失败: " << port_ << "\n";
        return false;
    }

    // 打开并配置：8 数据位、无校验、1 停止位、无流控、独占占用
    if (uart_obj_->open(port_, static_cast<int>(baud_), 8, 'N', 1, false, true) != 0)
    {
        UAV_LOG_ERROR << "打开串口失败: " << port_ << " @ " << baud_ << "\n";
        uart_obj_.reset();
        return false;
    }

    // 清空待确认命令队列
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        while (!pending_commands_queue_.empty())
        {
            delete pending_commands_queue_.front();
            pending_commands_queue_.pop();
        }
    }

    // 启动接收：线程由 Urt 内部管理，这里只提供回调
    frame_buffer_.clear();
    if (uart_obj_->start_receive([this](const uint8_t *data, std::size_t len) {
            onReceive(data, len);
        }, 512) != 0)
    {
        UAV_LOG_ERROR << "启动串口接收失败: " << port_ << "\n";
        uart_obj_->close();
        uart_obj_.reset();
        return false;
    }

    UAV_LOG_INFO << "成功连接端口: " << port_ << ", baud=" << baud_ << ", 接收已启动\n";
    return true;
}

bool ShihuiCore::reconnect()
{
    UAV_LOG_INFO << "尝试重新连接设备\n";

    disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_delay_ms_));
    return connect();
}

void ShihuiCore::disconnect()
{
    if (!isConnected())
    {
        return;
    }

    UAV_LOG_INFO << "正在断开连接...\n";

    // 1. 先停接收（Urt 内部线程），避免回调继续访问本对象
    if (uart_obj_)
    {
        uart_obj_->stop_receive();
    }
    command_cv_.notify_all();   // 唤醒可能还在等应答的调用方

    // 2. 关闭并释放串口对象
    if (uart_obj_)
    {
        uart_obj_->close();
        uart_obj_.reset();
    }

    UAV_LOG_INFO << "串口已关闭\n";
}

bool ShihuiCore::isConnected() const
{
    return uart_obj_ != nullptr;
}

// ---------------------------------------------------------------------------
// 连接状态监控
// ---------------------------------------------------------------------------
void ShihuiCore::setAutoReconnect(bool enable)
{
    auto_reconnect_ = enable;
    UAV_LOG_INFO << "自动重连 " << (enable ? "已启用" : "已关闭") << "\n";
}

void ShihuiCore::setMaxReconnectAttempts(int32_t attempts)
{
    max_reconnect_attempts_ = attempts;
    UAV_LOG_INFO << "最大重连次数设置为 " << attempts << "\n";
}

void ShihuiCore::setReconnectDelay(int32_t delay_ms)
{
    reconnect_delay_ms_ = delay_ms;
    UAV_LOG_INFO << "重连延迟设置为 " << delay_ms << " ms\n";
}

bool ShihuiCore::getAutoReconnect() const
{
    return auto_reconnect_;
}

int32_t ShihuiCore::getMaxReconnectAttempts() const
{
    return max_reconnect_attempts_;
}

int32_t ShihuiCore::getReconnectDelay() const
{
    return reconnect_delay_ms_;
}

void ShihuiCore::setShihuiReportCallback(std::function<void(ShihuiReportType, const uint8_t *, int32_t)> callback)
{
    active_report_callback_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// 校验和（VISCA 一般不校验，保留给需要校验的命令使用）
// ---------------------------------------------------------------------------
uint8_t ShihuiCore::calculateChecksum(const uint8_t *data, int32_t length)
{
    if (!data || length <= 0)
    {
        return 0;
    }

    uint8_t sum = 0;
    for (int32_t i = 0; i < length; i++)
    {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

// ---------------------------------------------------------------------------
// 异步接收线程：读串口 → 拼帧 → 匹配待确认命令 / 主动上报
// ---------------------------------------------------------------------------
void ShihuiCore::onReceive(const uint8_t *data, std::size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }

    // 累积到半帧缓冲后切帧（本函数只在 Urt 的接收线程里被调用，无需加锁）
    frame_buffer_.insert(frame_buffer_.end(), data, data + len);
    processFrameBuffer(frame_buffer_);

    // 防止异常数据把缓冲撑爆
    if (frame_buffer_.size() > 1024)
    {
        UAV_LOG_WARN << "帧缓冲区溢出，清空数据\n";
        frame_buffer_.clear();
    }
}

// ---------------------------------------------------------------------------
// 帧缓冲处理：按 帧头(0x90/0xA0) ... 0xFF 切出完整帧
// ---------------------------------------------------------------------------
void ShihuiCore::processFrameBuffer(std::vector<uint8_t> &frame_buffer)
{
    size_t processed_pos = 0;

    while (frame_buffer.size() - processed_pos >= 3)   // 最短帧长
    {
        // 找帧头
        size_t header_pos = processed_pos;
        bool found_header = false;
        for (; header_pos < frame_buffer.size(); header_pos++)
        {
            if (frame_buffer[header_pos] == 0x90 || frame_buffer[header_pos] == 0xA0)
            {
                found_header = true;
                break;
            }
        }

        if (!found_header)
        {
            // 没有帧头：最多保留尾部 3 字节，防止帧头被截断
            if (frame_buffer.size() > processed_pos + 3)
            {
                std::vector<uint8_t> tail(frame_buffer.end() - 3, frame_buffer.end());
                frame_buffer = tail;
            }
            return;
        }

        // 找帧尾
        size_t terminator_pos = header_pos + 1;
        bool found_terminator = false;
        for (; terminator_pos < frame_buffer.size(); terminator_pos++)
        {
            if (frame_buffer[terminator_pos] == 0xFF)
            {
                found_terminator = true;
                break;
            }
        }

        if (!found_terminator)
        {
            // 帧还不完整：丢掉帧头前的垃圾数据，保留候选帧
            if (header_pos > 0)
            {
                frame_buffer.erase(frame_buffer.begin(), frame_buffer.begin() + header_pos);
                processed_pos = 0;
            }
            return;
        }

        const size_t frame_len = terminator_pos - header_pos + 1;
        if (frame_len > 64)
        {
            UAV_LOG_WARN << "发现超长帧，长度 " << frame_len << "，已跳过\n";
            processed_pos = terminator_pos + 1;
            continue;
        }

        processReceivedFrame(frame_buffer.data() + header_pos, static_cast<int32_t>(frame_len));
        processed_pos = terminator_pos + 1;
    }

    if (processed_pos > 0)
    {
        frame_buffer.erase(frame_buffer.begin(), frame_buffer.begin() + processed_pos);
    }
}

// ---------------------------------------------------------------------------
// 单帧处理：主动上报分发 + 待确认命令匹配
// ---------------------------------------------------------------------------
void ShihuiCore::processReceivedFrame(const uint8_t *frame, int32_t frame_len)
{
    if (frame_len < 3 || (frame[0] != 0x90 && frame[0] != 0xA0) || frame[frame_len - 1] != 0xFF)
    {
        UAV_LOG_ERROR << "收到无效格式的帧\n";
        return;
    }

    if (frame_len > 64)
    {
        UAV_LOG_ERROR << "收到过长的帧，长度 " << frame_len << "\n";
        return;
    }

    const uint8_t response_socket = frame[1];

    // ---- 主动上报：连续变焦位置 90 07 04 69 00 00 0q 0q 0q 0q FF ----
    if (frame_len >= 10 && frame[1] == 0x07 && frame[2] == 0x04 && frame[3] == 0x69)
    {
        const uint32_t zoom_position = ((static_cast<uint32_t>(frame[6] & 0x7F) << 21) |
                                        (static_cast<uint32_t>(frame[7] & 0x7F) << 14) |
                                        (static_cast<uint32_t>(frame[8] & 0x7F) << 7) |
                                        static_cast<uint32_t>(frame[9] & 0x7F));
        UAV_LOG_INFO << "收到连续变焦位置主动上报，变焦位置=" << zoom_position << "\n";

        if (active_report_callback_)
        {
            active_report_callback_(SHIHUI_ZOOM_POSITION, frame, frame_len);
        }
        return;
    }

    // ---- 主动上报：聚焦位置 90 08 ... FF ----
    if (frame_len >= 3 && frame[0] == 0x90 && frame[frame_len - 1] == 0xFF)
    {
        if (frame[1] == 0x08)
        {
            UAV_LOG_INFO << "收到聚焦位置主动上报\n";
            if (active_report_callback_)
            {
                active_report_callback_(SHIHUI_FOCUS_POSITION, frame, frame_len);
            }
            return;
        }
        if ((frame[1] & 0xF0) == 0x00)
        {
            UAV_LOG_INFO << "收到其他类型主动上报\n";
            if (active_report_callback_)
            {
                active_report_callback_(SHIHUI_OTHER, frame, frame_len);
            }
            return;
        }
    }

    // ---- 主动上报：上电/初始化完成 90 38 FF ----
    if (response_socket == 0x38 && active_report_callback_)
    {
        UAV_LOG_INFO << "收到相机上电/初始化完成上报 (90 38 FF)\n";
        active_report_callback_(SHIHUI_INITIALIZE_COMPLETE, frame, frame_len);
        return;
    }

    // ---- 命令应答匹配 ----
    std::lock_guard<std::mutex> lock(command_mutex_);

    if (pending_commands_queue_.empty())
    {
        // 队列为空：多为已超时命令的延迟应答，忽略即可
        return;
    }

    PendingCommand *pending_cmd = pending_commands_queue_.front();

    if (response_socket == pending_cmd->expected_final_response_socket)
    {
        // 期望的最终应答
        pending_cmd->response_data.assign(frame, frame + frame_len);
        pending_cmd->response_received = true;
        pending_cmd->cv->notify_one();
    }
    else if (response_socket == RESPONSE_COMMAND_RECEIVED)   // 0x41 中间确认
    {
        UAV_LOG_INFO << "收到中间确认 (0x41)，继续等待最终应答\n";
    }
    else if ((response_socket & 0xF0) == 0x40 || (response_socket & 0xF0) == 0x50)
    {
        // 可能是延迟应答，继续等待真正的应答
        UAV_LOG_INFO << "收到非预期应答 socket=0x" << std::hex << static_cast<int>(response_socket)
                     << std::dec << "，继续等待\n";
    }
    else
    {
        // 明确的错误应答：同样交给等待方，由上层判断
        UAV_LOG_WARN << "收到错误应答 socket=0x" << std::hex << static_cast<int>(response_socket)
                     << std::dec << "\n";
        pending_cmd->response_data.assign(frame, frame + frame_len);
        pending_cmd->response_received = true;
        pending_cmd->cv->notify_one();
    }
}

// ---------------------------------------------------------------------------
// 查询命令：入队 → 发送 → 等最终应答
// ---------------------------------------------------------------------------
bool ShihuiCore::sendQueryCommandAndWaitForResponse(const uint8_t *cmd, int32_t cmd_len, uint8_t *response,
                                                    int32_t &resp_len, int32_t timeout_ms, int32_t max_response_len)
{
    if (cmd == nullptr || cmd_len < 2 || response == nullptr || resp_len <= 0)
    {
        UAV_LOG_ERROR << "无效的命令或响应缓冲\n";
        return false;
    }

    PendingCommand *pending_cmd = new PendingCommand();
    const uint8_t command_socket = cmd[1];

    // 查询命令(0x09)期望 90 50，设置命令(0x01)期望 90 51
    pending_cmd->expected_final_response_socket = (command_socket == 0x09) ? 0x50 : 0x51;
    pending_cmd->response_received = false;
    pending_cmd->cv = &command_cv_;

    {
        std::unique_lock<std::mutex> lock(command_mutex_);
        pending_commands_queue_.push(pending_cmd);
    }

    if (!sendCommand(cmd, cmd_len))
    {
        UAV_LOG_ERROR << "发送查询命令失败\n";
        std::unique_lock<std::mutex> lock(command_mutex_);
        std::queue<PendingCommand *> tmp;
        while (!pending_commands_queue_.empty())
        {
            PendingCommand *c = pending_commands_queue_.front();
            pending_commands_queue_.pop();
            if (c != pending_cmd)
            {
                tmp.push(c);
            }
        }
        pending_commands_queue_.swap(tmp);
        lock.unlock();
        delete pending_cmd;
        return false;
    }

    std::unique_lock<std::mutex> lock(command_mutex_);
    const bool ok = command_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                         [&] { return pending_cmd->response_received; });
    if (!ok)
    {
        UAV_LOG_WARN << "等待应答超时(" << timeout_ms << " ms)\n";
        // 精确摘除本命令，避免残留导致后续应答匹配错位
        std::queue<PendingCommand *> tmp;
        while (!pending_commands_queue_.empty())
        {
            PendingCommand *c = pending_commands_queue_.front();
            pending_commands_queue_.pop();
            if (c != pending_cmd)
            {
                tmp.push(c);
            }
        }
        pending_commands_queue_.swap(tmp);
        lock.unlock();
        delete pending_cmd;
        return false;
    }

    int32_t n = static_cast<int32_t>(pending_cmd->response_data.size());
    if (n > max_response_len)
    {
        UAV_LOG_ERROR << "应答过长(" << n << " > " << max_response_len << ")，已截断\n";
        n = max_response_len;
    }
    if (n > 0)
    {
        memcpy(response, pending_cmd->response_data.data(), static_cast<size_t>(n));
    }
    resp_len = n;

    // 从队列中取出已完成的命令
    if (!pending_commands_queue_.empty() && pending_commands_queue_.front() == pending_cmd)
    {
        pending_commands_queue_.pop();
    }
    lock.unlock();
    delete pending_cmd;
    return true;
}

// ---------------------------------------------------------------------------
// 等待指定 socket 的应答帧（头文件声明但原实现缺失，这里补齐）
//
// 语义：把一个"期望 socket"的等待项入队，然后等它被应答。
// 用途：需要等某个特定应答（如主动上报被触发后的确认帧）时使用。
// 注意：调用前应确保队列里没有其它在途命令，否则队列是 FIFO 的，会等错对象。
// ---------------------------------------------------------------------------
bool ShihuiCore::waitForResponse(uint8_t expected_socket, std::vector<uint8_t> &response, int32_t timeout_ms)
{
    PendingCommand *pending_cmd = new PendingCommand();
    pending_cmd->expected_final_response_socket = expected_socket;
    pending_cmd->response_received = false;
    pending_cmd->cv = &command_cv_;

    {
        std::unique_lock<std::mutex> lock(command_mutex_);
        pending_commands_queue_.push(pending_cmd);
    }

    std::unique_lock<std::mutex> lock(command_mutex_);
    const bool ok = command_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                         [&] { return pending_cmd->response_received; });
    if (!ok)
    {
        std::queue<PendingCommand *> tmp;
        while (!pending_commands_queue_.empty())
        {
            PendingCommand *c = pending_commands_queue_.front();
            pending_commands_queue_.pop();
            if (c != pending_cmd)
            {
                tmp.push(c);
            }
        }
        pending_commands_queue_.swap(tmp);
        lock.unlock();
        delete pending_cmd;
        return false;
    }

    response = pending_cmd->response_data;
    if (!pending_commands_queue_.empty() && pending_commands_queue_.front() == pending_cmd)
    {
        pending_commands_queue_.pop();
    }
    lock.unlock();
    delete pending_cmd;
    return true;
}

// ---------------------------------------------------------------------------
// 取回当前在途命令的应答帧（头文件声明但原实现缺失，这里补齐）
//
// length：入参为调用方缓冲区容量，出参为实际拷贝长度。
// 返回 false 表示当前没有在途命令、或等待超时。
// ---------------------------------------------------------------------------
bool ShihuiCore::receiveResponse(uint8_t *response, int32_t &length)
{
    if (response == nullptr || length <= 0)
    {
        UAV_LOG_ERROR << "无效的响应缓冲\n";
        return false;
    }

    PendingCommand *pending_cmd = nullptr;
    {
        std::unique_lock<std::mutex> lock(command_mutex_);
        if (pending_commands_queue_.empty())
        {
            UAV_LOG_WARN << "当前没有在途命令，无法接收应答\n";
            return false;
        }
        pending_cmd = pending_commands_queue_.front();
    }

    std::unique_lock<std::mutex> lock(command_mutex_);
    const bool ok = command_cv_.wait_for(lock, std::chrono::milliseconds(3000),
                                         [&] { return pending_cmd->response_received; });
    if (!ok)
    {
        UAV_LOG_WARN << "接收应答超时\n";
        return false;
    }

    int32_t n = static_cast<int32_t>(pending_cmd->response_data.size());
    if (n > length)
    {
        n = length;
    }
    memcpy(response, pending_cmd->response_data.data(), static_cast<size_t>(n));
    length = n;

    if (!pending_commands_queue_.empty() && pending_commands_queue_.front() == pending_cmd)
    {
        pending_commands_queue_.pop();
    }
    lock.unlock();
    delete pending_cmd;
    return true;
}

// ---------------------------------------------------------------------------
// 底层发送 / 应答码解析
// ---------------------------------------------------------------------------
bool ShihuiCore::sendCommand(const uint8_t *data, int32_t length)
{
    if (!isConnected())
    {
        UAV_LOG_ERROR << "串口未连接\n";
        return false;
    }
    if (data == nullptr || length <= 0)
    {
        return false;
    }

    // Urt::write 内部已加写锁，这里直接调用即可
    const int32_t bytes_written = uart_obj_->write(data, static_cast<size_t>(length));
    if (bytes_written < 0)
    {
        UAV_LOG_ERROR << "命令写入失败\n";
        return false;
    }
    if (bytes_written != length)
    {
        UAV_LOG_WARN << "命令写入不完整: " << bytes_written << "/" << length << " 字节\n";
        return false;
    }

    return true;
}

bool ShihuiCore::parseResponseCode(const uint8_t *response, int32_t length,
                                   ResponseCode &code)
{
    if (!response || length < 3)
    {
        UAV_LOG_ERROR << "无效的应答数据用于解析\n";
        return false;
    }
    if (response[0] != 0x90 || response[length - 1] != 0xFF)
    {
        UAV_LOG_ERROR << "应答格式无效\n";
        return false;
    }

    // 注意：这里必须用完整的第二字节比较（原实现先 & 0xF0 再比 0x41，永远匹配不上）
    const uint8_t response_code = response[1];

    switch (response_code)
    {
        case RESPONSE_COMMAND_RECEIVED:   code = RESPONSE_COMMAND_RECEIVED;   break;
        case RESPONSE_COMMAND_COMPLETED:  code = RESPONSE_COMMAND_COMPLETED;  break;
        case RESPONSE_POWER_ON:           code = RESPONSE_POWER_ON;           break;
        case RESPONSE_LENGTH_ERROR:       code = RESPONSE_LENGTH_ERROR;       break;
        case RESPONSE_SYNTAX_ERROR:       code = RESPONSE_SYNTAX_ERROR;       break;
        case RESPONSE_BUFFER_FULL:        code = RESPONSE_BUFFER_FULL;        break;
        case RESPONSE_COMMAND_CANCELLED:  code = RESPONSE_COMMAND_CANCELLED;  break;
        case RESPONSE_NO_SOCKET:          code = RESPONSE_NO_SOCKET;          break;
        case RESPONSE_NOT_EXECUTABLE:     code = RESPONSE_NOT_EXECUTABLE;     break;
        default:
            UAV_LOG_WARN << "未知的应答码: 0x" << std::hex << static_cast<int>(response_code)
                         << std::dec << "\n";
            return false;
    }

    if (code == RESPONSE_COMMAND_RECEIVED || code == RESPONSE_COMMAND_COMPLETED)
    {
        return true;
    }

    UAV_LOG_ERROR << "命令执行失败，应答码: 0x" << std::hex << static_cast<int>(response_code)
                  << std::dec << "\n";
    return false;
}

// ---------------------------------------------------------------------------
// 图像参数设置
// ---------------------------------------------------------------------------
bool ShihuiCore::setImageProperty(ImagePropertyType type, uint8_t value)
{
    UAV_LOG_INFO << "设置图像属性 type=0x" << std::hex << static_cast<int>(type)
                 << " value=0x" << static_cast<int>(value) << std::dec << "\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x04,
        0x63,
        static_cast<uint8_t>(type),
        static_cast<uint8_t>((value >> 4) & 0x0F),
        static_cast<uint8_t>(value & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setWhiteBalance(WhiteBalanceMode mode)
{
    UAV_LOG_INFO << "设置白平衡模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x14, 0x50, static_cast<uint8_t>(mode),
        0x00, 0x00, 0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setDayNightSwitchDelay(uint8_t type, uint8_t delay)
{
    UAV_LOG_INFO << "设置日夜切换延迟 type=0x" << std::hex << static_cast<int>(type)
                 << " delay=" << std::dec << static_cast<int>(delay) << "s\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x04,
        0x51,
        type,
        static_cast<uint8_t>((delay >> 4) & 0x0F),
        static_cast<uint8_t>(delay & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setRBgain(uint8_t rGain, uint8_t bGain)
{
    UAV_LOG_INFO << "设置手动白平衡增益 R=" << static_cast<int>(rGain)
                 << " B=" << static_cast<int>(bGain) << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x14, 0x50, 0x04, rGain, bGain, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setGamma(uint8_t value)
{
    UAV_LOG_INFO << "设置伽马值: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x5B, value, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setSharpness(uint8_t value)
{
    UAV_LOG_INFO << "设置锐化等级: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x04,
        0x42,
        0x00,
        0x00,
        static_cast<uint8_t>((value >> 4) & 0x0F),
        static_cast<uint8_t>(value & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setImageStyle(ImageStyle style)
{
    UAV_LOG_INFO << "设置图像风格: 0x" << std::hex << static_cast<int>(style) << std::dec << "\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x60, static_cast<uint8_t>(style), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setFormat(uint8_t format)
{
    UAV_LOG_INFO << "设置尾板视频输入方式: 0x" << std::hex << static_cast<int>(format)
                 << std::dec << " (0=LVDS/BT1120, 1=MIPI)\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x23, 0x00, 0x00, 0x00, 0x00, format, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

// 分辨率命令按协议文档的 "0p 0q 0r 0s" 编码（4 个半字节）。
// 注意：参考实现用了 7 个半字节，与它自己的注释不符；若实测设备按 7 个半字节解析，
// 改回 81 01 04 23 01 <7 个半字节> FF 即可。
bool ShihuiCore::setHorizontalResolution(uint32_t resolution)
{
    UAV_LOG_INFO << "设置水平分辨率: " << resolution << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x23, 0x01,
        static_cast<uint8_t>((resolution >> 12) & 0x0F),
        static_cast<uint8_t>((resolution >> 8) & 0x0F),
        static_cast<uint8_t>((resolution >> 4) & 0x0F),
        static_cast<uint8_t>(resolution & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setVerticalResolution(uint32_t resolution)
{
    UAV_LOG_INFO << "设置垂直分辨率: " << resolution << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x23, 0x02,
        static_cast<uint8_t>((resolution >> 12) & 0x0F),
        static_cast<uint8_t>((resolution >> 8) & 0x0F),
        static_cast<uint8_t>((resolution >> 4) & 0x0F),
        static_cast<uint8_t>(resolution & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setNoiseReduction2D(uint8_t level)
{
    UAV_LOG_INFO << "设置 2D 降噪等级: " << static_cast<int>(level) << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x53, level, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setNoiseReduction3D(uint8_t level)
{
    UAV_LOG_INFO << "设置 3D 降噪等级: " << static_cast<int>(level) << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x51, 0x30, level, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setAINR(uint8_t mode, uint8_t level)
{
    UAV_LOG_INFO << "设置高级降噪 mode=0x" << std::hex << static_cast<int>(mode)
                 << " level=0x" << static_cast<int>(level) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x14, 0x64, 0x02, mode, level, 0x00, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setPseudoColor(uint8_t color)
{
    UAV_LOG_INFO << "设置伪彩色模式: 0x" << std::hex << static_cast<int>(color) << std::dec << "\n";
    uint8_t cmd[] = {0x84, 0x01, 0x04, 0x51, 0x32, color, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::freezeImage(bool freeze)
{
    UAV_LOG_INFO << "图像" << (freeze ? "冻结" : "解冻") << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x62, static_cast<uint8_t>(freeze ? 0x02 : 0x03), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setHighlightCompensation(bool on)
{
    UAV_LOG_INFO << "强光抑制" << (on ? "开启" : "关闭") << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x33, static_cast<uint8_t>(on ? 0x02 : 0x03), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setWDRLevel(uint8_t mode, uint8_t level)
{
    UAV_LOG_INFO << "设置宽动态 mode=0x" << std::hex << static_cast<int>(mode)
                 << " level=" << std::dec << static_cast<int>(level) << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x14, 0x51, mode, level, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 曝光控制
// ---------------------------------------------------------------------------
bool ShihuiCore::setExposureMode(ExposureMode mode)
{
    UAV_LOG_INFO << "设置曝光模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x39, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setExposure(uint8_t mode, uint8_t apertureMin,
                             uint8_t apertureMax, uint32_t shutterMin,
                             uint32_t shutterMax, uint8_t gainMin,
                             uint8_t gainMax)
{
    UAV_LOG_INFO << "设置曝光参数 mode=0x" << std::hex << static_cast<int>(mode)
                 << " 光圈[" << static_cast<int>(apertureMin) << ","
                 << static_cast<int>(apertureMax) << "] 快门[" << std::dec << shutterMin << ","
                 << shutterMax << "] 增益[" << static_cast<int>(gainMin) << ","
                 << static_cast<int>(gainMax) << "]\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x14,
        0x58,
        mode,
        apertureMin,
        apertureMax,
        static_cast<uint8_t>((shutterMin >> 16) & 0x7F),
        static_cast<uint8_t>((shutterMin >> 8) & 0x7F),
        static_cast<uint8_t>(shutterMin & 0x7F),
        static_cast<uint8_t>((shutterMax >> 16) & 0x7F),
        static_cast<uint8_t>((shutterMax >> 8) & 0x7F),
        gainMin,
        gainMax,
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setExposureControl(ExposureControlType type, uint8_t value)
{
    UAV_LOG_INFO << "设置曝光控制 type=0x" << std::hex << static_cast<int>(type)
                 << " value=0x" << static_cast<int>(value) << std::dec << "\n";
    uint8_t cmd[9] = {0};
    cmd[0] = 0x81;
    cmd[1] = 0x01;
    cmd[2] = 0x04;

    switch (type)
    {
        case APERTURE_UP:
        case APERTURE_DOWN:
        case APERTURE_RESET:
            cmd[3] = 0x0B;
            cmd[4] = static_cast<uint8_t>(type & 0xFF);
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case APERTURE_DIRECT:
            cmd[3] = 0x4B;
            break;
        case GAIN_UP:
        case GAIN_DOWN:
        case GAIN_RESET:
            cmd[3] = 0x0C;
            cmd[4] = static_cast<uint8_t>(type & 0xFF);
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case GAIN_DIRECT:
            cmd[3] = 0x4C;
            break;
        case SHUTTER_UP:
        case SHUTTER_DOWN:
        case SHUTTER_RESET:
            cmd[3] = 0x0A;
            cmd[4] = static_cast<uint8_t>(type & 0xFF);
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case SHUTTER_DIRECT:
            cmd[3] = 0x4A;
            break;
        case EXP_COMP_UP:
        case EXP_COMP_DOWN:
        case EXP_COMP_RESET:
            cmd[3] = 0x0E;
            cmd[4] = static_cast<uint8_t>(type & 0xFF);
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case EXP_COMP_DIRECT:
            cmd[3] = 0x4E;
            break;
        default:
            UAV_LOG_WARN << "未知的曝光控制类型\n";
            return false;
    }

    // xxx_DIRECT：直接设置数值
    cmd[4] = 0x00;
    cmd[5] = 0x00;
    cmd[6] = static_cast<uint8_t>((value >> 4) & 0x0F);
    cmd[7] = static_cast<uint8_t>(value & 0x0F);
    cmd[8] = 0xFF;
    return sendCommand(cmd, 9);
}

bool ShihuiCore::setSharpnessControl(SharpnessControlType type)
{
    UAV_LOG_INFO << "设置锐化控制: 0x" << std::hex << static_cast<int>(type) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x02, static_cast<uint8_t>(type & 0xFF), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setAntiFlickerMode(AntiFlickerMode mode)
{
    UAV_LOG_INFO << "设置防闪烁模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x14, 0x4F, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 变焦控制
// ---------------------------------------------------------------------------
bool ShihuiCore::setZoomControl(ZoomControlType type, uint16_t position)
{
    UAV_LOG_INFO << "变焦控制 type=0x" << std::hex << static_cast<int>(type)
                 << " position=0x" << position << std::dec << "\n";
    uint8_t cmd[9] = {0};
    cmd[0] = 0x81;
    cmd[1] = 0x01;
    cmd[2] = 0x04;

    if (type == ZOOM_DIRECT)
    {
        cmd[3] = 0x47;
        cmd[4] = static_cast<uint8_t>((position >> 12) & 0x0F);
        cmd[5] = static_cast<uint8_t>((position >> 8) & 0x0F);
        cmd[6] = static_cast<uint8_t>((position >> 4) & 0x0F);
        cmd[7] = static_cast<uint8_t>(position & 0x0F);
        cmd[8] = 0xFF;
        return sendCommand(cmd, 9);
    }

    // ZOOM_STOP / ZOOM_TELE / ZOOM_WIDE：协议里 07 之后的字节是速度档位，
    // 由调用方经 position 传入（CameraNode/RGBCamera 即如此，如 0x21~0x27）；
    // position 传 0 时退化为枚举自带的默认档位 ZOOM_TELE=0x27 / ZOOM_WIDE=0x37
    cmd[3] = 0x07;
    cmd[4] = static_cast<uint8_t>((position != 0) ? (position & 0xFF) : (type & 0xFF));
    cmd[5] = 0xFF;
    return sendCommand(cmd, 6);
}

bool ShihuiCore::setZoomMode(ZoomMode mode)
{
    UAV_LOG_INFO << "设置变倍模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x06, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setCustomZoom(uint32_t position, bool isThermal)
{
    UAV_LOG_INFO << "设置自定义变焦(" << (isThermal ? "热成像" : "可见光") << "): " << position << "\n";
    uint8_t cmd[9] = {0};
    cmd[0] = static_cast<uint8_t>(isThermal ? 0x84 : 0x81);
    cmd[1] = 0x01;
    cmd[2] = 0x04;
    cmd[3] = 0x47;
    cmd[4] = 0x50;
    cmd[5] = static_cast<uint8_t>((position >> 14) & 0x7F);
    cmd[6] = static_cast<uint8_t>((position >> 7) & 0x7F);
    cmd[7] = static_cast<uint8_t>(position & 0x7F);
    cmd[8] = 0xFF;
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setOriginalZoom(uint32_t position, bool isThermal)
{
    UAV_LOG_INFO << "设置原始变倍比(" << (isThermal ? "热成像" : "可见光") << "): " << position << "\n";
    uint8_t cmd[9] = {0};
    cmd[0] = static_cast<uint8_t>(isThermal ? 0x84 : 0x81);
    cmd[1] = 0x01;
    cmd[2] = 0x04;
    cmd[3] = 0x47;
    cmd[4] = 0x51;
    cmd[5] = static_cast<uint8_t>((position >> 14) & 0x7F);
    cmd[6] = static_cast<uint8_t>((position >> 7) & 0x7F);
    cmd[7] = static_cast<uint8_t>(position & 0x7F);
    cmd[8] = 0xFF;
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setDirectZoom(uint32_t position, bool isThermal)
{
    UAV_LOG_INFO << "直接变焦(" << (isThermal ? "热成像" : "可见光") << "): " << position << "\n";
    uint8_t cmd[9] = {0};
    cmd[0] = static_cast<uint8_t>(isThermal ? 0x84 : 0x81);
    cmd[1] = 0x01;
    cmd[2] = 0x04;
    cmd[3] = 0x47;
    cmd[4] = static_cast<uint8_t>((position >> 12) & 0x0F);
    cmd[5] = static_cast<uint8_t>((position >> 8) & 0x0F);
    cmd[6] = static_cast<uint8_t>((position >> 4) & 0x0F);
    cmd[7] = static_cast<uint8_t>(position & 0x0F);
    cmd[8] = 0xFF;
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setZoomFocus(uint32_t zoomPosition, uint32_t focusPosition, bool isThermal)
{
    UAV_LOG_INFO << "变焦对焦联动(" << (isThermal ? "热成像" : "可见光") << ") zoom="
                 << zoomPosition << " focus=" << focusPosition << "\n";
    // 81/84 01 04 47 <zoom 4 半字节> <focus 4 半字节> FF
    uint8_t cmd[13] = {0};
    cmd[0] = static_cast<uint8_t>(isThermal ? 0x84 : 0x81);
    cmd[1] = 0x01;
    cmd[2] = 0x04;
    cmd[3] = 0x47;
    cmd[4] = static_cast<uint8_t>((zoomPosition >> 12) & 0x0F);
    cmd[5] = static_cast<uint8_t>((zoomPosition >> 8) & 0x0F);
    cmd[6] = static_cast<uint8_t>((zoomPosition >> 4) & 0x0F);
    cmd[7] = static_cast<uint8_t>(zoomPosition & 0x0F);
    cmd[8] = static_cast<uint8_t>((focusPosition >> 12) & 0x0F);
    cmd[9] = static_cast<uint8_t>((focusPosition >> 8) & 0x0F);
    cmd[10] = static_cast<uint8_t>((focusPosition >> 4) & 0x0F);
    cmd[11] = static_cast<uint8_t>(focusPosition & 0x0F);   // 原实现这里误写成 >>4
    cmd[12] = 0xFF;
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setThermalDZoom(uint8_t position)
{
    UAV_LOG_INFO << "设置热成像数字变焦: 0x" << std::hex << static_cast<int>(position)
                 << std::dec << "\n";
    if (position < 0x10 || position > 0x80)
    {
        UAV_LOG_ERROR << "热成像数字变焦超出范围(0x10~0x80)\n";
        return false;
    }

    uint8_t cmd[] = {
        0x84, 0x01, 0x04, 0x46, 0x00, 0x00,
        static_cast<uint8_t>((position >> 4) & 0x0F),
        static_cast<uint8_t>(position & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 聚焦控制
// ---------------------------------------------------------------------------
bool ShihuiCore::setFocusControl(FocusControlType type, uint16_t position)
{
    UAV_LOG_INFO << "聚焦控制 type=0x" << std::hex << static_cast<int>(type)
                 << " position=0x" << position << std::dec << "\n";
    uint8_t cmd[9] = {0};
    cmd[0] = 0x81;
    cmd[1] = 0x01;
    cmd[2] = 0x04;

    switch (type)
    {
        case FOCUS_STOP:
        case FOCUS_FAR:
        case FOCUS_NEAR:
            // 同变焦：08 之后的字节是速度档位，由 position 传入，0 时用枚举默认档位
            cmd[3] = 0x08;
            cmd[4] = static_cast<uint8_t>((position != 0) ? (position & 0xFF) : (type & 0xFF));
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case FOCUS_AUTO_TRIGGER:
        case FOCUS_MANUAL_TRIGGER:
            cmd[3] = 0x38;
            cmd[4] = static_cast<uint8_t>(type & 0xFF);
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case FOCUS_ONE_PUSH:
            cmd[3] = 0x18;
            cmd[4] = 0x01;
            cmd[5] = 0xFF;
            return sendCommand(cmd, 6);
        case FOCUS_DIRECT:
            cmd[3] = 0x48;
            cmd[4] = static_cast<uint8_t>((position >> 12) & 0x0F);
            cmd[5] = static_cast<uint8_t>((position >> 8) & 0x0F);
            cmd[6] = static_cast<uint8_t>((position >> 4) & 0x0F);
            cmd[7] = static_cast<uint8_t>(position & 0x0F);
            cmd[8] = 0xFF;
            return sendCommand(cmd, 9);
        default:
            UAV_LOG_WARN << "未知的聚焦控制类型\n";
            return false;
    }
}

bool ShihuiCore::setFocusSensitivity(FocusSensitivity sensitivity)
{
    UAV_LOG_INFO << "设置聚焦灵敏度: 0x" << std::hex << static_cast<int>(sensitivity)
                 << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x14, 0x57, 0x10, static_cast<uint8_t>(sensitivity), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setFocusLimit(uint16_t limit)
{
    UAV_LOG_INFO << "设置最近聚焦距离: 0x" << std::hex << limit << std::dec << "\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x04,
        0x28,
        static_cast<uint8_t>((limit >> 12) & 0x0F),
        static_cast<uint8_t>((limit >> 8) & 0x0F),
        static_cast<uint8_t>((limit >> 4) & 0x0F),
        static_cast<uint8_t>(limit & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

// ROI 区域对焦：4 个坐标各 2 字节（x1h x1l y1h y1l x2h x2l y2h y2l）。
// 原实现丢了 y1/x2 低字节、y2 高字节重复写两次；若协议文档另有定义，只改这里。
bool ShihuiCore::setRoiFocus(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    UAV_LOG_INFO << "设置 ROI 对焦区域 (" << x1 << "," << y1 << ")-(" << x2 << "," << y2 << ")\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x04,
        0x58,
        0x31,
        static_cast<uint8_t>((x1 >> 8) & 0xFF),
        static_cast<uint8_t>(x1 & 0xFF),
        static_cast<uint8_t>((y1 >> 8) & 0xFF),
        static_cast<uint8_t>(y1 & 0xFF),
        static_cast<uint8_t>((x2 >> 8) & 0xFF),
        static_cast<uint8_t>(x2 & 0xFF),
        static_cast<uint8_t>((y2 >> 8) & 0xFF),
        static_cast<uint8_t>(y2 & 0xFF),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setAutoFocusMode(AutoFocusMode mode)
{
    UAV_LOG_INFO << "设置自动对焦模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x57, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 功能设置
// ---------------------------------------------------------------------------
bool ShihuiCore::setICR(ICRMode mode)
{
    UAV_LOG_INFO << "设置 ICR: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x01, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setAutoICR(AutoICRMode mode)
{
    UAV_LOG_INFO << "设置自动 ICR: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x51, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setAutoICRThreshold(uint8_t threshold)
{
    UAV_LOG_INFO << "设置自动 ICR 切换灵敏度: 0x" << std::hex << static_cast<int>(threshold)
                 << std::dec << "\n";
    uint8_t cmd[] = {
        0x81,
        0x01,
        0x04,
        0x21,
        0x00,
        0x00,
        static_cast<uint8_t>((threshold >> 4) & 0x0F),
        static_cast<uint8_t>(threshold & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setDefogLevel(DefogMode mode, uint8_t level)
{
    if (level > 100)
    {
        level = 100;
    }
    UAV_LOG_INFO << "设置除雾 mode=0x" << std::hex << static_cast<int>(mode)
                 << " level=" << std::dec << static_cast<int>(level) << "\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x55, static_cast<uint8_t>(mode), level, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setHeatHaze(HeatHazeMode mode, HeatHazeLevel level)
{
    UAV_LOG_INFO << "设置热浪减少 mode=0x" << std::hex << static_cast<int>(mode)
                 << " level=0x" << static_cast<int>(level) << std::dec << "\n";
    uint8_t cmd[] = {
        0x82, 0x01, 0x14, 0x56, static_cast<uint8_t>(mode),
        static_cast<uint8_t>(level), 0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setStabilizerMode(StabilizerMode mode)
{
    UAV_LOG_INFO << "设置防抖模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x53, static_cast<uint8_t>(mode), 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setOSDDisplay(uint8_t channel, OSDRegion region,
                               OSDFunction function, bool enable)
{
    UAV_LOG_INFO << "设置 OSD 显示 channel=0x" << std::hex << static_cast<int>(channel)
                 << " region=0x" << static_cast<int>(region)
                 << " function=0x" << static_cast<int>(function)
                 << " enable=" << std::dec << (enable ? 1 : 0) << "\n";
    uint8_t cmd[] = {
        0x82,
        0x01,
        0x14,
        0x12,
        channel,
        static_cast<uint8_t>(region),
        static_cast<uint8_t>(function),
        static_cast<uint8_t>(enable ? 0x01 : 0x00),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setCustomOSD(const char *text)
{
    if (text == nullptr)
    {
        return false;
    }

    size_t text_len = strlen(text);
    if (text_len > 57)
    {
        text_len = 57;      // 协议最长 57 个 ASCII 字符
    }

    UAV_LOG_INFO << "设置自定义 OSD 文本: " << text << "\n";

    std::vector<uint8_t> cmd;
    cmd.reserve(6 + text_len);
    cmd.push_back(0x82);
    cmd.push_back(0x01);
    cmd.push_back(0x14);
    cmd.push_back(0x01);
    cmd.push_back(0x11);
    for (size_t i = 0; i < text_len; i++)
    {
        cmd.push_back(static_cast<uint8_t>(text[i]));
    }
    cmd.push_back(0xFF);

    return sendCommand(cmd.data(), static_cast<int32_t>(cmd.size()));
}

bool ShihuiCore::setVideoFormat(VideoFormat visibleFormat,
                                uint8_t thermalFormatHigh,
                                uint8_t thermalFormatLow)
{
    UAV_LOG_INFO << "设置视频格式 可见光=0x" << std::hex << static_cast<int>(visibleFormat)
                 << " 热成像=0x" << static_cast<int>(thermalFormatHigh)
                 << static_cast<int>(thermalFormatLow) << std::dec << "\n";

    uint8_t cmd1[] = {
        0x81, 0x01, 0x04, 0x24, 0x72, 0x00, static_cast<uint8_t>(visibleFormat), 0xFF
    };
    if (!sendCommand(cmd1, sizeof(cmd1)))
    {
        return false;
    }

    if (thermalFormatHigh > 0 || thermalFormatLow > 0)
    {
        uint8_t cmd2[] = {
            0x81, 0x01, 0x04, 0x24, 0x72, thermalFormatHigh, thermalFormatLow, 0xFF
        };
        if (!sendCommand(cmd2, sizeof(cmd2)))
        {
            return false;
        }
    }
    return true;
}

bool ShihuiCore::setRegisterValue(uint8_t regNum, uint8_t value)
{
    UAV_LOG_INFO << "写寄存器 0x" << std::hex << static_cast<int>(regNum)
                 << " = 0x" << static_cast<int>(value) << std::dec << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24,
        regNum,
        static_cast<uint8_t>((value >> 4) & 0x0F),
        static_cast<uint8_t>(value & 0x0F),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setLockMode(bool lock)
{
    UAV_LOG_INFO << "曝光/对焦" << (lock ? "锁定" : "解锁") << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24,
        0x0F, 0x0A, 0x00, 0x00, static_cast<uint8_t>(lock ? 0x00 : 0x03),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setImageFlip(uint8_t mode)
{
    UAV_LOG_INFO << "设置图像翻转: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x01, 0x14, 0x54, mode, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 快照与录像
// ---------------------------------------------------------------------------
bool ShihuiCore::setSnap(uint8_t count, uint8_t delay)
{
    UAV_LOG_INFO << "设置快照 count=" << static_cast<int>(count)
                 << " delay=" << static_cast<int>(delay) << "\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x41, count, delay, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setSnapOnePush()
{
    UAV_LOG_INFO << "触发一次截图\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x24, 0x0A, 0x00, 0x00, 0x01, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setRecord(bool start)
{
    UAV_LOG_INFO << (start ? "开始录像" : "停止录像") << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24,
        0x0A, 0x00, 0x00, static_cast<uint8_t>(start ? 0x02 : 0x03),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// OSD 功能
// ---------------------------------------------------------------------------
bool ShihuiCore::setThermalGPSOSD(bool enable)
{
    UAV_LOG_INFO << "热成像 GPS 叠加" << (enable ? "开启" : "关闭") << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24,
        0x0A, 0x00, 0x00, static_cast<uint8_t>(enable ? 0x04 : 0x05),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setZoomOSD(bool enable)
{
    UAV_LOG_INFO << "变倍 OSD" << (enable ? "开启" : "关闭") << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24,
        0x0A, 0x00, 0x00, static_cast<uint8_t>(enable ? 0x06 : 0x07),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setThermalTimeTitleOSD(bool enable)
{
    UAV_LOG_INFO << "热成像时间标题叠加" << (enable ? "开启" : "关闭") << "\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24,
        0x0A, 0x00, 0x00, static_cast<uint8_t>(enable ? 0x0A : 0x0B),
        0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 系统功能
// ---------------------------------------------------------------------------
bool ShihuiCore::setInitialize()
{
    UAV_LOG_INFO << "镜头初始化\n";
    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x19, 0x01, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setImageInitialize()
{
    UAV_LOG_INFO << "图像参数恢复默认\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x6F, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setPowerOff()
{
    UAV_LOG_INFO << "相机关机\n";
    uint8_t cmd[] = {0x82, 0x01, 0x14, 0x00, 0x03, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::setBaudRate(BaudRate rate)
{
    UAV_LOG_INFO << "修改相机串口波特率: 0x" << std::hex << static_cast<int>(rate)
                 << std::dec << " (0=9600,1=19200,2=38400,3=115200)\n";
    uint8_t cmd[] = {
        0x81, 0x01, 0x04, 0x24, 0x00, 0x00, static_cast<uint8_t>(rate), 0xFF
    };
    return sendCommand(cmd, sizeof(cmd));
}

// ---------------------------------------------------------------------------
// 查询命令
//
// 统一模式：组查询帧 → sendQueryCommandAndWaitForResponse（内部等 90 5x 最终应答）
//          → 校验帧头帧尾 → 解出字段
// ---------------------------------------------------------------------------

// 查询应答的统一校验：帧头必须是 0x90、帧尾 0xFF，且长度不小于 min_len
static bool checkShihuiResponse(const uint8_t *response, int32_t resp_len, int32_t min_len,
                                const char *what)
{
    if (resp_len < min_len)
    {
        UAV_LOG_ERROR << what << " 应答长度不足: " << resp_len << " < " << min_len << "\n";
        return false;
    }
    if (response[0] != 0x90 || response[resp_len - 1] != 0xFF)
    {
        UAV_LOG_ERROR << what << " 应答帧格式无效\n";
        return false;
    }
    return true;
}

bool ShihuiCore::getImageProperty(ImagePropertyType type, uint8_t &value)
{
    UAV_LOG_INFO << "查询图像属性 type=0x" << std::hex << static_cast<int>(type)
                 << std::dec << "\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x63, static_cast<uint8_t>(type), 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询图像属性失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询图像属性"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[2] << 4) | response[3]);
    UAV_LOG_INFO << "图像属性值: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getWhiteBalance(WhiteBalanceMode &mode, uint8_t &rGain, uint8_t &bGain)
{
    UAV_LOG_INFO << "查询白平衡\n";
    uint8_t cmd[] = {0x81, 0x09, 0x14, 0x50, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询白平衡失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 6, "查询白平衡"))
    {
        return false;
    }

    mode = static_cast<WhiteBalanceMode>(response[2]);
    rGain = response[3];
    bGain = response[4];
    UAV_LOG_INFO << "白平衡 mode=0x" << std::hex << static_cast<int>(mode)
                 << " R=" << static_cast<int>(rGain)
                 << " B=" << static_cast<int>(bGain) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getGamma(uint8_t &value)
{
    UAV_LOG_INFO << "查询伽马值\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x5B, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询伽马值失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询伽马值"))
    {
        return false;
    }

    value = response[2];
    UAV_LOG_INFO << "伽马值: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

// 应答帧 90 50 00 00 0p 0q FF（7 字节）——原实现缓冲区只有 6 字节，条件恒真
bool ShihuiCore::getSharpness(uint8_t &value)
{
    UAV_LOG_INFO << "查询锐化等级\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x42, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询锐化等级失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询锐化等级"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[4] << 4) | response[5]);
    UAV_LOG_INFO << "锐化等级: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getCameraInhibition(uint8_t &value)
{
    UAV_LOG_INFO << "查询抑制等级\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x02, 0x12, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询抑制等级失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询抑制等级"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[2] << 4) | response[3]);
    UAV_LOG_INFO << "抑制等级: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getNR2D(uint8_t &value)
{
    UAV_LOG_INFO << "查询 2D 降噪等级\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x53, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 2D 降噪失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询 2D 降噪"))
    {
        return false;
    }

    value = response[2];
    UAV_LOG_INFO << "2D 降噪: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getNR3D(uint8_t &value)
{
    UAV_LOG_INFO << "查询 3D 降噪等级\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x51, 0x30, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 3D 降噪失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询 3D 降噪"))
    {
        return false;
    }

    value = response[2];
    UAV_LOG_INFO << "3D 降噪: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getAdvancedNR(uint8_t &mode, uint8_t &level)
{
    UAV_LOG_INFO << "查询高级降噪\n";
    uint8_t cmd[] = {0x81, 0x09, 0x14, 0x64, 0x02, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询高级降噪失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询高级降噪"))
    {
        return false;
    }

    mode = response[2];
    level = response[3];
    UAV_LOG_INFO << "高级降噪 mode=0x" << std::hex << static_cast<int>(mode)
                 << " level=0x" << static_cast<int>(level) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getAFMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询自动对焦模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x14, 0x57, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询自动对焦模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询自动对焦模式"))
    {
        return false;
    }

    mode = response[2];
    UAV_LOG_INFO << "自动对焦模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getImageBrightness(uint8_t &value)
{
    return getImageProperty(BRIGHTNESS, value);
}

bool ShihuiCore::getImageContrast(uint8_t &value)
{
    return getImageProperty(CONTRAST, value);
}

bool ShihuiCore::getImageSaturation(uint8_t &value)
{
    return getImageProperty(SATURATION, value);
}

bool ShihuiCore::getImageTone(uint8_t &value)
{
    return getImageProperty(TONE, value);
}

bool ShihuiCore::getShutterPosition(uint8_t &value)
{
    UAV_LOG_INFO << "查询快门位置\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x4A, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询快门位置失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询快门位置"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[4] << 4) | response[5]);
    UAV_LOG_INFO << "快门位置: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getGainPosition(uint8_t &value)
{
    UAV_LOG_INFO << "查询增益值\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x4C, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询增益值失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询增益值"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[4] << 4) | response[5]);
    UAV_LOG_INFO << "增益值: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getIrcGain(uint8_t &value)
{
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x4C, 0x52, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 ICR 切换增益失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询 ICR 切换增益"))
    {
        return false;
    }

    value = response[2];
    return true;
}

bool ShihuiCore::getExposureCompensationPosition(uint8_t &value)
{
    UAV_LOG_INFO << "查询曝光补偿值\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x4E, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询曝光补偿值失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询曝光补偿值"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[4] << 4) | response[5]);
    UAV_LOG_INFO << "曝光补偿值: 0x" << std::hex << static_cast<int>(value) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getAEMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询曝光模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x39, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询曝光模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询曝光模式"))
    {
        return false;
    }

    mode = response[2];
    UAV_LOG_INFO << "曝光模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getZoomPosition(uint16_t &position)
{
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x47, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询变焦位置失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询变焦位置"))
    {
        return false;
    }

    position = static_cast<uint16_t>((response[2] << 12) | (response[3] << 8) |
                                     (response[4] << 4) | response[5]);
    return true;
}

// 查询变焦模式：标准查询帧 81 09 04 06 FF（原实现把未初始化的输出参数写进了命令）
bool ShihuiCore::getZoomMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询变焦模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x06, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询变焦模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询变焦模式"))
    {
        return false;
    }

    mode = response[2];
    UAV_LOG_INFO << "变焦模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getMaximumZoomRatio(uint8_t &opticalZoom, uint8_t &digitalZoom)
{
    UAV_LOG_INFO << "查询最大变焦倍数\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x47, 0x51, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询最大变焦倍数失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询最大变焦倍数"))
    {
        return false;
    }

    opticalZoom = static_cast<uint8_t>((response[2] << 4) | response[3]);
    digitalZoom = static_cast<uint8_t>((response[4] << 4) | response[5]);
    UAV_LOG_INFO << "最大变焦 光学=" << static_cast<int>(opticalZoom)
                 << " 数字=" << static_cast<int>(digitalZoom) << "\n";
    return true;
}

bool ShihuiCore::getCurrentZoomPosition(uint32_t &targetPosition, uint32_t &originalZoomRatio,
                                        uint16_t &zoomPosition)
{
    UAV_LOG_INFO << "查询当前变焦位置\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x47, 0x50, 0xFF};
    uint8_t response[16] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询当前变焦位置失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 13, "查询当前变焦位置"))
    {
        return false;
    }

    // 三段 28 位解包：每段 3 字节低 7 位，移位 14/7/0（原实现第 3 段误写成 <<7）
    targetPosition = ((static_cast<uint32_t>(response[2] & 0x7F) << 14) |
                      (static_cast<uint32_t>(response[3] & 0x7F) << 7) |
                      static_cast<uint32_t>(response[4] & 0x7F));

    originalZoomRatio = ((static_cast<uint32_t>(response[5] & 0x7F) << 14) |
                         (static_cast<uint32_t>(response[6] & 0x7F) << 7) |
                         static_cast<uint32_t>(response[7] & 0x7F));

    zoomPosition = static_cast<uint16_t>(((response[8] & 0x0F) << 12) |
                                         ((response[9] & 0x0F) << 8) |
                                         ((response[10] & 0x0F) << 4) |
                                         (response[11] & 0x0F));
    return true;
}

// 连续变焦位置上报开关：模式 2=启用，3=禁用（按头文件签名，参数用引用传入）
bool ShihuiCore::getContinuousZoomPos(uint8_t &mode)
{
    UAV_LOG_INFO << "设置连续变焦位置上报模式: " << static_cast<int>(mode) << "\n";
    if (mode != 2 && mode != 3)
    {
        UAV_LOG_ERROR << "连续变焦位置上报模式非法(仅支持 2=启用 / 3=禁用)\n";
        return false;
    }

    uint8_t cmd[] = {0x81, 0x01, 0x04, 0x69, mode, 0xFF};
    return sendCommand(cmd, sizeof(cmd));
}

bool ShihuiCore::getFocusPosition(uint16_t &position)
{
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x48, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询聚焦位置失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询聚焦位置"))
    {
        return false;
    }

    position = static_cast<uint16_t>((response[2] << 12) | (response[3] << 8) |
                                     (response[4] << 4) | response[5]);
    return true;
}

bool ShihuiCore::getFocusMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询聚焦模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x38, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询聚焦模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询聚焦模式"))
    {
        return false;
    }

    mode = response[2];
    UAV_LOG_INFO << "聚焦模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    return true;
}

// 原实现末尾漏了 return true，会走到函数末尾（未定义行为）
bool ShihuiCore::getFocusSensitivity(uint8_t &sensitivity)
{
    UAV_LOG_INFO << "查询聚焦灵敏度\n";
    uint8_t cmd[] = {0x81, 0x09, 0x14, 0x57, 0x10, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询聚焦灵敏度失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询聚焦灵敏度"))
    {
        return false;
    }

    sensitivity = response[2];
    UAV_LOG_INFO << "聚焦灵敏度: 0x" << std::hex << static_cast<int>(sensitivity)
                 << std::dec << "\n";
    return true;
}

bool ShihuiCore::getAFSensitivity(uint8_t &sensitivity)
{
    return getFocusSensitivity(sensitivity);
}

bool ShihuiCore::getFocusNearLimit(uint16_t &limit)
{
    UAV_LOG_INFO << "查询聚焦近限\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x28, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询聚焦近限失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询聚焦近限"))
    {
        return false;
    }

    limit = static_cast<uint16_t>((response[2] << 12) | (response[3] << 8) |
                                  (response[4] << 4) | response[5]);
    return true;
}

bool ShihuiCore::getPictureFlipMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询图像翻转模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x66, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询图像翻转模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询图像翻转模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getICRMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询 ICR 模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x51, 0x24, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 ICR 模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询 ICR 模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getICREnableMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询 ICR 启用模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x01, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 ICR 启用模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询 ICR 启用模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getAutoICRMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询自动 ICR 模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x51, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询自动 ICR 模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询自动 ICR 模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

// 应答帧 90 50 0p 0q FF（5 字节）——原实现缓冲区 6 字节却判断 <7，恒失败
bool ShihuiCore::getAutoICRThreshold(uint8_t &threshold)
{
    UAV_LOG_INFO << "查询自动 ICR 阈值\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x21, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询自动 ICR 阈值失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询自动 ICR 阈值"))
    {
        return false;
    }

    threshold = static_cast<uint8_t>((response[2] << 4) | response[3]);
    UAV_LOG_INFO << "自动 ICR 阈值: 0x" << std::hex << static_cast<int>(threshold)
                 << std::dec << "\n";
    return true;
}

bool ShihuiCore::getDayNightSwitchDelay(uint8_t type, uint8_t &delay)
{
    UAV_LOG_INFO << "查询日夜切换延迟 type=0x" << std::hex << static_cast<int>(type)
                 << std::dec << "\n";
    if (type != 0x22 && type != 0x23)
    {
        UAV_LOG_ERROR << "日夜切换延迟类型非法(仅 0x22 白天→夜晚 / 0x23 夜晚→白天)\n";
        return false;
    }

    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x51, type, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询日夜切换延迟失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询日夜切换延迟"))
    {
        return false;
    }

    delay = static_cast<uint8_t>((response[2] << 4) | response[3]);
    UAV_LOG_INFO << "日夜切换延迟: " << static_cast<int>(delay) << " 秒\n";
    return true;
}

bool ShihuiCore::getElectronicFogMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询电子除雾模式\n";
    uint8_t cmd[] = {0x82, 0x09, 0x14, 0x55, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询电子除雾模式失败\n";
        return false;
    }
    // 该类应答帧头是 0xA0
    if (resp_len < 5 || response[0] != 0xA0 || response[resp_len - 1] != 0xFF)
    {
        UAV_LOG_ERROR << "查询电子除雾模式应答格式无效\n";
        return false;
    }

    mode = response[2];
    UAV_LOG_INFO << "电子除雾模式: 0x" << std::hex << static_cast<int>(mode) << std::dec << "\n";
    return true;
}

bool ShihuiCore::getFogPenetrationLevel(uint8_t &level)
{
    UAV_LOG_INFO << "查询透雾等级\n";
    uint8_t cmd[] = {0x82, 0x09, 0x14, 0x55, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询透雾等级失败\n";
        return false;
    }
    if (resp_len < 5 || response[0] != 0xA0 || response[resp_len - 1] != 0xFF)
    {
        UAV_LOG_ERROR << "查询透雾等级应答格式无效\n";
        return false;
    }

    level = response[3];
    UAV_LOG_INFO << "透雾等级: " << static_cast<int>(level) << "\n";
    return true;
}

bool ShihuiCore::getStabilizerMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询防抖模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x34, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询防抖模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询防抖模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getImageStabilizer(uint8_t &mode)
{
    UAV_LOG_INFO << "查询图像稳定器\n";
    uint8_t cmd[] = {0x82, 0x09, 0x14, 0x53, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询图像稳定器失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询图像稳定器"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getWDRMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询宽动态模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x33, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询宽动态模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询宽动态模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getWD(uint8_t &level)
{
    UAV_LOG_INFO << "查询 WD 等级\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x3D, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 WD 等级失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询 WD 等级"))
    {
        return false;
    }

    level = response[2];
    return true;
}

bool ShihuiCore::getSlowShutterMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询慢快门模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x5A, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询慢快门模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询慢快门模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getHighlightCompensationMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询强光补偿模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x33, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询强光补偿模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询强光补偿模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getHighlightCompensationStrength(uint8_t &strength)
{
    UAV_LOG_INFO << "查询强光补偿强度\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x33, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询强光补偿强度失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询强光补偿强度"))
    {
        return false;
    }

    strength = response[2];
    return true;
}

bool ShihuiCore::getStyle(uint8_t &style)
{
    UAV_LOG_INFO << "查询图像风格\n";
    uint8_t cmd[] = {0x81, 0x09, 0x14, 0x60, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询图像风格失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询图像风格"))
    {
        return false;
    }

    style = response[2];
    return true;
}

bool ShihuiCore::getDZoomMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询数字变焦模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x06, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询数字变焦模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询数字变焦模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getLRReverseMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询左右镜像模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x61, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询左右镜像模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 4, "查询左右镜像模式"))
    {
        return false;
    }

    mode = response[2];
    return true;
}

bool ShihuiCore::getMipiAcquisition(uint8_t &mode)
{
    UAV_LOG_INFO << "查询 MIPI 采集模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x24, 0x74, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 MIPI 采集模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询 MIPI 采集模式"))
    {
        return false;
    }

    mode = response[3];
    return true;
}

bool ShihuiCore::getFormat(uint8_t &format)
{
    UAV_LOG_INFO << "查询尾板视频输入方式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x23, 0x00, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询视频输入方式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询视频输入方式"))
    {
        return false;
    }

    format = response[3];
    return true;
}

bool ShihuiCore::getMipiFormat(uint8_t &formatHigh, uint8_t &formatLow)
{
    UAV_LOG_INFO << "查询 MIPI 格式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x24, 0x72, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询 MIPI 格式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询 MIPI 格式"))
    {
        return false;
    }

    formatHigh = response[2];
    formatLow = response[3];
    return true;
}

bool ShihuiCore::getVisibleLightVideoFormat(uint8_t &format)
{
    return getMipiFormat(format, format);   // 同一查询帧，可见光取低位字段
}

bool ShihuiCore::getThermalVideoFormat(uint8_t &format)
{
    uint8_t high = 0;
    return getMipiFormat(high, format);
}

// 原实现从 response[1]（Socket 字节）开始解包且末两段都 <<4，这里按 12/8/4/0 解包
bool ShihuiCore::getHorizontalResolution(uint16_t &resolution)
{
    UAV_LOG_INFO << "查询水平分辨率\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x23, 0x01, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询水平分辨率失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询水平分辨率"))
    {
        return false;
    }

    resolution = static_cast<uint16_t>((response[2] << 12) | (response[3] << 8) |
                                       (response[4] << 4) | response[5]);
    return true;
}

bool ShihuiCore::getVerticalResolution(uint16_t &resolution)
{
    UAV_LOG_INFO << "查询垂直分辨率\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x23, 0x02, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询垂直分辨率失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询垂直分辨率"))
    {
        return false;
    }

    resolution = static_cast<uint16_t>((response[2] << 12) | (response[3] << 8) |
                                       (response[4] << 4) | response[5]);
    return true;
}

bool ShihuiCore::getRegisterValue(uint16_t regNum, uint8_t &value)
{
    UAV_LOG_INFO << "查询寄存器 0x" << std::hex << regNum << std::dec << "\n";
    uint8_t cmd[] = {
        0x81, 0x09, 0x04, 0x24,
        static_cast<uint8_t>((regNum >> 4) & 0x0F),
        static_cast<uint8_t>(regNum & 0x0F), 0xFF
    };
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询寄存器失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询寄存器"))
    {
        return false;
    }

    value = static_cast<uint8_t>((response[2] << 4) | response[3]);
    return true;
}

bool ShihuiCore::getLockMode(uint8_t &mode)
{
    UAV_LOG_INFO << "查询锁定模式\n";
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x24, 0x0F, 0x0A, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询锁定模式失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询锁定模式"))
    {
        return false;
    }

    mode = response[3];
    return true;
}

bool ShihuiCore::getHeatHaze(HeatHazeMode &mode, HeatHazeLevel &level)
{
    UAV_LOG_INFO << "查询热浪减少\n";
    uint8_t cmd[] = {0x82, 0x09, 0x14, 0x56, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询热浪减少失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 5, "查询热浪减少"))
    {
        return false;
    }

    mode = static_cast<HeatHazeMode>(response[2]);
    level = static_cast<HeatHazeLevel>(response[3]);
    return true;
}

// ---------------------------------------------------------------------------
// 版本 / 统计类查询
// ---------------------------------------------------------------------------
bool ShihuiCore::getProgramVersion(uint16_t &year, uint8_t &month, uint8_t &day,
                                   uint8_t &serialNumber)
{
    UAV_LOG_INFO << "查询程序版本\n";
    uint8_t cmd[] = {0x81, 0x09, 0x00, 0x72, 0x01, 0xFF};
    uint8_t response[16] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询程序版本失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 9, "查询程序版本"))
    {
        return false;
    }

    year = static_cast<uint16_t>((response[2] << 8) | response[3]);
    month = response[4];
    day = response[5];
    serialNumber = response[6];
    UAV_LOG_INFO << "程序版本 year=" << year << " month=" << static_cast<int>(month)
                 << " day=" << static_cast<int>(day)
                 << " serial=" << static_cast<int>(serialNumber) << "\n";
    return true;
}

// 应答字段布局按原实现（vendor/model 各 2 字节，主/次/修订各 1 字节）。
// 注意：原实现用 8 字节缓冲却读 response[8]（越界），这里放大缓冲并做长度检查；
//      若实测字段位置不同，按相机协议文档调整这里的下标。
bool ShihuiCore::getVersion(uint16_t &vendorId, uint16_t &modelId, uint8_t &majorVersion,
                            uint8_t &minorVersion, uint8_t &fixVersion)
{
    UAV_LOG_INFO << "查询版本信息\n";
    uint8_t cmd[] = {0x81, 0x09, 0x00, 0x02, 0xFF};
    uint8_t response[16] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询版本信息失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 9, "查询版本信息"))
    {
        return false;
    }

    vendorId = static_cast<uint16_t>((response[2] << 8) | response[3]);
    modelId = static_cast<uint16_t>((response[4] << 8) | response[5]);
    majorVersion = response[6];
    minorVersion = response[7];
    fixVersion = response[8];
    return true;
}

// 28 位解包的公共逻辑：3 个字节低 7 位 → 21 位，或 4 个字节低 7 位 → 28 位
static uint32_t unpackU28(const uint8_t *p, int count)
{
    uint32_t v = 0;
    for (int i = 0; i < count; i++)
    {
        v = (v << 7) | static_cast<uint32_t>(p[i] & 0x7F);
    }
    return v;
}

bool ShihuiCore::getLensZoomInfo(uint32_t &zoomTimes, uint32_t &zoomDirectionTimes)
{
    UAV_LOG_INFO << "查询镜头变焦信息\n";
    uint8_t cmd[] = {0x81, 0x09, 0x00, 0x73, 0x00, 0xFF};
    uint8_t response[16] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询镜头变焦信息失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 12, "查询镜头变焦信息"))
    {
        return false;
    }

    zoomTimes = unpackU28(response + 2, 4);
    zoomDirectionTimes = unpackU28(response + 6, 4);
    return true;
}

bool ShihuiCore::getLensFocusInfo(uint32_t &focusTimes, uint32_t &focusDirectionTimes)
{
    UAV_LOG_INFO << "查询镜头聚焦信息\n";
    uint8_t cmd[] = {0x81, 0x09, 0x00, 0x73, 0x01, 0xFF};
    uint8_t response[16] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询镜头聚焦信息失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 12, "查询镜头聚焦信息"))
    {
        return false;
    }

    focusTimes = unpackU28(response + 2, 4);
    focusDirectionTimes = unpackU28(response + 6, 4);
    return true;
}

bool ShihuiCore::getLensIrisInfo(uint32_t &irisTimes, uint32_t &icrTimes)
{
    UAV_LOG_INFO << "查询镜头光圈信息\n";
    uint8_t cmd[] = {0x81, 0x09, 0x00, 0x73, 0x02, 0xFF};
    uint8_t response[16] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询镜头光圈信息失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 12, "查询镜头光圈信息"))
    {
        return false;
    }

    irisTimes = unpackU28(response + 2, 4);
    icrTimes = unpackU28(response + 6, 4);
    return true;
}

// 机芯温度：应答 90 50 0p 0q 0q 0q FF，p 为符号位，qqq 为 0.1℃ 为单位的绝对值
bool ShihuiCore::getTemperature(float &temperature)
{
    uint8_t cmd[] = {0x81, 0x09, 0x04, 0x19, 0x30, 0x50, 0x00, 0x00, 0xFF};
    uint8_t response[8] = {0};
    int32_t resp_len = sizeof(response);

    if (!sendQueryCommandAndWaitForResponse(cmd, sizeof(cmd), response, resp_len, 3000, sizeof(response)))
    {
        UAV_LOG_ERROR << "查询机芯温度失败\n";
        return false;
    }
    if (!checkShihuiResponse(response, resp_len, 7, "查询机芯温度"))
    {
        return false;
    }

    const uint16_t raw = static_cast<uint16_t>(((response[3] & 0x0F) << 8) |
                                               ((response[4] & 0x0F) << 4) |
                                               (response[5] & 0x0F));
    const float value = static_cast<float>(raw) / 10.0f;
    temperature = (response[2] == 0) ? value : -value;

    UAV_LOG_INFO << "机芯温度: " << temperature << " ℃\n";
    return true;
}
