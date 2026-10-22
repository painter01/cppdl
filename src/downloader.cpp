#include "downloader.h"  // 本模块接口：DownloadConfig / RunDownload 等声明

#include <windows.h>  // Win32 API：文件句柄、控制台事件、线程与错误码

#include "format.h"  // 项目内格式化：FormatBytes / FormatSpeed / FormatPercent / UTF-8 转换

#include <algorithm>  // std::min / std::max
#include <atomic>  // std::atomic：跨线程共享停止标志、进度与活跃连接数
#include <cerrno>  // errno：判断 strtoll 解析是否溢出
#include <chrono>  // std::chrono：计时、200ms 采样、重试等待
#include <cmath>  // 数学函数（本文件暂未直接使用，保留原有包含）
#include <condition_variable>  // std::condition_variable：采样线程的可中断等待
#include <cstdio>  // snprintf：格式化下载统计摘要
#include <cstdlib>  // std::strtoll：解析状态文件中的整数
#include <deque>  // std::deque：保存最近 2 秒的采样点
#include <exception>  // std::exception：捕获线程创建失败
#include <functional>  // std::function：保存进度提供者与回调
#include <mutex>  // std::mutex / std::lock_guard：保护错误列表与致命错误
#include <sstream>  // std::istringstream / std::ostringstream：读写 .dlstate 文本
#include <string>  // std::string / std::wstring
#include <thread>  // std::thread：分片下载、进度采样、状态落盘
#include <utility>  // std::move / std::pair
#include <vector>  // std::vector：分片边界、线程列表与错误列表

namespace {  // 匿名命名空间：以下符号仅在本翻译单元内可见

constexpr long long kSegmentThreshold = 1LL << 20;  // 小于 1 MiB 不分片
constexpr long long kMinChunkSize = 256 * 1024;      // 单片最小 256 KiB

std::atomic<bool> g_stop{false};  // 全局停止标志：仅当 hooks.stopFlag 为空时作为兜底（单任务控制台模式）
std::atomic<bool> g_interrupted{false};  // 同上，兜底中断标记

// --- per-task 停止标志访问辅助（优先使用 hooks 中的任务级标志，回退到进程级全局标志）---
inline bool IsStopped(const DownloadHooks& h) {
    return h.stopFlag ? h.stopFlag->load(std::memory_order_relaxed)
                      : g_stop.load(std::memory_order_relaxed);
}
inline void RequestStop(const DownloadHooks& h) {
    if (h.stopFlag) h.stopFlag->store(true, std::memory_order_relaxed);
    else g_stop.store(true, std::memory_order_relaxed);
}
inline bool IsInterrupted(const DownloadHooks& h) {
    return h.interruptedFlag ? h.interruptedFlag->load(std::memory_order_relaxed)
                             : g_interrupted.load(std::memory_order_relaxed);
}
inline void MarkInterrupted(const DownloadHooks& h) {
    if (h.interruptedFlag) h.interruptedFlag->store(true, std::memory_order_relaxed);
    else g_interrupted.store(true, std::memory_order_relaxed);
}

// 控制台控制事件处理函数。
// 参数 type 为事件类型（Ctrl+C、关闭窗口等）；返回 TRUE 表示已处理、系统不再执行默认终止。
// 线程安全：只写两个全局原子变量，不做 I/O、不加锁，避免在信号上下文死锁。
BOOL WINAPI CtrlHandler(DWORD type) {  // 控制台控制事件回调入口
    switch (type) {  // 按事件类型分别处理
        case CTRL_C_EVENT:  // 控制台 Ctrl+C
        case CTRL_BREAK_EVENT:  // Ctrl+Break，与 Ctrl+C 一样视为用户中断
            g_interrupted.store(true);  // 标记为用户主动中断，RunDownload 据此返回 Interrupted
            g_stop.store(true);  // 通知所有工作线程尽快停止
            return TRUE;  // 已处理，阻止系统默认终止进程
        case CTRL_CLOSE_EVENT:  // 控制台窗口被关闭
        case CTRL_LOGOFF_EVENT:  // 用户注销
        case CTRL_SHUTDOWN_EVENT:  // 系统关机
            g_stop.store(true);  // 只置停止标志，让下载线程有机会保存断点状态
            return TRUE;  // 已处理
        default:  // 其他不关心的事件
            return FALSE;  // 交回系统默认处理
    }
}

// ---------------------------------------------------------------- 消息输出

// 输出一条普通信息；hooks.onMessage 为空时静默忽略。
void Info(const DownloadHooks& hooks, const std::wstring& text) {  // 输出普通信息
    if (hooks.onMessage) hooks.onMessage(text, false);  // 第二个参数 false 表示非错误消息
}

// 输出一条错误信息，统一加上“错误: ”前缀，便于控制台/GUI 区分。
void Fail(const DownloadHooks& hooks, const std::wstring& text) {  // 输出错误信息
    if (hooks.onMessage) hooks.onMessage(L"错误: " + text, true);  // true 表示错误级别
}

// ---------------------------------------------------------------- 进度采样

// 周期采样下载进度并通过回调上报，与界面类型无关
// 线程模型：Start 创建独立采样线程，每 200ms 读取一次累计进度；
// 速度用最近约 2 秒的滑动窗口计算，避免瞬时抖动；
// Stop 通过条件变量唤醒并 join 采样线程，可安全重复调用。
class ProgressSampler {  // 周期进度采样器
public:
    using Provider = std::function<long long()>;  // 进度提供者：返回当前累计已下载字节

    ProgressSampler(Provider provider, long long total, std::atomic<int>* active,  // 进度来源、总量、活跃数指针
                    int workers, const DownloadHooks& hooks)  // 构造：保存进度来源、总量、活跃数指针、连接数与回调
        : provider_(std::move(provider)),  // 接管进度提供者 lambda
          total_(total),  // 总字节数，-1 表示未知
          active_(active),  // 指向跨线程的活跃连接计数
          workers_(workers),  // 总连接数（用于界面显示）
          callback_(hooks.onProgress) {}  // 复制进度回调；为空则不启动采样线程

    ~ProgressSampler() { Stop(); }  // 析构时确保采样线程已退出，避免访问已销毁的成员

    void Start() {  // 启动采样线程；没有进度回调时什么也不做
        if (callback_) thread_ = std::thread([this] { Loop(); });  // 仅在有回调时创建线程，避免空转
    }

