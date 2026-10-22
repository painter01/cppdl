#include "http.h"  // HttpStream / HttpOptions / HttpResult 的声明

#include <winhttp.h>  // WinHTTP API：WinHttpOpen、WinHttpOpenRequest、WinHttpReadData 等

#include <algorithm>  // std::min：限制单次读取量不超过调用方缓冲区
#include <cwchar>     // wcslen：计算响应头字符串的实际长度
#include <cwctype>    // std::towlower：大小写不敏感地识别 "bytes"

namespace {  // 匿名命名空间：以下辅助函数仅在本翻译单元可见

/*
 * 去掉字符串首尾空白（空格、制表符、回车、换行）。
 * 参数 s：输入字符串。
 * 返回：去空白后的子串；原字符串不被修改。
 */
std::wstring Trim(const std::wstring& s) {
    size_t begin = 0;  // 左边界：从 0 开始向右扫描
    size_t end = s.size();  // 右边界：初始为长度（开区间）
    while (begin < end && (s[begin] == L' ' || s[begin] == L'\t' ||  // 左端仍有字符且当前是空格/制表符
                           s[begin] == L'\r' || s[begin] == L'\n')) {  // 回车和换行也视为空白
        ++begin;  // 左边界右移，跳过空白
    }
    while (end > begin && (s[end - 1] == L' ' || s[end - 1] == L'\t' ||  // 右端仍有字符且末字符是空格/制表符
                           s[end - 1] == L'\r' || s[end - 1] == L'\n')) {  // 回车和换行也视为空白
        --end;  // 右边界左移，跳过空白
    }
    return s.substr(begin, end - begin);  // 取 [begin, end) 区间；全空白时返回空串
}

// 查询任意响应头，返回去首尾空白后的值
/*
 * 按名称查询任意响应头。
 * 参数 request：已收到响应的请求句柄；name：头名称；value：输出值（已去首尾空白）。
 * 返回：true 表示头存在且读取成功；false 表示头不存在或缓冲区读取失败。
 * 说明：WinHTTP 的头名称匹配不区分大小写；先查长度再分配缓冲，避免固定长度截断。
 */
bool QueryHeader(HINTERNET request, LPCWSTR name, std::wstring& value) {
    DWORD length = 0;  // 接收所需缓冲区大小（字节，包含结尾 '\0'）
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER,  // 自定义头名 + 空输出缓冲区：仅查询长度
                        &length, WINHTTP_NO_HEADER_INDEX);  // 按名称查找，不使用头索引
    if (length == 0) return false;  // 长度为 0 说明响应中不存在该头

    std::wstring buffer(length / sizeof(wchar_t) + 1, L'\0');  // 字节数换算为宽字符数，并多留一位余量
    DWORD written = length;  // 传入缓冲区容量，返回时写入实际字节数
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, buffer.data(), &written,  // 第二次调用：真正读取头值
                             WINHTTP_NO_HEADER_INDEX)) {  // 仍然按名称查找
        return false;  // 读取失败（如缓冲区不足）时视为没有该头
    }
    buffer.resize(wcslen(buffer.c_str()));  // 按实际 C 字符串长度收缩，去掉多余字节与结尾 '\0'
    value = Trim(buffer);  // 去掉头值两侧空白后写回
    return true;  // 查询成功
}

/*
 * 从 text 的 pos 位置开始解析一个非负十进制整数。
 * 参数 text：输入字符串；pos：起始位置，成功时前进到数字之后；value：解析结果。
 * 返回：true 表示至少读到一位数字且未溢出；false 表示此处不是数字或已溢出。
 * 说明：仅接受 0-9，不接受符号；用于 Content-Range 的 start/end/total。
 */
