#include "url.h"  // 本文件实现的函数声明

#include <windows.h>  // WideCharToMultiByte / MultiByteToWideChar：UTF-16 与 UTF-8 互转

#include <algorithm>  // 标准算法库（本文件未直接调用其中的算法）
#include <cwctype>    // std::towlower：把协议名、参数名转成小写做不敏感比较
#include <cstdint>    // 定宽整数类型（uint16_t 由 url.h 使用）

/*
 * 把 UTF-16 宽字符串转换为 UTF-8 字节串。
 * 参数 s：输入宽字符串。
 * 返回：UTF-8 编码的 std::string；输入为空或转换失败时返回空串。
 * 说明：先查询所需字节数，再按该长度一次性分配并转换，避免截断。
 */
std::string NarrowUtf8(const std::wstring& s) {
    if (s.empty()) return {};  // 空串直接返回，省去两次 API 调用
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),  // 目标代码页 UTF-8，flags=0，输入宽字符及长度
                                  nullptr, 0, nullptr, nullptr);  // 输出缓冲区传 nullptr，仅查询所需字节数
    if (len <= 0) return {};  // 查询失败时返回空串，调用方会得到空输出
    std::string out(static_cast<size_t>(len), '\0');  // 分配 len 个字节（不含额外的结尾 '\0'）
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),  // 第二次调用：输入参数与查询时一致
                        out.data(), len, nullptr, nullptr);  // 把 UTF-8 字节写入已分配缓冲区
    return out;  // 返回转换结果
}

/*
 * 把 UTF-8 字节串转换为 UTF-16 宽字符串。
 * 参数 s：输入字节串。
 * 返回：宽字符串；如果字节串不是合法 UTF-8，则退回系统 ANSI 代码页（CP_ACP）解释；
 *       两次尝试都失败时返回空串。
 */
std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};  // 空串直接返回
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),  // 按严格 UTF-8 解析，遇到非法序列返回失败
                                  static_cast<int>(s.size()), nullptr, 0);  // 输出缓冲区为 nullptr，仅查询宽字符数
    if (len > 0) {  // UTF-8 合法，可以正常转换
        std::wstring out(static_cast<size_t>(len), L'\0');  // 分配所需数量的宽字符
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),  // 第二次调用：使用相同的严格模式
                            static_cast<int>(s.size()), out.data(), len);  // 写入宽字符缓冲区
        return out;  // 返回转换结果
    }
    // 非法 UTF-8 时退回系统代码页
    len = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);  // 按系统 ANSI 代码页查询长度
    if (len <= 0) return {};  // 仍然失败则返回空串
    std::wstring out(static_cast<size_t>(len), L'\0');  // 分配宽字符缓冲区
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), len);  // 按 ANSI 代码页完成转换
    return out;  // 返回回退后的结果
}

namespace {  // 匿名命名空间：以下辅助函数仅在本文件可见

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

/*
 * 返回字符串的小写副本（按当前 C 区域设置，使用 towlower）。
 * 参数 s：输入字符串；返回转换后的新字符串，原串不变。
 */
std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;  // 复制一份，避免修改调用方数据
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));  // 逐字符转小写
    return out;  // 返回副本
}

/*
 * 解析 URL 中的端口号。
 * 参数 text：冒号之后的数字文本；port：输出端口；err：失败时写入中文原因。
 * 返回：true 表示端口在 1..65535 范围内；false 表示为空、含非数字或越界。
 * 说明：逐位累加并在过程中检查上界，避免超长数字撑爆 long。
 */
