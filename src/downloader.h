#pragma once  // 头文件保护：同一编译单元内只展开一次，防止重复定义

#include <atomic>    // 标准库：std::atomic 用于跨线程的停止标志指针
#include <functional>  // 标准库：std::function 用于保存进度与消息回调
#include <string>  // 标准库：std::wstring 用于保存宽字符路径与提示文本

#include "http.h"  // 项目内 HTTP 封装：HttpOptions / HttpResult / HttpStream
#include "url.h"  // 项目内 URL 类型：保存解析后的地址信息

// 下载任务的完整配置，由调用方填充后传给 RunDownload。
// 结构体按值语义使用，本身不涉及线程安全；字段含义见行尾注释。
struct DownloadConfig {  // 下载任务配置
    Url url;                  // 待下载的 URL（协议、主机、路径等已解析）
    std::wstring outputPath;  // 输出文件路径，直接交给 Win32 文件 API
    int threads = 8;          // 期望的分段线程数，实际值还受文件大小与最小分片限制
    int retries = 5;          // 每个分片的失败重试次数，总尝试次数为 retries + 1
    bool resume = true;       // 是否启用断点续传：读写 outputPath + ".dlstate"
    HttpOptions http;         // HTTP 选项：超时、代理、请求头、TLS 校验等
};

// 一次进度采样的快照
// 由 ProgressSampler 周期构造并通过 onProgress 回调交给界面；
// 结构体按值传递，回调返回后即失效，回调方无需加锁。
struct ProgressInfo {  // 进度快照
    long long done = 0;     // 已下载字节
    long long total = -1;   // 总字节，-1 表示未知
    double speedBps = 0.0;  // 平滑后的速度（字节/秒）
    int active = 0;         // 当前活跃连接数
    int workers = 1;        // 总连接数
};

// 界面回调。回调在工作线程上下文触发，必须快速返回、不得直接操作 UI。
// onProgress 约每 200ms 调用一次；onMessage 用于输出文本或错误信息。
// stopFlag / interruptedFlag 为可选的 per-task 停止标志指针，为空时退化为进程级全局标志（兼容单任务模式）。
struct DownloadHooks {  // 界面回调集合
    // 约每 200ms 上报一次进度
    std::function<void(const ProgressInfo&)> onProgress;  // 进度回调，可为空
    // 文本消息（isError 为 true 表示错误）
    std::function<void(const std::wstring&, bool isError)> onMessage;  // 消息回调，可为空

    // per-task 停止标志指针（非空时优先使用，支持多任务并发）
    std::atomic<bool>* stopFlag = nullptr;         // 任务级停止请求
    std::atomic<bool>* interruptedFlag = nullptr;  // 任务级用户中断标记
};

// 下载最终状态，用于区分“完成 / 被中断（可续传） / 失败”三种结局
enum class DownloadStatus {  // 下载最终状态
    Completed,    // 全部数据下载成功，输出文件已就绪
    Interrupted,  // 被 RequestDownloadStop 或 Ctrl+C 中断，断点信息已尽量保存
    Failed,       // 网络、HTTP 或文件错误导致失败
};

// 执行下载。阻塞直到完成、失败或被 RequestDownloadStop / Ctrl+C 中断。
// 参数 cfg 为任务配置；hooks 为进度/消息回调（std::function 可为空）。
// 返回 Completed 表示成功；Interrupted 表示已中断且断点状态已保存；Failed 表示失败。
// 副作用：会创建工作线程并在返回前全部 join，调用线程在此期间被阻塞。
DownloadStatus RunDownload(const DownloadConfig& cfg, const DownloadHooks& hooks);  // 阻塞式下载入口

// 请求停止当前下载（状态会保存，可断点续传）
// 仅设置全局原子标志，可从任意线程调用；实际退出由工作线程轮询完成。
void RequestDownloadStop();  // 请求停止当前下载

// 注册 Ctrl+C / Ctrl+Break 处理（仅设置停止标志，不直接做 I/O）
// 处理函数只写原子变量，避免在控制台信号上下文执行危险操作。
void InstallInterruptHandler();  // 注册 Ctrl+C / Ctrl+Break 处理