bool ParseLongLong(const std::wstring& text, size_t& pos, long long& value) {
    while (pos < text.size() && (text[pos] == L' ' || text[pos] == L'\t')) ++pos;  // 跳过数字前的空格与制表符
    if (pos >= text.size() || text[pos] < L'0' || text[pos] > L'9') return false;  // 此处不是数字，解析失败
    long long result = 0;  // 累加器
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {  // 逐个消费数字字符
        result = result * 10 + (text[pos] - L'0');  // 十进制累加：字符值减去 '0'
        if (result < 0) return false;  // 有符号溢出后变负，说明数值超出范围
        ++pos;  // 移动到下一个字符
    }
    value = result;  // 写回解析结果
    return true;  // 解析成功
}

// 解析 "bytes 0-499/1234" 或 "bytes */1234"
/*
 * 解析 Content-Range 响应头。
 * 参数 value：头值，例如 "bytes 0-499/1234"，或总长未知时的星号形式（见上方原注释）。
 * 参数 result：输出 rangeStart/rangeEnd/rangeTotal，仅当返回 true 时有效。
 * 返回：true 表示格式合法；false 表示前缀、分隔符、数字或区间关系不合法。
 * 说明：total 为 '*' 时记为 -1（长度未知）；start/end 为 0 基闭区间。
 */
bool ParseContentRange(const std::wstring& value, HttpResult& result) {
    std::wstring text = Trim(value);  // 去掉头值两侧空白
    if (text.size() < 5) return false;  // 至少需要 "bytes" 五个字符
    if (text.compare(0, 5, L"bytes") != 0 && text.compare(0, 5, L"BYTES") != 0) return false;  // 前缀只接受全小写或全大写 bytes

    size_t slash = text.find(L'/');  // 总长度位于 '/' 之后
    if (slash == std::wstring::npos) return false;  // 没有 '/' 说明格式不完整

    size_t pos = 5;  // 跳过 "bytes" 前缀
    while (pos < text.size() && (text[pos] == L' ' || text[pos] == L'\t')) ++pos;  // 跳过前缀后的空白

    if (pos < text.size() && text[pos] == L'*') {  // 长度为 0 的资源
        long long total = 0;  // 总长度
        size_t totalPos = slash + 1;  // '/' 之后开始读总长度
        if (!ParseLongLong(text, totalPos, total)) return false;  // 总长度必须是数字
        result.rangeStart = -1;  // '*' 形式没有具体区间起点
        result.rangeEnd = -1;  // 也没有区间终点
        result.rangeTotal = total;  // 记录总长度
        return true;  // 解析成功
    }

    long long start = 0;  // 区间起点（包含）
    long long end = 0;  // 区间终点（包含）
    if (!ParseLongLong(text, pos, start)) return false;  // 读取起点
    if (pos >= text.size() || text[pos] != L'-') return false;  // 起点后必须是 '-'
    ++pos;  // 跳过 '-'
    if (!ParseLongLong(text, pos, end)) return false;  // 读取终点
    if (pos >= text.size() || text[pos] != L'/') return false;  // 终点后必须是 '/'
    ++pos;  // 跳过 '/'

    long long total = -1;  // -1 表示总长度未知
    if (pos < text.size() && text[pos] == L'*') {  // 形如 bytes 0-499/*
        total = -1;  // 明确标记为未知
    } else if (!ParseLongLong(text, pos, total)) {  // 否则总长度必须是数字
        return false;  // 总长度非法
    }
    if (end < start || (total >= 0 && end >= total)) return false;  // 终点不能小于起点；已知总量时 0 基终点必须小于总量

    result.rangeStart = start;  // 写回区间起点
    result.rangeEnd = end;  // 写回区间终点
    result.rangeTotal = total;  // 写回总长度（可能为 -1）
    return true;  // 解析成功
}

/*
 * 判断 Accept-Ranges 头中是否包含 "bytes" 标记（大小写不敏感）。
 * 参数 value：头值，例如 "bytes" 或 "none"。
 * 返回：true 表示服务器声明支持字节范围请求。
 */
bool ContainsBytesToken(const std::wstring& value) {
    std::wstring lower = value;  // 复制一份再转小写，避免修改原值
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));  // 逐字符转小写
    return lower.find(L"bytes") != std::wstring::npos;  // 子串匹配即可，"none" 不会命中
}