bool ParsePort(const std::wstring& text, uint16_t& port, std::wstring& err) {
    if (text.empty()) {  // 冒号后没有任何字符，如 "http://host:/path"
        err = L"URL 中的端口号为空";  // 记录具体错误
        return false;  // 解析失败
    }
    long value = 0;  // 累加端口数值
    for (wchar_t ch : text) {  // 逐字符检查并累加
        if (ch < L'0' || ch > L'9') {  // 端口只允许十进制数字
            err = L"URL 中的端口号非法";  // 记录非法字符错误
            return false;  // 解析失败
        }
        value = value * 10 + (ch - L'0');  // 十进制左移累加
        if (value > 65535) {  // 端口是 16 位无符号数，超过即越界
            err = L"URL 中的端口号超出范围";  // 记录越界错误
            return false;  // 解析失败
        }
    }
    if (value <= 0) {  // 端口 0 不是合法的可连接端口
        err = L"URL 中的端口号非法";  // 与非法字符使用同一提示
        return false;  // 解析失败
    }
    port = static_cast<uint16_t>(value);  // 范围已保证安全，可收窄为 uint16_t
    return true;  // 解析成功
}

// 将百分号编码的 UTF-8 字节串解码为宽字符串
/*
 * 解码百分号编码。
 * 参数 in：可能包含 "%XX" 的 UTF-8 字节串。
 * 返回：解码后的宽字符串；非法的 "%" 序列按原样保留。
 * 说明：只解码一次，不处理 '+' 到空格的转换（URL 查询串语义中此处不需要）。
 */
std::wstring PercentDecode(const std::string& in) {
    std::string out;  // 存放解码后的字节
    out.reserve(in.size());  // 解码只会变短或等长，预留原长度即可
    auto hex = [](char c) -> int {  // 把单个十六进制字符转成数值，非法返回 -1
        if (c >= '0' && c <= '9') return c - '0';  // 数字 0-9
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;  // 小写 a-f 对应 10-15
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;  // 大写 A-F 对应 10-15
        return -1;  // 其他字符不是十六进制
    };
    for (size_t i = 0; i < in.size(); ++i) {  // 逐字节扫描输入
        if (in[i] == '%' && i + 2 < in.size()) {  // 是 '%' 且后面至少还有两个字符
            int hi = hex(in[i + 1]);  // 高 4 位
            int lo = hex(in[i + 2]);  // 低 4 位
            if (hi >= 0 && lo >= 0) {  // 两个字符都合法时才解码
                out.push_back(static_cast<char>((hi << 4) | lo));  // 合并成一个字节
                i += 2;  // 跳过已经消费的两个十六进制字符
                continue;  // 继续处理下一个字节
            }
        }
        out.push_back(in[i]);  // 非 '%' 或非法编码：原样保留
    }
    return WidenUtf8(out);  // 把解码得到的 UTF-8 字节串转为宽字符
}

// 去掉值两侧的引号与空白
/*
 * 去掉首尾空白，并在首尾成对出现双引号时剥掉这对引号。
 * 参数 s：输入字符串。
 * 返回：去引号后的文本；只有单侧引号时保持原样。
 */
std::wstring Unquote(const std::wstring& s) {
    std::wstring out = Trim(s);  // 先去掉首尾空白
    if (out.size() >= 2 && out.front() == L'"' && out.back() == L'"') {  // 至少两个字符且首尾都是双引号
        out = out.substr(1, out.size() - 2);  // 截掉首尾各一个引号
    }
    return out;  // 返回处理后的文本
}

// 只保留最后一个路径分隔符之后的部分
/*
 * 去掉路径中的目录部分，只保留最后一段。
 * 参数 s：可能是 "/dir/file" 或 "dir\\file" 形式的文本。
 * 返回：最后一个 '/' 或 '\\' 之后的子串；没有分隔符时返回原串。
 */
std::wstring StripDirectories(const std::wstring& s) {
    size_t pos = s.find_last_of(L"/\\");  // 同时兼容 URL 风格的 '/' 和 Windows 风格的 '\\'
    return pos == std::wstring::npos ? s : s.substr(pos + 1);  // 无分隔符取原串，否则取其后部分
}

// 从查询串中按键（大小写不敏感）取值，值做一次百分号解码
/*
 * 从查询串中取出指定参数的值。
 * 参数 query：不含 '?' 的查询串，例如 "a=1&filename=x.bin"。
 * 参数 key：参数名（必须为小写），比较时忽略大小写。
 * 返回：参数值（已做一次百分号解码）；参数不存在时返回空串；
 *       参数存在但没有 '=' 时也返回空串。
 */