    void Stop() {  // 请求采样线程退出并 join；可重复调用
        if (stopped_) return;  // 已经停止过，直接返回，保证 join 只执行一次
        stopped_ = true;  // 记录已停止（该函数约定只由控制线程调用，无需原子）
        if (thread_.joinable()) {  // 线程确实启动过才需要通知与 join
            {
                std::lock_guard<std::mutex> lock(mutex_);  // 与 wait_for 的条件检查互斥
                stopping_ = true;  // 置停止条件，使等待中的采样线程能够醒来
            }
            cv_.notify_one();  // 唤醒 cv_.wait_for
            thread_.join();  // 阻塞等待采样线程退出，防止对象析构后线程仍在运行
        }
    }

private:
    void Loop() {  // 采样线程主循环：每 200ms 取一次进度并计算滑动窗口速度
        using clock = std::chrono::steady_clock;  // 单调时钟，避免系统时间调整影响速度计算
        auto now = clock::now();  // 当前采样时刻
        std::deque<std::pair<clock::time_point, long long>> samples;  // 保存 (时刻, 累计字节) 采样点
        samples.emplace_back(now, provider_());  // 先记录一个基线点，供首次速度计算使用

        while (true) {  // 循环直到 stopping_ 被置位
            {
                std::unique_lock<std::mutex> lock(mutex_);  // 加锁检查停止条件
                if (cv_.wait_for(lock, std::chrono::milliseconds(200),  // 以 200ms 超时等待停止条件
                                 [this] { return stopping_; })) {  // 最多等 200ms，或被停止标志唤醒
                    break;  // 收到停止请求，退出循环
                }
            }

            now = clock::now();  // 更新采样时刻
            const long long done = provider_();  // 读取当前累计已下载字节（所有分片之和）
            samples.emplace_back(now, done);  // 记录本次采样
            while (samples.size() > 2 &&  // 至少保留 2 个采样点
                   now - samples.front().first > std::chrono::seconds(2)) {  // 只保留最近 2 秒、且至少 2 个采样点
                samples.pop_front();  // 丢弃过旧的采样点
            }

            const double window =  // 滑动窗口长度（秒）
                std::chrono::duration<double>(now - samples.front().first).count();  // 滑动窗口长度（秒）
            double speed = 0.0;  // 默认速度为 0
            if (window > 0.25) {  // 窗口过短会使除数太小、速度剧烈抖动，因此要求至少 0.25 秒
                speed = static_cast<double>(done - samples.front().second) / window;  // 窗口内字节增量 / 窗口时长
            }
            if (speed < 0.0) speed = 0.0;  // 防御性处理：重试导致进度回退时不允许出现负速度

            ProgressInfo info;  // 构造本次进度快照
            info.done = done;  // 已下载字节
            info.total = total_;  // 总字节（-1 表示未知）
            info.speedBps = speed;  // 平滑后的速度
            info.active = active_->load(std::memory_order_relaxed);  // 活跃连接数，relaxed 足够（仅用于展示）
            info.workers = workers_;  // 总连接数
            callback_(info);  // 回调界面；约定回调本身不抛异常
        }
    }

    Provider provider_;  // 进度提供者
    long long total_ = -1;  // 总字节数，-1 表示未知
    std::atomic<int>* active_ = nullptr;  // 指向外部的活跃连接计数
    int workers_ = 1;  // 总连接数
    std::function<void(const ProgressInfo&)> callback_;  // 进度回调

    std::mutex mutex_;  // 保护 stopping_ 与条件变量等待
    std::condition_variable cv_;  // 用于可中断的 200ms 等待
    bool stopping_ = false;  // 停止请求标志（受 mutex_ 保护）
    bool stopped_ = false;  // 是否已经执行过 Stop
    std::thread thread_;  // 采样线程
};

// ---------------------------------------------------------------- 文件与状态

// 一次性把 data 全部写入 path（覆盖创建），成功返回 true。
// 用于写入较小的 .dlstate 状态文件；先写临时文件再原子替换由调用方负责。
bool WriteAllBytes(const std::wstring& path, const std::string& data) {  // 覆盖写整个小文件
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,  // 覆盖创建，独占写入
                              FILE_ATTRIBUTE_NORMAL, nullptr);  // 覆盖创建，独占写入
    if (file == INVALID_HANDLE_VALUE) return false;  // 打开失败（无权限、目录不存在等）
    const DWORD expected = static_cast<DWORD>(data.size());  // 状态文件很小，可安全截断为 DWORD
    DWORD written = 0;  // 实际写入字节数
    bool ok = WriteFile(file, data.data(), expected, &written, nullptr) != 0 &&  // 写入全部数据
              written == expected;  // 必须写满全部字节才算成功
    FlushFileBuffers(file);  // 刷盘，降低掉电丢失状态的概率
    CloseHandle(file);  // 关闭句柄
    return ok;  // 返回写入结果
}

// 读取整个文件到 data；成功返回 true。
// 只接受 1 字节到 64 MiB 的文件，防止异常文件耗尽内存。
bool ReadAllBytes(const std::wstring& path, std::string& data) {  // 读取整个小文件
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,  // 只读打开，允许共享
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);  // 只读打开已有文件
    if (file == INVALID_HANDLE_VALUE) return false;  // 文件不存在或无法读取

    LARGE_INTEGER size{};  // 文件大小
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {  // 空文件、超大文件视为无效状态
        CloseHandle(file);  // 关闭句柄
        return false;  // 交给调用方按“状态无效”处理
    }

    data.resize(static_cast<size_t>(size.QuadPart));  // 一次性分配所需缓冲区
    const DWORD expected = static_cast<DWORD>(data.size());  // 期望读取的字节数（已限制 <= 64 MiB）
    DWORD read = 0;  // 实际读取字节数
    bool ok = ReadFile(file, data.data(), expected, &read, nullptr) != 0 &&  // 读取全部数据
              read == expected;  // 必须读满
    CloseHandle(file);  // 关闭句柄
    return ok;  // 返回读取结果
}

// 判断 path 是否存在且大小恰好等于 expected；任何错误都返回 false。
bool FileSizeEquals(const std::wstring& path, long long expected) {  // 校验文件实际大小
    WIN32_FILE_ATTRIBUTE_DATA info{};  // 接收文件属性与大小
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;  // 文件不存在或无法访问
    long long size = (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;  // 拼接 64 位文件大小
    return size == expected;  // 大小一致才返回 true
}

// 准备输出文件：
// - 以 OPEN_ALWAYS 打开（不存在则创建）；
// - keepExisting 为 true 且现有文件大小已等于 total 时直接保留；
// - 否则先截断为 0，再扩展到 total，保证后续各分片按偏移写入。
// 失败时通过 err 返回 Win32 错误描述。
bool PrepareOutputFile(const std::wstring& path, long long total, bool keepExisting,  // 准备输出文件
                       std::wstring& err) {  // 通过 err 返回失败原因
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,  // 以写权限打开
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,  // 允许其他线程共享
                              FILE_ATTRIBUTE_NORMAL, nullptr);  // 允许其他线程同时打开同一文件
    if (file == INVALID_HANDLE_VALUE) {  // 打开失败
        err = FormatWin32Error(GetLastError());  // 记录打开失败原因
        return false;  // 准备失败
    }

    if (keepExisting && FileSizeEquals(path, total)) {  // 续传时文件已完整，无需重新布局
        CloseHandle(file);  // 关闭句柄
        return true;  // 直接复用现有文件内容
    }

    LARGE_INTEGER zero{};  // 文件偏移 0
    LARGE_INTEGER target{};  // 目标大小
    target.QuadPart = total;  // 设置为期望的总字节数
    bool ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != 0 &&  // 定位到开头
              SetEndOfFile(file) != 0 &&  // 先截断为 0
              SetFilePointerEx(file, target, nullptr, FILE_BEGIN) != 0 &&  // 再定位到目标大小
              SetEndOfFile(file) != 0;  // 先截断为 0，再扩展到 total，得到可随机写入的空文件
    if (!ok) err = FormatWin32Error(GetLastError());  // 记录失败原因
    CloseHandle(file);  // 关闭句柄
    return ok;  // 返回准备结果
}