// 非 ASCII 和空白字符需要百分号编码，避免 WinHTTP 拒绝该路径
/*
 * 对请求路径做最小化百分号编码。
 * 参数 path：宽字符路径（可能包含中文、空格等）。
 * 返回：百分号编码后的宽字符路径。
 * 规则：先转 UTF-8，再对 <= 0x20 或 >= 0x7F 的字节编码为 %XX；
 *       其余 ASCII（包括已有的 '%'）原样保留，避免对已编码路径二次编码。
 */
std::wstring EncodePath(const std::wstring& path) {
    const std::string utf8 = NarrowUtf8(path);  // 统一转为 UTF-8 字节序列
    static const char* hex = "0123456789ABCDEF";  // 大写十六进制数字表
    std::string encoded;  // 存放编码结果
    encoded.reserve(utf8.size());  // 预留原始长度，减少扩容次数
    for (unsigned char ch : utf8) {  // 按无符号字节遍历，避免 >=0x80 被当作负数
        if (ch <= 0x20 || ch >= 0x7F) {  // 控制字符、空格和所有非 ASCII 字节都需要编码
            encoded.push_back('%');  // 百分号前缀
            encoded.push_back(hex[ch >> 4]);  // 高 4 位对应的十六进制字符
            encoded.push_back(hex[ch & 0x0F]);  // 低 4 位对应的十六进制字符
        } else {
            encoded.push_back(static_cast<char>(ch));  // 可打印 ASCII 原样保留
        }
    }
    return WidenUtf8(encoded);  // 结果全是 ASCII，可安全地按 UTF-8 转回宽字符
}

}  // namespace

/*
 * 把 Win32 / WinHTTP 错误码转换为可读文本。
 * 参数 code：通常来自 GetLastError()。
 * 返回：去掉尾部换行的系统消息；系统消息表查不到时返回 "错误码 N"。
 * 说明：先查系统消息表，失败后再查 winhttp.dll 自带的消息表。
 */
std::wstring FormatWin32Error(DWORD code) {
    wchar_t* buffer = nullptr;  // FormatMessageW 会分配缓冲区并写到这里
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |  // 自动分配缓冲区 + 从系统消息表查找
            FORMAT_MESSAGE_IGNORE_INSERTS,  // 忽略消息中的插入占位符
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),  // 使用系统默认语言
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);  // 输出指针按 API 要求强转，最小缓冲区 0

    if (length == 0) {  // 系统消息表中没有该错误码
        HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");  // 取得已加载的 WinHTTP 模块句柄
        if (winhttp != nullptr) {  // 模块确实已加载
            length = FormatMessageW(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |  // 自动分配缓冲区 + 从指定模块消息表查找
                    FORMAT_MESSAGE_IGNORE_INSERTS,  // 忽略插入占位符
                winhttp, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),  // 在 winhttp.dll 消息表中查找
                reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);  // 同样自动分配输出缓冲区
        }
    }

    std::wstring message;  // 最终返回的文本
    if (length != 0 && buffer != nullptr) {  // 任一次查找成功
        message.assign(buffer, length);  // 按返回长度复制（消息可能不以 '\0' 结尾）
        LocalFree(buffer);  // 释放 FormatMessageW 分配的缓冲区
        message = Trim(message);  // 去掉消息末尾的回车换行
    } else {
        message = L"错误码 " + std::to_wstring(code);  // 查不到时退化为错误码文本
    }
    return message;  // 返回可读消息
}

/*
 * 析构函数：确保离开作用域时释放所有 WinHTTP 句柄。
 */
HttpStream::~HttpStream() { Close(); }

/*
 * 关闭请求、连接、会话三个句柄。
 * 关闭顺序与创建顺序相反（请求 -> 连接 -> 会话），
 * 每个句柄关闭后立即置空，因此 Close 可以安全地重复调用。
 */