std::wstring GetQueryParam(const std::wstring& query, const std::wstring& key) {
    size_t pos = 0;  // 当前参数对的起始位置
    while (pos <= query.size()) {  // 允许处理空查询串与末尾空参数
        size_t amp = query.find(L'&', pos);  // 下一个 '&' 分隔符
        std::wstring pair = query.substr(  // 截取当前 "name=value" 片段
            pos, amp == std::wstring::npos ? std::wstring::npos : amp - pos);  // 没有 '&' 时取到末尾
        size_t eq = pair.find(L'=');  // 参数名与值的分隔位置
        std::wstring name = eq == std::wstring::npos ? pair : pair.substr(0, eq);  // 没有 '=' 时整个片段都是参数名
        if (ToLower(Trim(name)) == key) {  // 参数名去空白并转小写后与目标键比较
            if (eq == std::wstring::npos) return L"";  // 只有名字没有值，视为空值
            return PercentDecode(NarrowUtf8(pair.substr(eq + 1)));  // 值转 UTF-8 后做一次百分号解码
        }
        if (amp == std::wstring::npos) break;  // 已经是最后一个参数，结束循环
        pos = amp + 1;  // 跳到下一个参数对
    }
    return L"";  // 未找到目标参数
}

// 从 Content-Disposition 值中提取文件名，优先 filename*=（RFC 5987）
/*
 * 解析 Content-Disposition（或 response-content-disposition 查询参数）中的文件名。
 * 参数 cd：形如 "attachment; filename=\"a.zip\"; filename*=UTF-8''a.zip" 的文本。
 * 返回：提取出的文件名（已剥离目录）；没有 filename 信息时返回空串。
 * 说明：优先使用支持非 ASCII 的 filename*=（RFC 5987），其次才是普通 filename=。
 */
std::wstring FileNameFromContentDisposition(const std::wstring& cd) {
    std::wstring lower = ToLower(cd);  // 小写副本，用于大小写不敏感地定位关键字

    size_t star = lower.find(L"filename*=");  // 查找扩展参数 filename*=
    if (star != std::wstring::npos) {  // 存在扩展参数时优先使用
        std::wstring value = cd.substr(star + 10);  // "filename*=" 共 10 个字符，从其后取值（保留原始大小写）
        size_t semi = value.find(L';');  // 值可能被后续参数用 ';' 分隔
        if (semi != std::wstring::npos) value.resize(semi);  // 只保留第一个分号之前的部分
        value = Unquote(value);  // 去掉可能存在的双引号与空白
        // 形如 UTF-8''<percent-encoded>
        size_t tick = value.rfind(L"''");  // 查找字符集与语言标记之间的两个单引号
        if (tick != std::wstring::npos) value = value.substr(tick + 2);  // 丢弃 "UTF-8''" 前缀，只留编码后的文件名
        // 内层按规范是百分号编码，需要再解码一次
        return StripDirectories(PercentDecode(NarrowUtf8(value)));  // 转 UTF-8 -> 百分号解码 -> 去掉目录部分
    }

    size_t pos = lower.find(L"filename=");  // 回退到普通 filename= 参数
    if (pos != std::wstring::npos) {  // 找到了普通参数
        std::wstring value = cd.substr(pos + 9);  // "filename=" 共 9 个字符
        size_t semi = value.find(L';');  // 同样在分号处截断
        if (semi != std::wstring::npos) value.resize(semi);  // 丢弃后续参数
        return StripDirectories(Unquote(value));  // 去引号/空白后再去掉目录
    }
    return L"";  // 两种形式都不存在
}

}  // namespace