// 断点续传状态，对应输出文件旁的 .dlstate 文件：
// url 记录下载地址，total 为文件总大小，chunkSize 为分片大小，
// done[i] 为第 i 个分片已完成的字节数。
struct ResumeState {  // 断点续传状态
    std::wstring url;  // 下载 URL，防止用错状态文件
    long long total = -1;  // 文件总字节数
    long long chunkSize = -1;  // 分片大小；分片方案变化时旧状态不可用
    std::vector<long long> done;  // 每个分片已完成的字节数
};

// 把十进制字符串解析为非负 long long；格式非法、溢出或为负都返回 false。
bool ParseLongLong(const std::string& text, long long& value) {  // 解析非负十进制整数
    if (text.empty()) return false;  // 空串直接失败
    char* end = nullptr;  // 指向解析结束位置
    errno = 0;  // 清零 errno，便于判断 ERANGE
    long long parsed = std::strtoll(text.c_str(), &end, 10);  // 按十进制解析
    if (errno != 0 || end == text.c_str() || *end != '\0' || parsed < 0) return false;  // 溢出、无数字、尾部有残留或负数均无效
    value = parsed;  // 解析成功，写回结果
    return true;  // 成功
}

// 读取并解析 .dlstate 状态文件；格式不合法或内容不一致时返回 false。
// 正常格式（首行固定为 "CPPDL-STATE 1"）：
//   url <utf8-url>
//   size <total>
//   chunk <chunkSize>
//   count <n>
//   <index> <done>   （n 行）
bool ReadStateFile(const std::wstring& path, ResumeState& state) {  // 读取并校验状态文件
    std::string raw;  // 原始文件内容
    if (!ReadAllBytes(path, raw)) return false;  // 文件不存在或读取失败

    std::istringstream input(raw);  // 按行解析
    std::string line;  // 当前行
    if (!std::getline(input, line)) return false;  // 连首行都没有，文件无效
    if (!line.empty() && line.back() == '\r') line.pop_back();  // 兼容 CRLF 换行
    if (line != "CPPDL-STATE 1") return false;  // 版本标识不匹配，视为旧格式/损坏

    bool hasUrl = false;  // 是否读到 url
    bool hasTotal = false;  // 是否读到 size
    bool hasChunkSize = false;  // 是否读到 chunk
    std::vector<std::pair<long long, long long>> entries;  // 暂存 (分片下标, 已完成字节)

    while (std::getline(input, line)) {  // 逐行读取剩余内容
        if (!line.empty() && line.back() == '\r') line.pop_back();  // 去掉 CRLF 的 '\r'
        if (line.empty()) continue;  // 跳过空行

        if (line.compare(0, 4, "url ") == 0) {  // url 行
            state.url = WidenUtf8(line.substr(4));  // 取 "url " 之后的内容并转成宽字符
            hasUrl = true;  // 标记已读到
        } else if (line.compare(0, 5, "size ") == 0) {  // size 行
            if (!ParseLongLong(line.substr(5), state.total)) return false;  // 解析总大小，非法则整个状态无效
            hasTotal = true;  // 标记已读到
        } else if (line.compare(0, 6, "chunk ") == 0) {  // chunk 行
            if (!ParseLongLong(line.substr(6), state.chunkSize)) return false;  // 解析分片大小
            hasChunkSize = true;  // 标记已读到
        } else if (line.compare(0, 6, "count ") == 0) {  // count 行
            // 数量会在下方通过 done 条目校验
        } else {  // 其余行应为 "<index> <done>"
            size_t space = line.find(' ');  // 定位两个数字之间的空格
            if (space == std::string::npos) return false;  // 没有空格，格式错误
            long long index = 0;  // 分片下标
            long long done = 0;  // 已完成字节
            if (!ParseLongLong(line.substr(0, space), index) ||  // 解析分片下标
                !ParseLongLong(line.substr(space + 1), done)) {  // 两个字段都必须是非负整数
                return false;  // 任一解析失败则状态无效
            }
            entries.emplace_back(index, done);  // 记录该分片进度
        }
    }

    if (!hasUrl || !hasTotal || !hasChunkSize || entries.empty()) return false;  // 必需字段缺失或没有分片记录

    long long count = 0;  // 分片总数 = 最大下标 + 1
    for (const auto& entry : entries) {  // 遍历所有分片记录
        if (entry.first + 1 > count) count = entry.first + 1;  // 取最大下标加一
    }
    state.done.assign(static_cast<size_t>(count), 0);  // 分配并清零进度数组
    for (const auto& entry : entries) {  // 把记录写回对应下标
        state.done[static_cast<size_t>(entry.first)] = entry.second;  // 缺失的下标保持 0
    }
    return true;  // 解析成功
}