void HttpStream::Close() {
    if (request_ != nullptr) {  // 请求句柄存在
        WinHttpCloseHandle(request_);  // 关闭请求
        request_ = nullptr;  // 置空，防止重复关闭
    }
    if (connection_ != nullptr) {  // 连接句柄存在
        WinHttpCloseHandle(connection_);  // 关闭连接
        connection_ = nullptr;  // 置空
    }
    if (session_ != nullptr) {  // 会话句柄存在
        WinHttpCloseHandle(session_);  // 关闭会话
        session_ = nullptr;  // 置空
    }
}

/*
 * 建立 WinHTTP 会话、连接主机并发送 GET 请求。
 * 参数 url：已解析的 URL（主机、端口、路径、是否 https）。
 * 参数 opts：User-Agent、代理、超时与额外请求头。
 * 参数 rangeHeader：Range 头的值（如 "bytes=0-1023"）；为空表示不发送 Range。
 * 参数 result：输出状态码、Content-Length、Content-Range、Accept-Ranges。
 * 参数 err：失败时写入中文错误描述。
 * 返回：true 表示已收到响应且状态码可读；false 表示任一步骤失败（句柄已清理）。
 * 说明：开头先 Close()，因此同一实例可复用；Content-Range 解析失败仅记录 err，
 *       不立即返回 false，调用方仍可依据状态码与 contentLength 决定如何处理。
 */