/*
 * 解析 http:// 或 https:// URL。
 * 参数 input：用户输入的 URL，首尾空白会被忽略。
 * 参数 out：成功时写入 secure/host/port/path/raw；失败时可能已写入部分字段。
 * 参数 err：失败时写入中文错误原因。
 * 返回：true 表示解析成功；false 表示缺少协议、协议不支持、主机为空、IPv6 格式错误或端口非法。
 * 支持：http/https、IPv6 字面量（[::1]）、显式端口、查询串（保留在 path 中）、片段（丢弃）。
 */
bool ParseUrl(const std::wstring& input, Url& out, std::wstring& err) {
    std::wstring s = Trim(input);  // 去掉用户输入首尾的空白
    size_t schemeEnd = s.find(L"://");  // 协议与主机之间的分隔符
    if (schemeEnd == std::wstring::npos) {  // 没有 "://" 说明不是绝对 URL
        err = L"URL 必须以 http:// 或 https:// 开头";  // 给出明确提示
        return false;  // 解析失败
    }

    std::wstring scheme = ToLower(s.substr(0, schemeEnd));  // 取出协议名并转小写，实现大小写不敏感
    if (scheme == L"http") {  // 普通 HTTP
        out.secure = false;  // 不启用 TLS
    } else if (scheme == L"https") {  // 加密 HTTP
        out.secure = true;  // 后续 WinHTTP 请求需要安全标志
    } else {  // 其他协议（ftp、file 等）不支持
        err = L"不支持的协议: " + s.substr(0, schemeEnd);  // 错误信息保留用户原始大小写
        return false;  // 解析失败
    }

    std::wstring rest = s.substr(schemeEnd + 3);  // 跳过 "://"，剩下 authority + path + query + fragment
    size_t fragment = rest.find(L'#');  // 片段标识符不会发送给服务器
    if (fragment != std::wstring::npos) rest.resize(fragment);  // 截掉 '#' 及其后的内容

    size_t pathPos = rest.find_first_of(L"/?");  // authority 在第一个 '/' 或 '?' 之前结束
    std::wstring authority = (pathPos == std::wstring::npos) ? rest : rest.substr(0, pathPos);  // 提取主机与端口部分
    if (pathPos == std::wstring::npos) {  // URL 中没有任何路径
        out.path = L"/";  // HTTP 请求目标至少要有一个 '/'
    } else if (rest[pathPos] == L'?') {  // 直接以查询串开头，例如 "http://host?a=1"
        out.path = L"/" + rest.substr(pathPos);  // 补上前导 '/'，查询串原样保留
    } else {
        out.path = rest.substr(pathPos);  // 从 '/' 开始取路径（含可能的查询串）
    }
    if (out.path.empty()) out.path = L"/";  // 防御性兜底，保证请求目标非空

    if (authority.empty()) {  // 例如 "http:///path" 没有主机名
        err = L"URL 缺少主机名";  // 记录错误
        return false;  // 解析失败
    }

    out.port = out.secure ? 443 : 80;  // 先填入协议默认端口，后面若显式指定再覆盖
    if (authority.front() == L'[') {  // IPv6 字面量
        size_t close = authority.find(L']');  // 找到右方括号
        if (close == std::wstring::npos) {  // 缺少右方括号
            err = L"URL 中的 IPv6 地址格式错误";  // 记录错误
            return false;  // 解析失败
        }
        out.host = authority.substr(1, close - 1);  // 方括号内是 IPv6 地址，不含括号
        if (close + 1 < authority.size()) {  // 右方括号后还有内容，应该是 ":port"
            if (authority[close + 1] != L':' ||  // 必须是冒号
                !ParsePort(authority.substr(close + 2), out.port, err)) {  // 冒号后必须是合法端口
                if (err.empty()) err = L"URL 中的 IPv6 端口格式错误";  // ParsePort 未设置错误时补一条格式提示
                return false;  // 解析失败
            }
        }
    } else {  // 普通主机名或 IPv4
        size_t colon = authority.rfind(L':');  // 最后一个冒号用于分隔端口（主机名本身不含冒号）
        if (colon != std::wstring::npos) {  // 存在显式端口
            if (!ParsePort(authority.substr(colon + 1), out.port, err)) return false;  // 解析端口失败则整体失败
            out.host = authority.substr(0, colon);  // 冒号之前是主机名
        } else {
            out.host = authority;  // 没有端口，整个 authority 都是主机名
        }
    }

    if (out.host.empty()) {  // 例如 "http://:8080/path"
        err = L"URL 缺少主机名";  // 记录错误
        return false;  // 解析失败
    }

    out.raw = s;  // 保存去空白后的完整原始 URL，供断点续传校验
    return true;  // 解析成功
}