// 保存断点状态到 outputPath + ".dlstate"。
// 先写 ".tmp" 再 MoveFileEx 原子替换，避免崩溃时留下半截状态文件。
// done 为各分片的原子进度，读取使用 relaxed 内存序（只要求最终一致）。
bool SaveStateFile(const std::wstring& outputPath, const std::wstring& url,  // 保存断点状态
                   long long total, long long chunkSize,  // 总大小与分片大小
                   const std::vector<std::atomic<long long>>& done) {  // 各分片已完成的原子计数
    std::ostringstream output;  // 在内存中拼出完整状态文本
    output << "CPPDL-STATE 1\n";  // 魔数与版本
    output << "url " << NarrowUtf8(url) << "\n";  // 下载地址（UTF-8）
    output << "size " << total << "\n";  // 文件总大小
    output << "chunk " << chunkSize << "\n";  // 分片大小
    output << "count " << done.size() << "\n";  // 分片数量
    for (size_t i = 0; i < done.size(); ++i) {  // 逐个分片写入进度
        output << i << ' ' << done[i].load(std::memory_order_relaxed) << "\n";  // "下标 已完成字节"
    }

    const std::wstring path = outputPath + L".dlstate";  // 正式状态文件路径
    const std::wstring temp = path + L".tmp";  // 临时文件路径
    if (!WriteAllBytes(temp, output.str())) {  // 先写临时文件
        DeleteFileW(temp.c_str());  // 写入失败则清理残留
        return false;  // 保存失败
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(),  // 用临时文件替换正式文件
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {  // 原子替换正式文件并尽量刷盘
        DeleteFileW(temp.c_str());  // 替换失败则清理临时文件
        return false;  // 保存失败
    }
    return true;  // 保存成功
}

// 汇总所有分片已完成字节数。
// 使用 relaxed 读取即可：进度只用于展示/判断完成，不需要与其他内存操作同步。
long long SumDone(const std::vector<std::atomic<long long>>& done) {  // 汇总各分片进度
    long long sum = 0;  // 累计值
    for (const auto& value : done) {  // 遍历每个分片
        sum += value.load(std::memory_order_relaxed);  // 读取并累加
    }
    return sum;  // 返回总和
}

// ---------------------------------------------------------------- 下载

// 判断 HTTP 状态码是否为永久错误：4xx 中除 408（超时）和 429（限流）外不应重试。
bool IsPermanentStatus(int status) {  // 判断是否为不可重试的 4xx
    return status >= 400 && status < 500 && status != 408 && status != 429;  // 永久错误直接放弃
}

// 指数退避等待：参数 attempt 为已失败的尝试序号（从 0 开始），等待 250ms << attempt（上限 8 秒）。
// 等待期间每 100ms 检查一次停止标志，中断时可立即返回；stopFlag 为空时回退到进程级 g_stop。
void Backoff(int attempt, const std::atomic<bool>* stopFlag) {  // 指数退避并响应停止
    int delay = 250 << std::min(attempt, 5);  // 移位次数封顶 5，避免溢出
    if (delay > 8000) delay = 8000;  // 最长等待 8 秒
    auto stopped = [stopFlag]() {
        return stopFlag ? stopFlag->load(std::memory_order_relaxed)
                        : g_stop.load(std::memory_order_relaxed);
    };
    for (int waited = 0; waited < delay && !stopped(); waited += 100) {  // 分片式等待，便于响应停止
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 每次睡 100ms
    }
}

// 输出下载完成摘要：总大小、连接数、耗时与平均速度。
// total <= 0 时省略大小与平均速度；start 为整体开始时刻。
void PrintSummary(const DownloadHooks& hooks, const DownloadConfig& cfg, long long total,  // 输出完成摘要
                  int workers, std::chrono::steady_clock::time_point start) {  // 连接数与开始时刻
    double seconds =  // 总耗时（秒）
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();  // 计算总耗时（秒）
    if (seconds <= 0.0) seconds = 0.001;  // 防止除零/异常值

    char buffer[256];  // 存放 snprintf 结果
    std::string line = "下载完成: " + NarrowUtf8(cfg.outputPath);  // 摘要开头：输出路径
    if (total > 0) {  // 已知总大小时给出完整统计
        snprintf(buffer, sizeof(buffer), "  (%s, %d 个连接, %.1f 秒, 平均 %s)",  // 格式化完整统计
                 FormatBytes(total).c_str(), workers, seconds,  // 大小、连接数、耗时
                 FormatSpeed(static_cast<double>(total) / seconds).c_str());  // 大小、连接数、耗时、平均速度
    } else {  // 未知总大小
        snprintf(buffer, sizeof(buffer), "  (%d 个连接, %.1f 秒)", workers, seconds);  // 未知大小时只报连接数与耗时
    }
    line += buffer;  // 拼成完整一行
    Info(hooks, WidenUtf8(line));  // 转回宽字符后上报
}

// 单个分片工作线程的上下文。
// 由主线程在启动线程前填好；工作线程只读取这些指针，写入 done/active 时使用原子操作，
// 写入 errors/fatal 时必须持有 mutex。
struct WorkerContext {  // 单个分片线程的上下文
    const DownloadConfig* cfg = nullptr;  // 下载配置（URL、输出路径、重试次数、HTTP 选项）
    std::atomic<bool>* stopFlag = nullptr;  // per-task 停止标志（空则回退全局；可写）
    int index = 0;  // 分片下标，从 0 开始
    long long chunkStart = 0;  // 本分片起始偏移（含）
    long long chunkEnd = 0;  // 本分片结束偏移（含）
    long long total = 0;  // 文件总大小，用于校验服务器返回
    std::atomic<long long>* done = nullptr;  // 本分片已完成的原子计数
    std::atomic<int>* active = nullptr;  // 全局活跃连接数
    std::mutex* mutex = nullptr;  // 保护 errors 与 fatal
    std::vector<std::wstring>* errors = nullptr;  // 非致命的分片错误列表
    std::wstring* fatal = nullptr;  // 第一条致命错误；非空表示整体失败
};

// 记录一条致命错误并立即请求停止当前任务。
// 只保留第一条错误（fatal 为空时才写入），避免多线程互相覆盖。
void RecordFatal(const WorkerContext& ctx, const std::wstring& message) {  // 记录致命错误并停止任务
    {
        std::lock_guard<std::mutex> lock(*ctx.mutex);  // 加锁保护 fatal
        if (ctx.fatal->empty()) *ctx.fatal = message;  // 仅记录首条致命错误
    }
    if (ctx.stopFlag) ctx.stopFlag->store(true, std::memory_order_relaxed);
    else g_stop.store(true, std::memory_order_relaxed);  // 让其他分片尽快退出
}

// 单个分片的下载线程入口。
// 参数 ctx 按值接收（内含指针），线程生命周期内有效。
// 行为：从 done 记录的偏移开始请求 Range，循环读取并写入文件对应位置；
// 网络错误按 cfg.retries 指数退避重试；写文件失败或服务器不支持 Range 视为致命错误。
// 返回值：无。非致命失败会追加到 ctx.errors，致命失败通过 RecordFatal 记录。
void ChunkWorker(WorkerContext ctx) {  // 分片下载线程入口
    const DownloadConfig& cfg = *ctx.cfg;  // 取出配置引用，避免反复解引用
    const long long length = ctx.chunkEnd - ctx.chunkStart + 1;  // 本分片总字节数
    long long done = ctx.done->load(std::memory_order_relaxed);  // 读取续传进度（已完成字节）
    if (done >= length) return;  // 该分片已经完整，直接结束

    auto stopped = [&ctx]() {  // 读取本任务的停止标志
        return ctx.stopFlag ? ctx.stopFlag->load(std::memory_order_relaxed)
                            : g_stop.load(std::memory_order_relaxed);
    };

    HANDLE file = CreateFileW(cfg.outputPath.c_str(), GENERIC_WRITE,  // 以写权限打开输出文件
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,  // 允许其他分片线程共享
                              FILE_ATTRIBUTE_NORMAL, nullptr);  // 打开已预分配的输出文件
    if (file == INVALID_HANDLE_VALUE) {  // 打开失败
        RecordFatal(ctx, L"打开输出文件失败: " + FormatWin32Error(GetLastError()));  // 打开失败是致命错误
        return;  // 结束本线程
    }

    const int maxAttempts = std::max(0, cfg.retries) + 1;  // 总尝试次数 = 重试次数 + 首次
    std::wstring lastError;  // 保存最近一次错误，用于最终报告

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {  // 按尝试次数循环
        if (stopped()) break;  // 任务级停止（中断或其他分片致命错误）
        if (done >= length) break;  // 本分片已下完，无需再请求

        const long long from = ctx.chunkStart + done;  // 本次请求的起始偏移（续传位置）
        const std::wstring range = L"bytes=" + std::to_wstring(from) + L"-" +  // 起始偏移
                                   std::to_wstring(ctx.chunkEnd);  // 构造 Range 请求头

        HttpStream stream;  // 每次尝试使用独立的 HTTP 连接
        HttpResult result;  // 响应状态、Content-Range、Content-Length 等
        std::wstring err;  // HTTP 层错误描述
        if (!stream.Open(cfg.url, cfg.http, range, result, err)) {  // 建立连接并发送带 Range 的 GET
            lastError = err;  // 记录错误
            if (attempt + 1 < maxAttempts) Backoff(attempt, ctx.stopFlag);  // 还有剩余尝试则退避后重试
            continue;  // 进入下一次尝试
        }

        if (result.status == 200) {  // 服务器忽略了 Range，返回整个文件
            RecordFatal(ctx, L"服务器忽略了 Range 请求（返回 200），无法分段下载");  // 无法继续分段，整体失败
            break;  // 退出尝试循环
        }
        if (result.status != 206) {  // 非 206 视为异常响应
            lastError = L"HTTP 状态码 " + std::to_wstring(result.status);  // 记录状态码
            if (IsPermanentStatus(result.status)) break;  // 4xx 永久错误不再重试
            if (attempt + 1 < maxAttempts) Backoff(attempt, ctx.stopFlag);  // 临时错误退避重试
            continue;  // 进入下一次尝试
        }
        if (result.rangeTotal >= 0 && result.rangeTotal != ctx.total) {  // 服务器上的文件大小与探测结果不一致
            RecordFatal(ctx, L"服务器上的文件大小已变化，请删除输出文件后重试");  // 继续下载会写错位置
            break;  // 终止
        }
        if (result.rangeStart >= 0 && result.rangeStart != from) {  // 服务器返回的起始位置不符合请求
            lastError = L"服务器返回了不符合请求的 Content-Range";  // 记录错误
            if (attempt + 1 < maxAttempts) Backoff(attempt, ctx.stopFlag);  // 退避后重试
            continue;  // 本次响应不可用
        }

        LARGE_INTEGER offset{};  // 文件写入偏移
        offset.QuadPart = from;  // 定位到本分片当前续传位置
        if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {  // 移动文件指针
            RecordFatal(ctx, L"定位输出文件失败: " + FormatWin32Error(GetLastError()));  // 定位失败无法继续
            break;  // 终止
        }

        ctx.active->fetch_add(1, std::memory_order_relaxed);  // 活跃连接数 +1，用于进度展示
        long long remaining = ctx.chunkEnd - from + 1;  // 本次连接还需接收的字节数
        bool ok = true;  // 本次读取流程是否正常结束
        bool writeFailed = false;  // 是否发生写文件错误（不可重试）
        char buffer[64 * 1024];  // 64 KiB 栈上缓冲区，减少系统调用次数

        while (remaining > 0 && !stopped()) {  // 持续接收直到分片结束或被停止
            DWORD got = 0;  // 本次实际读取字节数
            const DWORD capacity = static_cast<DWORD>(  // 本次读取上限
                std::min<long long>(remaining, static_cast<long long>(sizeof(buffer))));  // 本次最多读取 min(剩余, 缓冲区)
            if (!stream.Read(buffer, capacity, got, err)) {  // 从 HTTP 流读取数据
                lastError = err;  // 记录网络错误
                ok = false;  // 标记失败，稍后重试
                break;  // 退出读取循环
            }
            if (got == 0) {  // 连接提前关闭
                lastError = L"连接在数据接收完成前关闭";  // 记录错误
                ok = false;  // 标记失败
                break;  // 退出读取循环
            }

            DWORD written = 0;  // 实际写入文件的字节数
            if (!WriteFile(file, buffer, got, &written, nullptr) || written != got) {  // 写入必须完整
                lastError = L"写入输出文件失败: " + FormatWin32Error(GetLastError());  // 记录写文件错误
                ok = false;  // 标记失败
                writeFailed = true;  // 写文件错误不可通过重试解决
                break;  // 退出读取循环
            }

            done += got;  // 累加本分片已完成字节
            remaining -= got;  // 减少未接收字节
            ctx.done->store(done, std::memory_order_relaxed);  // 发布最新进度（relaxed 足够）
        }
        ctx.active->fetch_sub(1, std::memory_order_relaxed);  // 连接结束，活跃数 -1

        if (ok && remaining == 0) {  // 正常读完整个分片
            CloseHandle(file);  // 关闭文件句柄
            return;  // 本分片完成
        }
        if (writeFailed) {  // 写文件失败
            RecordFatal(ctx, lastError);  // 升级为致命错误
            break;  // 终止尝试
        }
        if (attempt + 1 < maxAttempts) Backoff(attempt, ctx.stopFlag);  // 网络错误且还有尝试次数则退避
    }

    CloseHandle(file);  // 关闭文件句柄
    if (!stopped()) {  // 非任务级停止导致的失败才记录分片错误
        std::lock_guard<std::mutex> lock(*ctx.mutex);  // 加锁保护错误列表
        ctx.errors->push_back(L"分片 " + std::to_wstring(ctx.index + 1) + L" 失败: " +  // 错误信息前半段
                              (lastError.empty() ? L"未知错误" : lastError));  // 下标从 1 开始展示
    }
}

// 单连接顺序下载（服务器不支持 Range 时的回退路径）。
// 每次运行都会截断输出文件，因此不支持断点续传；失败时按 cfg.retries 重试，
// 每次重试前把文件重置为空并重新预分配。返回最终状态。
DownloadStatus RunSequential(const DownloadConfig& cfg, long long total,  // 单连接顺序下载
                             std::chrono::steady_clock::time_point start,  // 整体开始时刻
                             const DownloadHooks& hooks) {  // 进度与消息回调
    const std::wstring statePath = cfg.outputPath + L".dlstate";  // 可能存在的旧状态文件
    DeleteFileW(statePath.c_str());  // 单连接回退模式不支持断点续传

    HANDLE file = CreateFileW(cfg.outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,  // 覆盖创建，允许读共享
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);  // 覆盖创建输出文件
    if (file == INVALID_HANDLE_VALUE) {  // 创建失败
        Fail(hooks, L"无法创建输出文件 " + cfg.outputPath + L": " +  // 报告路径
                   FormatWin32Error(GetLastError()));  // 报告创建失败原因
        return DownloadStatus::Failed;  // 无法继续
    }

    LARGE_INTEGER zero{};  // 偏移 0
    if (total > 0) {  // 已知总大小时预分配文件
        LARGE_INTEGER target{};  // 目标大小
        target.QuadPart = total;  // 设为总字节数
        if (!SetFilePointerEx(file, target, nullptr, FILE_BEGIN) || !SetEndOfFile(file) ||  // 定位到末尾并扩展文件
            !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {  // 扩展到 total 后回到开头
            Fail(hooks, L"预分配文件失败: " + FormatWin32Error(GetLastError()));  // 报告错误
            CloseHandle(file);  // 关闭句柄
            return DownloadStatus::Failed;  // 失败返回
        }
    }

    std::atomic<long long> done{0};  // 本模式只有一个连接，记录已写字节
    std::atomic<int> active{0};  // 活跃连接数（0/1）
    ProgressSampler sampler([&done] { return done.load(std::memory_order_relaxed); },  // 进度来源为已写字节
                            total, &active, 1, hooks);  // 创建进度采样器，连接数为 1
    sampler.Start();  // 启动采样线程

    const int maxAttempts = std::max(0, cfg.retries) + 1;  // 总尝试次数
    bool success = false;  // 是否成功完成
    bool writeFailed = false;  // 是否发生写文件错误
    std::wstring lastError;  // 最近一次错误

    for (int attempt = 0; attempt < maxAttempts && !IsStopped(hooks); ++attempt) {  // 按尝试次数循环，停止时退出
        if (attempt > 0) {  // 重试前重置文件
            Backoff(attempt - 1, hooks.stopFlag);  // 指数退避
            if (IsStopped(hooks)) break;  // 退避期间收到停止请求

            SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);  // 回到文件开头
            SetEndOfFile(file);  // 截断为空
            if (total > 0) {  // 已知大小则重新预分配
                LARGE_INTEGER target{};  // 目标大小
                target.QuadPart = total;  // 设为总字节数
                SetFilePointerEx(file, target, nullptr, FILE_BEGIN);  // 移动到末尾
                SetEndOfFile(file);  // 扩展文件
                SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);  // 回到开头准备写入
            }
            done.store(0, std::memory_order_relaxed);  // 进度清零
        }

        HttpStream stream;  // 本次尝试的 HTTP 连接
        HttpResult result;  // 响应信息
        std::wstring err;  // HTTP 错误描述
        if (!stream.Open(cfg.url, cfg.http, L"", result, err)) {  // 不带 Range 的普通 GET
            lastError = err;  // 记录错误
            continue;  // 重试
        }
        if (result.status == 206) {  // 未请求 Range 却返回 206
            lastError = L"服务器在未请求 Range 时返回了 206";  // 协议异常
            break;  // 不再重试
        }
        if (result.status != 200) {  // 只接受 200
            lastError = L"HTTP 状态码 " + std::to_wstring(result.status);  // 记录状态码
            if (IsPermanentStatus(result.status)) break;  // 永久错误不重试
            continue;  // 临时错误重试
        }

        long long expected = result.contentLength >= 0 ? result.contentLength : total;  // 期望长度：优先用 Content-Length
        if (total >= 0 && result.contentLength >= 0 && result.contentLength != total) {  // 与探测结果不一致
            lastError = L"文件大小与探测结果不一致";  // 记录错误
            break;  // 服务器内容已变化，终止
        }

        active.store(1, std::memory_order_relaxed);  // 标记连接活跃
        long long written = 0;  // 本次已写字节
        bool ok = true;  // 本次读取是否正常结束
        char buffer[64 * 1024];  // 64 KiB 缓冲区

        for (;;) {  // 持续读取直到结束/错误/停止
            if (IsStopped(hooks)) {  // 检查停止标志
                ok = false;  // 视为未完成
                break;  // 退出读取循环
            }
            DWORD got = 0;  // 本次读取字节数
            if (!stream.Read(buffer, sizeof(buffer), got, err)) {  // 从网络读取
                lastError = err;  // 记录网络错误
                ok = false;  // 标记失败
                break;  // 退出读取循环
            }
            if (got == 0) break;  // 对端正常关闭，读取结束

            DWORD wrote = 0;  // 实际写入字节数
            if (!WriteFile(file, buffer, got, &wrote, nullptr) || wrote != got) {  // 写入必须完整
                lastError = L"写入输出文件失败: " + FormatWin32Error(GetLastError());  // 记录写文件错误
                ok = false;  // 标记失败
                writeFailed = true;  // 写文件错误不可重试
                break;  // 退出读取循环
            }
            written += got;  // 累加已写字节
            done.store(written, std::memory_order_relaxed);  // 更新进度
        }
        active.store(0, std::memory_order_relaxed);  // 连接不再活跃

        if (ok) {  // 读取流程正常结束
            if (expected >= 0 && written != expected) {  // 长度与期望不符
                lastError = L"接收的数据长度与 Content-Length 不一致";  // 记录错误
                continue;  // 重试
            }
            LARGE_INTEGER endOffset{};  // 文件结束偏移
            endOffset.QuadPart = written;  // 设为实际写入长度
            SetFilePointerEx(file, endOffset, nullptr, FILE_BEGIN);  // 定位到写入末尾
            SetEndOfFile(file);  // 截断多余预分配空间
            success = true;  // 标记成功
            break;  // 结束尝试循环
        }
        if (writeFailed) break;  // 写文件错误直接放弃
    }

    sampler.Stop();  // 停止进度采样线程
    CloseHandle(file);  // 关闭输出文件

    if (success) {  // 下载成功
        PrintSummary(hooks, cfg, total > 0 ? total : 0, 1, start);  // 输出完成摘要
        return DownloadStatus::Completed;  // 返回完成
    }
    if (IsInterrupted(hooks)) {  // 用户主动中断（任务级或全局）
        Info(hooks, L"已中断。单连接模式不支持断点续传，重新运行会从头开始。");  // 明确告知无法续传
        return DownloadStatus::Interrupted;  // 返回中断状态
    }
    Fail(hooks, lastError.empty() ? L"下载失败" : lastError);  // 报告最终错误
    return DownloadStatus::Failed;  // 返回失败
}

}  // namespace

