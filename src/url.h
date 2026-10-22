#pragma once

#include <cstdint>  // uint16_t：端口号的存储类型
#include <string>   // std::string / std::wstring

// 解析后的 URL。host/path 均为宽字符，直接供 WinHTTP 使用。
/*
 * ParseUrl 的输出结构。
 * 注意 path 可能包含查询串（例如 "/a/b?x=1"），HttpStream 会把它原样作为请求目标；
 * raw 保存去掉首尾空白后的原始 URL，供断点续传状态校验使用。
 */
struct Url {
    bool secure = false;  // true 表示 https（WinHTTP 需要 WINHTTP_FLAG_SECURE）
    std::wstring host;  // 主机名或 IPv6 地址字面量（不含方括号）
    uint16_t port = 80;  // 端口；http 默认 80，https 默认 443，可被 URL 显式覆盖
    std::wstring path;  // 请求路径，至少为 "/"；可能带查询串和百分号编码
    std::wstring raw;   // 原始 URL，用于断点续传状态校验
};

// 解析 http:// 或 https:// URL
/*
 * 解析 http:// 或 https:// URL。
 * 参数 input：用户输入的 URL，首尾空白会被忽略。
 * 参数 out：成功时写入解析结果；失败时内容可能已被部分修改。
 * 参数 err：失败时写入中文错误原因。
 * 返回：true 表示解析成功；false 表示缺少协议、协议不支持、主机为空或端口非法等。
 * 说明：支持 IPv6 字面量（形如 http://[::1]:8080/path），未显式指定端口时按协议选择默认端口。
 */
bool ParseUrl(const std::wstring& input, Url& out, std::wstring& err);

// 从 URL 推导默认输出文件名（已做百分号解码与非法字符替换）
/*
 * 从 URL 推导默认输出文件名。
 * 优先级：response-content-disposition 参数 > filename/fname/attname/download 参数
 *         > 路径最后一段 > "download.bin"。
 * 参数 rawUrl：原始 URL。
 * 返回：已做百分号解码、目录剥离和 Windows 非法字符替换的文件名。
 */
std::wstring DefaultFileNameFromUrl(const std::wstring& rawUrl);

// UTF-8 <-> UTF-16 转换
/*
 * UTF-8 与 UTF-16 互转，失败时返回空串（具体回退策略见 .cpp 中的实现说明）。
 */
std::string NarrowUtf8(const std::wstring& s);
std::wstring WidenUtf8(const std::string& s);
