#pragma once

#include <windows.h>  // DWORD、HMODULE 等 Win32 基础类型
#include <winhttp.h>  // HINTERNET 与 WinHTTP API 声明

#include <cstdint>  // uint16_t：Url::port 的类型
#include <string>   // std::wstring：配置与错误文本
#include <vector>   // std::vector：额外请求头列表

#include "url.h"  // Url：HttpStream::Open 的输入参数

/*
 * HTTP 会话配置。
 * 下载器为每个线程创建一个 HttpStream，并共享同一份只读配置。
 * 所有超时单位为毫秒。
 */
struct HttpOptions {
    std::wstring userAgent = L"cppdl/1.0";  // User-Agent；为空时 Open 内部回退到 "cppdl/1.0"
    std::wstring proxy;                 // 形如 host:port，为空表示使用系统代理设置
    std::vector<std::wstring> headers;  // 额外的请求头，每项形如 "Key: Value"
    int connectTimeoutMs = 15000;  // 解析主机与建立连接的超时
    int sendTimeoutMs = 30000;  // 发送请求的超时
    int receiveTimeoutMs = 30000;  // 接收响应与数据的超时
};

/*
 * 一次 HTTP 响应的关键元数据。
 * Open 成功返回后由调用方读取，用于判断是否支持分段、总长度以及实际返回的区间。
 */
struct HttpResult {
    int status = 0;  // HTTP 状态码，例如 200、206、302
    long long contentLength = -1;  // Content-Length
    long long rangeStart = -1;     // Content-Range: bytes start-end/total
    long long rangeEnd = -1;  // 实际返回区间的终点（闭区间）
    long long rangeTotal = -1;  // 资源总长度；'*' 或无法解析时为 -1
    bool acceptRanges = false;  // 服务器是否声明支持 Range 请求
};

// 基于 WinHTTP 的同步 GET 流。每个线程应使用独立实例。
/*
 * 封装 WinHTTP 的同步 GET 请求。
 * 生命周期：Open 建立会话/连接/请求 -> Read 反复读取响应体 -> Close（或析构）释放句柄。
 * 线程安全：每个实例只由一个线程使用，内部不加锁。
 */
class HttpStream {
public:
    HttpStream() = default;  // 默认构造：三个句柄均为 nullptr
    ~HttpStream();  // 析构时自动调用 Close，防止句柄泄漏

    HttpStream(const HttpStream&) = delete;  // 禁止拷贝：句柄所有权不能共享
    HttpStream& operator=(const HttpStream&) = delete;  // 禁止赋值：避免重复关闭同一句柄

    // 建立连接并发送 GET 请求；rangeHeader 为空表示不发送 Range 头
    /*
     * 建立连接并发送 GET 请求。
     * 参数 url：目标 URL；opts：代理、超时、User-Agent 等；rangeHeader：Range 头的值（不含 "Range: "）。
     * 参数 result：输出状态码与响应头信息；err：失败时输出中文原因。
     * 返回：true 表示已收到响应；false 表示会话、连接、请求或收发失败。
     */
    bool Open(const Url& url, const HttpOptions& opts, const std::wstring& rangeHeader,
              HttpResult& result, std::wstring& err);

    // 返回 false 表示失败；返回 true 且 got == 0 表示响应体结束
    /*
     * 读取响应体。
     * 参数 buffer/capacity：调用方缓冲区及其容量；got：实际读取字节数（0 表示结束）。
     * 返回：true 表示本次读取完成；false 表示请求已关闭或 WinHTTP 出错。
     */
    bool Read(char* buffer, DWORD capacity, DWORD& got, std::wstring& err);

    void Close();  // 关闭请求、连接、会话三个句柄，可重复调用

private:
    HINTERNET session_ = nullptr;  // WinHTTP 会话句柄
    HINTERNET connection_ = nullptr;  // 到目标主机的连接句柄
    HINTERNET request_ = nullptr;  // 具体 GET 请求句柄
};

// 将 Win32 / WinHTTP 错误码转换为可读文本
/*
 * 把 Win32 / WinHTTP 错误码转换为可读文本。
 * 参数 code：通常来自 GetLastError()。
 * 返回：系统消息；查不到时返回 "错误码 N"。
 */
std::wstring FormatWin32Error(DWORD code);