// 注册控制台中断处理函数；TRUE 表示追加到处理链而非覆盖。
void InstallInterruptHandler() { SetConsoleCtrlHandler(CtrlHandler, TRUE); }  // 注册控制台事件处理

// 从任意线程请求停止：同时标记“用户中断”和“全局停止”，断点状态由主流程保存。
void RequestDownloadStop() {  // 从任意线程请求停止下载
    g_interrupted.store(true);  // 区分于普通错误导致的中断
    g_stop.store(true);  // 通知所有工作线程退出
}

// 下载主流程。
// 步骤：重置全局状态（若需要） -> 探测文件大小/Range 支持 -> 计算分片 -> 读取续传状态 ->
// 预分配输出文件 -> 启动状态落盘线程/分片线程/进度采样 -> join -> 根据结果保存或删除状态。
// 参数 cfg 为任务配置，hooks 为进度与消息回调；返回最终状态。
// 线程安全：内部各任务使用 hooks.stopFlag 独立控制；全局 g_stop 仅作为无 per-task 标志时的兜底。
DownloadStatus RunDownload(const DownloadConfig& cfg, const DownloadHooks& hooks) {  // 下载主流程入口
    // 仅在未提供 per-task 标志时才重置全局标志（多任务并发时调用者负责各自初始化）
    if (!hooks.stopFlag) g_stop.store(false);
    if (!hooks.interruptedFlag) g_interrupted.store(false);
    InstallInterruptHandler();  // 确保 Ctrl+C 能被捕获（仅控制台模式有效）

    const auto startTime = std::chrono::steady_clock::now();  // 记录整体开始时间
    const std::wstring statePath = cfg.outputPath + L".dlstate";  // 续传状态文件路径

    Info(hooks, L"目标: " + cfg.url.raw);  // 先输出下载目标

    // 1. 用 "Range: bytes=0-0" 探测文件大小与分段能力
    HttpResult probe;  // 探测响应
    {
        HttpStream stream;  // 临时连接，离开作用域即关闭
        std::wstring err;  // 探测错误
        if (!stream.Open(cfg.url, cfg.http, L"bytes=0-0", probe, err)) {  // 只请求第 0 个字节
            Fail(hooks, err);  // 探测失败直接终止
            return DownloadStatus::Failed;  // 返回失败
        }
    }
    if (IsStopped(hooks)) {  // 探测期间可能被 Ctrl+C 或 GUI 停止
        Info(hooks, L"已中断。");  // 告知用户
        return DownloadStatus::Interrupted;  // 返回中断
    }

    if (probe.status == 401) {  // 需要身份认证
        Fail(hooks, L"服务器要求身份认证（HTTP 401）");  // 报告原因
        return DownloadStatus::Failed;  // 失败
    }
    if (probe.status == 407) {  // 代理需要认证
        Fail(hooks, L"代理要求身份认证（HTTP 407）");  // 报告原因
        return DownloadStatus::Failed;  // 失败
    }
    if (probe.status == 416) {  // Range 不可满足
        if (probe.rangeTotal == 0) {  // 文件长度为 0
            std::wstring ioErr;  // 文件操作错误
            if (!PrepareOutputFile(cfg.outputPath, 0, false, ioErr)) {  // 创建空输出文件
                Fail(hooks, L"无法创建输出文件: " + ioErr);  // 报告错误
                return DownloadStatus::Failed;  // 失败
            }
            Info(hooks, L"服务器上的文件为空，已创建空文件。");  // 告知已完成
            return DownloadStatus::Completed;  // 空文件视为下载完成
        }
        Fail(hooks, L"服务器拒绝 Range 探测（HTTP 416）");  // 非空却拒绝 Range
        return DownloadStatus::Failed;  // 失败
    }
    if (probe.status >= 400 || (probe.status != 200 && probe.status != 206)) {  // 其他异常状态
        Fail(hooks, L"服务器返回 HTTP " + std::to_wstring(probe.status));  // 报告状态码
        return DownloadStatus::Failed;  // 失败
    }

    const bool rangeSupported =  // 是否支持分段下载
        probe.status == 206 && probe.rangeStart == 0 && probe.rangeEnd == 0 &&  // 必须是 206 且区间正好 0-0
        probe.rangeTotal > 0;  // 仅当返回 206 且正好是 0-0、总长已知时才认为支持分段
    long long total = -1;  // 文件总大小
    if (rangeSupported) {  // 支持 Range
        total = probe.rangeTotal;  // 从 Content-Range 取总大小
    } else if (probe.status == 200 && probe.contentLength >= 0) {  // 不支持 Range 但给了 Content-Length
        total = probe.contentLength;  // 使用 Content-Length
    }

    if (total == 0) {  // 空文件
        std::wstring ioErr;  // 文件操作错误
        if (!PrepareOutputFile(cfg.outputPath, 0, false, ioErr)) {  // 创建空文件
            Fail(hooks, L"无法创建输出文件: " + ioErr);  // 报告错误
            return DownloadStatus::Failed;  // 失败
        }
        Info(hooks, L"服务器上的文件为空，已创建空文件。");  // 告知用户
        return DownloadStatus::Completed;  // 完成
    }

    if (!rangeSupported || total < 0) {  // 服务器不支持分段或大小未知
        if (total >= 0) {  // 至少知道大小
            Info(hooks, L"文件大小: " + WidenUtf8(FormatBytes(total)));  // 显示大小
        }
        Info(hooks, L"服务器不支持 Range 请求，使用单连接顺序下载。");  // 说明回退策略
        return RunSequential(cfg, total, startTime, hooks);  // 走单连接流程
    }

    Info(hooks, L"文件大小: " + WidenUtf8(FormatBytes(total)));  // 分段模式：显示文件大小

    // 2. 计算分片
    int workers = std::max(1, cfg.threads);  // 线程数至少为 1
    if (total < kSegmentThreshold) {  // 小文件不分片
        workers = 1;  // 单连接即可
    } else {  // 大文件：按线程数分片
        long long byMinChunk = total / kMinChunkSize;  // 按最小分片 256 KiB 计算最多可分几片
        if (byMinChunk < 1) byMinChunk = 1;  // 防御性兜底
        if (workers > byMinChunk) workers = static_cast<int>(byMinChunk);  // 不允许分片小于最小值
    }
    long long chunkSize = (total + workers - 1) / workers;  // 向上取整计算每片大小
    workers = static_cast<int>((total + chunkSize - 1) / chunkSize);  // 按实际片大小回算真实线程数（末片可能更小）

    std::vector<long long> starts(static_cast<size_t>(workers));  // 每个分片的起始偏移
    std::vector<long long> ends(static_cast<size_t>(workers));  // 每个分片的结束偏移（含）
    for (int i = 0; i < workers; ++i) {  // 逐片计算边界
        starts[static_cast<size_t>(i)] = static_cast<long long>(i) * chunkSize;  // 起始 = 下标 * 片大小
        ends[static_cast<size_t>(i)] =  // 结束偏移（含）
            std::min(total - 1, starts[static_cast<size_t>(i)] + chunkSize - 1);  // 结束不越过文件末尾
    }

    std::vector<std::atomic<long long>> done(static_cast<size_t>(workers));  // 每个分片已完成的原子计数
    for (auto& value : done) value.store(0, std::memory_order_relaxed);  // 初始清零

    // 3. 读取断点状态
    bool resumed = false;  // 是否成功复用旧进度
    if (cfg.resume) {  // 启用续传才尝试读取
        ResumeState state;  // 旧状态
        const bool matched =  // 旧状态是否与本次任务完全匹配
            ReadStateFile(statePath, state) && state.url == cfg.url.raw &&  // 能读出状态且 URL 一致
            state.total == total && state.chunkSize == chunkSize &&  // 总大小与分片大小一致
            static_cast<int>(state.done.size()) == workers &&  // 分片数量一致
            FileSizeEquals(cfg.outputPath, total);  // URL/大小/分片方案/分片数/文件大小全部一致才可续传
        if (matched) {  // 可以续传
            long long already = 0;  // 旧进度总和
            for (int i = 0; i < workers; ++i) {  // 逐片恢复
                const long long length =  // 本片长度
                    ends[static_cast<size_t>(i)] - starts[static_cast<size_t>(i)] + 1;  // 本片长度
                long long value = state.done[static_cast<size_t>(i)];  // 旧进度值
                value = std::max(0LL, std::min(value, length));  // 夹在 [0, length] 内，防止损坏数据越界
                done[static_cast<size_t>(i)].store(value, std::memory_order_relaxed);  // 恢复到原子计数
                already += value;  // 累加总进度
            }
            resumed = true;  // 标记为续传
            if (already > 0) {  // 有实际进度时提示
                Info(hooks, L"发现未完成的下载，从断点继续（已完成 " +  // 提示续传
                             FormatPercent(already, total) + L"）。");  // 显示完成百分比
            }
        } else {  // 状态不匹配
            DeleteFileW(statePath.c_str());  // 状态不匹配，删除避免误导
        }
    } else {  // 未启用续传
        DeleteFileW(statePath.c_str());  // 未启用续传，清理旧状态
    }

    std::wstring ioErr;  // 文件准备错误
    if (!PrepareOutputFile(cfg.outputPath, total, resumed, ioErr)) {  // 预分配/保留输出文件
        Fail(hooks, L"无法准备输出文件 " + cfg.outputPath + L": " + ioErr);  // 报告错误
        return DownloadStatus::Failed;  // 失败
    }

    if (SumDone(done) >= total) {  // 所有分片在状态文件中都已完成
        DeleteFileW(statePath.c_str());  // 无需续传，删除状态
        PrintSummary(hooks, cfg, total, workers, startTime);  // 输出完成摘要
        return DownloadStatus::Completed;  // 完成
    }

    Info(hooks, L"启用 " + std::to_wstring(workers) + L" 个连接进行分段下载。");  // 告知实际连接数

    // 4. 断点状态定期落盘
    std::atomic<bool> saverStop{false};  // 状态保存线程的停止标志
    std::thread saver;  // 定期保存 .dlstate 的线程
    if (cfg.resume) {  // 仅续传模式需要
        saver = std::thread([&] {  // 启动状态保存线程
            long long lastSaved = -1;  // 上次保存时的进度总和，避免无变化时重复写盘
            while (!saverStop.load(std::memory_order_relaxed)) {  // 循环直到主流程要求停止
                for (int i = 0; i < 10 && !saverStop.load(std::memory_order_relaxed); ++i) {  // 分 10 次睡，共约 1 秒
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 每次 100ms，便于快速响应停止
                }
                if (saverStop.load(std::memory_order_relaxed)) break;  // 收到停止请求
                const long long current = SumDone(done);  // 读取当前总进度
                if (current != lastSaved) {  // 进度有变化才写盘，减少磁盘 I/O
                    SaveStateFile(cfg.outputPath, cfg.url.raw, total, chunkSize, done);  // 写状态文件
                    lastSaved = current;  // 记录已保存的进度
                }
            }
        });
    }

    // 5. 启动工作线程与进度显示
    std::atomic<int> active{0};  // 当前活跃连接数
    std::mutex errorMutex;  // 保护 errors 与 fatal
    std::vector<std::wstring> errors;  // 各分片的非致命错误
    std::wstring fatal;  // 第一条致命错误

    struct ThreadSpec {  // 启动线程前使用的分片描述
        int index = 0;  // 分片下标
        long long start = 0;  // 起始偏移
        long long end = 0;  // 结束偏移
    };
    std::vector<ThreadSpec> specs;  // 所有分片描述
    specs.reserve(static_cast<size_t>(workers));  // 预留空间，避免扩容
    for (int i = 0; i < workers; ++i) {  // 构造每个分片描述
        ThreadSpec spec;  // 临时对象
        spec.index = i;  // 下标
        spec.start = starts[static_cast<size_t>(i)];  // 起始
        spec.end = ends[static_cast<size_t>(i)];  // 结束
        specs.push_back(spec);  // 加入列表
    }

    std::vector<std::thread> threads;  // 工作线程列表
    threads.reserve(static_cast<size_t>(workers));  // 预留空间
    bool threadCreateFailed = false;  // 是否发生线程创建失败
    try {  // 线程创建可能抛出 std::system_error
        for (const ThreadSpec& spec : specs) {  // 为每个分片创建线程
            WorkerContext ctx;  // 构造线程上下文
            ctx.cfg = &cfg;  // 指向配置（生命周期覆盖所有线程）
            ctx.stopFlag = hooks.stopFlag;  // per-task 停止标志指针
            ctx.index = spec.index;  // 分片下标
            ctx.chunkStart = spec.start;  // 分片起始
            ctx.chunkEnd = spec.end;  // 分片结束
            ctx.total = total;  // 总大小，用于校验服务器
            ctx.done = &done[static_cast<size_t>(spec.index)];  // 指向本片的原子进度
            ctx.active = &active;  // 共享活跃连接数
            ctx.mutex = &errorMutex;  // 共享错误锁
            ctx.errors = &errors;  // 共享错误列表
            ctx.fatal = &fatal;  // 共享致命错误
            threads.emplace_back(ChunkWorker, ctx);  // 启动线程并复制上下文
        }
    } catch (const std::exception&) {  // 线程创建可能抛 std::system_error
        threadCreateFailed = true;  // 标记失败
        RequestStop(hooks);  // 让已启动的线程尽快退出
    }

    ProgressSampler sampler([&done] { return SumDone(done); }, total, &active, workers,  // 进度提供者为所有分片之和
                            hooks);  // 进度提供者为所有分片之和
    sampler.Start();  // 启动采样线程

    for (auto& thread : threads) thread.join();  // 等待所有分片线程结束
    sampler.Stop();  // 停止进度采样

    saverStop.store(true, std::memory_order_relaxed);  // 通知状态保存线程退出
    if (saver.joinable()) saver.join();  // 等待其退出

    auto persistState = [&] {  // 根据结果保存/清理续传状态的辅助 lambda
        if (!cfg.resume) {  // 未启用续传
            DeleteFileW(statePath.c_str());  // 删除可能残留的状态文件
            return;  // 结束
        }
        if (!SaveStateFile(cfg.outputPath, cfg.url.raw, total, chunkSize, done)) {  // 写入最新进度
            Fail(hooks, L"保存断点信息失败: " + statePath);  // 写盘失败要提示（不影响已下载数据）
        }
    };

    if (threadCreateFailed) {  // 有线程没创建成功
        persistState();  // 保存已完成的进度
        Fail(hooks, L"创建下载线程失败");  // 报告错误
        return DownloadStatus::Failed;  // 失败
    }

    if (IsInterrupted(hooks)) {  // 用户主动中断（任务级或全局）
        persistState();  // 保存断点
        Info(hooks, L"已中断。重新执行相同的命令即可从断点继续。");  // 提示如何续传
        return DownloadStatus::Interrupted;  // 返回中断
    }

    {
        std::lock_guard<std::mutex> lock(errorMutex);  // 加锁读取致命错误
        if (!fatal.empty()) {  // 存在致命错误
            persistState();  // 保存断点（已完成的片仍可复用）
            Fail(hooks, fatal);  // 报告首条致命错误
            return DownloadStatus::Failed;  // 失败
        }
    }

    if (SumDone(done) >= total) {  // 所有分片都已完成
        DeleteFileW(statePath.c_str());  // 删除状态文件
        PrintSummary(hooks, cfg, total, workers, startTime);  // 输出完成摘要
        return DownloadStatus::Completed;  // 完成
    }

    persistState();  // 未完成：保存断点供下次继续
    {
        std::lock_guard<std::mutex> lock(errorMutex);  // 加锁读取错误列表
        for (size_t i = 0; i < errors.size() && i < 5; ++i) {  // 最多展示 5 条分片错误
            Fail(hooks, errors[i]);  // 逐条上报
        }
        if (errors.size() > 5) {  // 还有更多错误未展示
            Fail(hooks, L"还有 " + std::to_wstring(errors.size() - 5) + L" 个分片失败");  // 汇总剩余数量
        }
    }
    Info(hooks, L"下载未完成。重新执行相同的命令可继续下载。");  // 提示可续传
    return DownloadStatus::Failed;  // 返回失败
}