/*
 * 从 URL 推导默认输出文件名。
 * 参数 rawUrl：原始 URL（可带签名查询参数）。
 * 返回：安全可用的 Windows 文件名；所有来源都为空时返回 "download.bin"。
 * 优先级：response-content-disposition > filename/fname/attname/download 参数
 *         > 路径最后一段 > "download.bin"。
 * 后处理：把控制字符与 Windows 非法字符替换为 '_'，并去掉结尾的 '.' 和空格。
 */
std::wstring DefaultFileNameFromUrl(const std::wstring& rawUrl) {
    std::wstring name;  // 逐步推导出的文件名，空表示尚未找到
    Url parsed;  // 复用 ParseUrl 的结果
    std::wstring err;  // ParseUrl 的错误输出，此处不关心具体原因
    if (ParseUrl(rawUrl, parsed, err)) {  // 只有 URL 能解析时才尝试从各字段推导
        std::wstring path = parsed.path;  // path 中可能包含查询串
        std::wstring query;  // 查询串部分
        size_t queryPos = path.find(L'?');  // 定位 '?' 分隔符
        if (queryPos != std::wstring::npos) {  // 存在查询串
            query = path.substr(queryPos + 1);  // '?' 之后是查询串
            path.resize(queryPos);  // path 只保留 '?' 之前的路径部分
        }

        // 1. 对象存储签名链接: response-content-disposition=attachment;filename=xxx
        //    （华为云 OBS / AWS S3 / 阿里云 OSS 常用）
        std::wstring cd = GetQueryParam(query, L"response-content-disposition");  // 查询签名链接中的文件名声明
        if (!cd.empty()) name = FileNameFromContentDisposition(cd);  // 从 Content-Disposition 语法中提取文件名

        // 2. 常见的直接文件名参数
        if (name.empty()) {  // 上一步没有结果时才尝试直接参数
            const wchar_t* keys[] = {L"filename", L"fname", L"attname", L"download"};  // 按优先级排列的常见参数名
            for (const wchar_t* key : keys) {  // 依次查询，先命中者优先
                name = GetQueryParam(query, key);  // 参数值已做百分号解码
                if (!name.empty()) break;  // 找到非空值即停止
            }
            if (!name.empty()) name = StripDirectories(name);  // 防止参数中夹带路径
        }

        // 3. 回退到路径最后一段
        if (name.empty()) {  // 查询参数都没有提供文件名
            size_t slash = path.rfind(L'/');  // 最后一个路径分隔符
            if (slash != std::wstring::npos) path = path.substr(slash + 1);  // 只保留最后一段
            name = PercentDecode(NarrowUtf8(path));  // 路径可能被百分号编码（如 %E6%96%87），需要解码
        }
    }
    if (name.empty()) return L"download.bin";  // 完全推导不出时使用固定兜底名

    // 替换 Windows 文件名中不允许出现的字符
    const wchar_t* invalid = L"<>:\"/\\|?*";  // Windows 保留字符集合
    for (wchar_t& ch : name) {  // 逐字符清洗
        if (ch < 32 || wcschr(invalid, ch) != nullptr) ch = L'_';  // 控制字符或保留字符统一替换为下划线
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {  // Windows 不允许文件名以点或空格结尾
        name.pop_back();  // 反复删除结尾的 '.' 与 ' '
    }
    if (name.empty()) return L"download.bin";  // 清洗后可能变成空串，再用兜底名
    return name;  // 返回安全文件名
}