bool HttpStream::Open(const Url& url, const HttpOptions& opts,
                      const std::wstring& rangeHeader, HttpResult& result,
                      std::wstring& err) {
    Close();  // 释放上一次请求占用的句柄，支持对象复用
    result = HttpResult{};  // 用默认值重置结果，避免残留上一次的状态

    const DWORD accessType = opts.proxy.empty() ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY  // 未指定代理时沿用系统代理设置
                                                : WINHTTP_ACCESS_TYPE_NAMED_PROXY;  // 指定了代理则使用命名代理
    session_ = WinHttpOpen(
        opts.userAgent.empty() ? L"cppdl/1.0" : opts.userAgent.c_str(), accessType,  // User-Agent；空时使用默认值
        opts.proxy.empty() ? WINHTTP_NO_PROXY_NAME : opts.proxy.c_str(),  // 代理地址；无代理时传 WINHTTP_NO_PROXY_NAME
        WINHTTP_NO_PROXY_BYPASS, 0);  // 不使用绕过列表；0 表示同步模式
    if (session_ == nullptr) {  // 会话创建失败
        err = L"初始化 WinHTTP 失败: " + FormatWin32Error(GetLastError());  // 附带 Win32 错误描述
        return false;  // 打开失败
    }

    WinHttpSetTimeouts(session_, opts.connectTimeoutMs, opts.connectTimeoutMs,  // 依次为解析超时、连接超时
                       opts.sendTimeoutMs, opts.receiveTimeoutMs);  // 发送超时、接收超时

    connection_ = WinHttpConnect(session_, url.host.c_str(), url.port, 0);  // 连接目标主机与端口，最后一个参数保留为 0
    if (connection_ == nullptr) {  // 连接失败（DNS 失败、拒绝连接等）
        err = L"连接主机失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        Close();  // 释放已创建的会话句柄
        return false;  // 打开失败
    }

    const std::wstring encodedPath = EncodePath(url.path);  // 对路径做百分号编码，避免空格/中文被 WinHTTP 拒绝
    request_ = WinHttpOpenRequest(connection_, L"GET", encodedPath.c_str(), nullptr,  // 方法 GET、请求目标为编码后的路径、HTTP/1.1
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,  // 不发送 Referer，使用默认 Accept 类型
                                  url.secure ? WINHTTP_FLAG_SECURE : 0);  // https 时启用安全标志
    if (request_ == nullptr) {  // 请求创建失败
        err = L"创建 HTTP 请求失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        Close();  // 释放连接与会话
        return false;  // 打开失败
    }

    std::wstring headers;  // 追加请求头文本，每行以 CRLF 结尾
    if (!rangeHeader.empty()) {  // 调用方要求分段下载
        headers += L"Range: ";  // Range 头名称
        headers += rangeHeader;  // 区间值，如 "bytes=0-1023"
        headers += L"\r\n";  // HTTP 头行结束符
    }
    for (const std::wstring& header : opts.headers) {  // 追加调用方自定义的每个请求头
        headers += header;  // 形如 "Key: Value"
        headers += L"\r\n";  // 每行单独以 CRLF 结束
    }

    if (!WinHttpSendRequest(
            request_,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),  // 无附加头时传专用常量
            static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {  // 宽字符 API 下长度按字符数计；GET 无请求体
        err = L"发送请求失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        Close();  // 释放句柄
        return false;  // 打开失败
    }

    if (!WinHttpReceiveResponse(request_, nullptr)) {  // 等待服务器返回响应头
        err = L"接收响应失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        Close();  // 释放句柄
        return false;  // 打开失败
    }

    DWORD statusCode = 0;  // 接收 HTTP 状态码
    DWORD statusSize = sizeof(statusCode);  // 缓冲区字节数
    if (!WinHttpQueryHeaders(request_,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,  // 查询状态码并要求以数字形式返回
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,  // 状态码是固定项，按索引查询
                             WINHTTP_NO_HEADER_INDEX)) {  // 不需要头索引
        err = L"读取 HTTP 状态码失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        Close();  // 释放句柄
        return false;  // 打开失败
    }
    result.status = static_cast<int>(statusCode);  // 保存状态码供调用方判断 200/206/302

    std::wstring value;  // 临时保存每个响应头的值
    if (QueryHeader(request_, L"Content-Length", value)) {  // 服务器给出了响应体长度
        wchar_t* end = nullptr;  // wcstoll 记录首个非法字符位置
        long long length = wcstoll(value.c_str(), &end, 10);  // 以十进制解析长度
        if (end != value.c_str() && length >= 0) result.contentLength = length;  // 至少读到一个数字且非负才接受
    }
    if (QueryHeader(request_, L"Content-Range", value)) {  // 206 响应会带 Content-Range
        if (!ParseContentRange(value, result)) {  // 解析区间、终点与总长度
            err = L"无法解析 Content-Range: " + value;  // 记录错误，但不算致命失败
        }
    }
    if (QueryHeader(request_, L"Accept-Ranges", value)) {  // 服务器可能声明是否支持范围请求
        result.acceptRanges = ContainsBytesToken(value);  // 头值含 bytes 即视为支持
    }

    return true;  // 已成功收到响应
}

/*
 * 从响应体读取一块数据。
 * 参数 buffer：调用方缓冲区；capacity：缓冲区容量（字节）。
 * 参数 got：输出实际读取字节数；为 0 表示响应体结束。
 * 参数 err：失败时写入中文错误描述。
 * 返回：true 表示调用成功（可能读到 0 字节）；false 表示请求已关闭或 WinHTTP 调用失败。
 * 说明：单次读取量取 available 与 capacity 的较小值，绝不超过调用方缓冲区。
 */
bool HttpStream::Read(char* buffer, DWORD capacity, DWORD& got, std::wstring& err) {
    got = 0;  // 默认没有读到数据
    if (request_ == nullptr) {  // 句柄已关闭或尚未 Open
        err = L"HTTP 请求已关闭";  // 给出明确错误
        return false;  // 读取失败
    }

    DWORD available = 0;  // 当前可从网络层读取的字节数
    if (!WinHttpQueryDataAvailable(request_, &available)) {  // 查询是否有数据可读
        err = L"查询可读数据失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        return false;  // 读取失败
    }
    if (available == 0) return true;  // 没有更多数据：调用方会把 got==0 当作响应体结束

    const DWORD toRead = std::min(available, capacity);  // 本次最多读 capacity，避免写越界
    DWORD read = 0;  // 实际读取到的字节数
    if (!WinHttpReadData(request_, buffer, toRead, &read)) {  // 把数据读入调用方缓冲区
        err = L"读取响应数据失败: " + FormatWin32Error(GetLastError());  // 附带错误描述
        return false;  // 读取失败
    }
    got = read;  // 写回实际读取量
    return true;  // 本次读取成功
}
