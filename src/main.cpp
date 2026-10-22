#include <windows.h>  // Win32 基础 API：SetConsoleOutputCP、GetStdHandle、GetConsoleMode、GetCommandLineW 等
#include <shellapi.h>  // CommandLineToArgvW：把命令行拆分为宽字符参数数组

#include <algorithm>  // std::max / std::min：计算默认线程数的上下限
#include <chrono>     // std::chrono::steady_clock：非控制台输出时的打印节流计时
#include <cstdio>     // fwrite / fputc / fflush / snprintf / stdout / stderr
#include <cstdlib>    // wcstol：解析整数参数
#include <cwchar>     // 宽字符相关声明（wchar_t 工具）
#include <cwctype>    // std::towlower：把请求头键名转成小写
#include <mutex>      // std::mutex / std::lock_guard：保护控制台输出不被多线程交错
#include <string>     // std::string / std::wstring
#include <thread>     // std::thread::hardware_concurrency：探测逻辑 CPU 数量
#include <vector>     // std::vector：保存参数、请求头列表

#include "downloader.h"  // DownloadConfig / DownloadHooks / ProgressInfo / RunDownload
#include "format.h"      // FormatBytes / FormatSpeed / FormatEta：进度行中的可读文本
#include "http.h"        // HttpOptions：代理、超时等 HTTP 配置
#include "url.h"         // ParseUrl / DefaultFileNameFromUrl / NarrowUtf8

namespace {  // 匿名命名空间：以下符号仅在本翻译单元内可见

/*
 * 命令行帮助文本。
 * 相邻的宽字符串字面量会在编译期拼接成一个完整常量，运行时由 Print(kUsage) 输出。
 * 内容依次为：程序简介、用法、选项列表、示例。
 */
constexpr const wchar_t* kUsage =
    L"cppdl - 多线程文件下载器\n"  // 标题行：程序名与一句话简介
    L"\n"  // 空行
    L"用法:\n"  // 用法小节标题
    L"  cppdl <URL> [选项]\n"  // 基本调用形式
    L"\n"  // 空行
    L"选项:\n"  // 选项小节标题
    L"  -o, --output <文件>       输出文件路径（默认使用 URL 中的文件名）\n"  // 指定输出路径
    L"  -t, --threads <数量>      并发连接数（默认按 CPU 自动选择，上限 32）\n"  // 指定并发连接数
    L"  -r, --retry <次数>        每个分片失败后的重试次数（默认 5）\n"  // 指定分片重试次数
    L"  -p, --proxy <host:port>   使用指定 HTTP 代理\n"  // 指定代理
    L"  -H, --header \"Key: Value\" 附加请求头，可重复使用\n"  // 追加自定义请求头
    L"  -n, --no-resume           忽略已有断点，重新下载\n"  // 禁用断点续传
    L"  -h, --help                显示该帮助\n"  // 显示帮助
    L"\n"  // 空行
    L"示例:\n"  // 示例小节标题
    L"  cppdl https://example.com/file.zip -t 16 -o D:\\downloads\\file.zip\n"  // 示例：指定线程数与输出文件
    L"  cppdl https://example.com/file.zip -H \"User-Agent: Mozilla/5.0\"\n";  // 示例：附加 User-Agent

constexpr int kProgressBarWidth = 24;  // 进度条总格数（'#'、'>'、'-' 合计的宽度）

/*
 * 把一行宽字符串转换为 UTF-8 并写入 stdout，随后换行并立即刷新。
 * 参数 text：要输出的文本。
 * 无返回值；不抛异常（此处只调用 C 标准库输出函数）。
 */
void Print(const std::wstring& text) {
    const std::string utf8 = NarrowUtf8(text);  // 宽字符转 UTF-8，保证中文在 UTF-8 控制台正常显示
    fwrite(utf8.data(), 1, utf8.size(), stdout);  // 按字节写出，避免格式串解析带来的转义问题
    fputc('\n', stdout);  // 输出行尾换行
    fflush(stdout);  // 立即刷新缓冲区，保证消息按调用顺序出现
}

/*
 * 把带 "错误: " 前缀的文本写入 stderr。
 * 参数 text：错误正文（宽字符）。
 * 无返回值；输出后立即刷新，便于用户第一时间看到失败原因。
 */
void PrintError(const std::wstring& text) {
    const std::string utf8 = NarrowUtf8(L"错误: " + text);  // 统一加中文前缀后转 UTF-8
    fwrite(utf8.data(), 1, utf8.size(), stderr);  // 写入标准错误流，便于与正常输出分离
    fputc('\n', stderr);  // 错误信息单独成行
    fflush(stderr);  // 立即刷新，避免崩溃时丢失错误信息
}

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
 * 返回字符串的小写副本（使用 towlower，按当前 C 区域设置）。
 * 参数 s：输入字符串；返回值是新字符串，原串不变。
 */
std::wstring ToLower(std::wstring s) {
    for (wchar_t& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));  // 逐个字符转小写
    return s;  // 返回修改后的副本
}

/*
 * 把十进制字符串解析为 int。
 * 参数 text：待解析文本；value：输出参数，仅在返回 true 时写入。
 * 返回：true 表示整串都是合法十进制整数且落在 ±1000000 内；false 表示格式错误或越界。
 * 说明：用 wcstol 解析后检查 end 是否到达字符串末尾，拒绝 "12abc" 这类部分合法的输入。
 */
bool ParseInt(const std::wstring& text, int& value) {
    if (text.empty()) return false;  // 空串无法解析
    wchar_t* end = nullptr;  // wcstol 会把首个非法字符的位置写到这里
    long parsed = wcstol(text.c_str(), &end, 10);  // 以 10 进制解析，允许前导正负号
    if (end == text.c_str() || *end != L'\0' || parsed < -1000000 || parsed > 1000000) {  // 没读到数字、尾部有多余字符或明显越界
        return false;  // 视为非法参数
    }
    value = static_cast<int>(parsed);  // 上面的范围检查保证收窄到 int 不会溢出
    return true;  // 解析成功
}

/*
 * 根据 CPU 逻辑核心数推算默认并发连接数。
 * 取 2 倍核心数，再夹在 [4, 32] 区间：
 * 小机器保证至少 4 个连接，大机器避免开过多连接压垮服务器或本机。
 * 无参数；返回 4 到 32 之间的线程数。
 */
int AutoThreadCount() {
    unsigned hardware = std::thread::hardware_concurrency();  // 逻辑处理器数量；无法检测时返回 0
    if (hardware == 0) hardware = 4;  // 探测失败时按 4 核心估算
    int count = static_cast<int>(hardware) * 2;  // 每个核心开两个连接，提高带宽利用率
    count = std::max(4, std::min(32, count));  // 先限制上限 32，再保证下限 4
    return count;  // 返回最终线程数
}

/*
 * 判断标准输出是否连接到真正的控制台（而不是被重定向到文件或管道）。
 * 无参数；返回 true 表示可以用 '\r' 原地刷新进度，false 表示应改为按行、低频输出。
 */
bool IsConsoleOutput() {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);  // 取得标准输出句柄
    DWORD mode = 0;  // 接收控制台模式位
    return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;  // 句柄有效且能读到控制台模式即为真控制台
}

/*
 * 构造一行控制台进度文本。
 * 参数 info：下载器在回调中提供的进度快照（已完成字节、总量、速度、连接数等）。
 * 返回：UTF-8 编码的一行文本。
 * 分支：总量已知时显示百分比、进度条与 ETA；总量未知时只显示已下载量和速度。
 */
std::string BuildProgressLine(const ProgressInfo& info) {
    char buffer[256];  // 栈缓冲区：容纳格式化后的数字与单位
    std::string line;  // 最终拼装出的整行文本
    if (info.total > 0) {  // 只有总量已知时才能计算百分比并画进度条
        double percent = 100.0 * static_cast<double>(info.done) /  // 已完成字节数乘以 100
                         static_cast<double>(info.total);  // 再除以总字节数得到百分比
        if (percent > 100.0) percent = 100.0;  // 防御性截断：done 偶尔可能略超 total
        int filled = static_cast<int>(percent * kProgressBarWidth / 100.0);  // 按比例换算成已填充格数
        if (filled > kProgressBarWidth) filled = kProgressBarWidth;  // 填充格数不能超过进度条宽度

        line += '[';  // 进度条左边界
        line.append(static_cast<size_t>(filled), '#');  // 已完成部分用 '#' 填充
        if (filled < kProgressBarWidth) {  // 尚未填满时补一个箭头和剩余 '-'
            line += '>';  // 箭头标出当前进度位置
            line.append(static_cast<size_t>(kProgressBarWidth - filled - 1), '-');  // 剩余未完成部分
        }
        line += "] ";  // 进度条右边界与分隔空格

        const double remaining =  // 剩余字节数，用于估算 ETA
            info.total > info.done ? static_cast<double>(info.total - info.done) : 0.0;  // 已完成超过总量时按 0 处理
        snprintf(buffer, sizeof(buffer), "%5.1f%%  %s / %s  %s  ETA %s  conn %d/%d",  // 百分比、已下载/总量、速度、剩余时间、连接数
                 percent, FormatBytes(info.done).c_str(), FormatBytes(info.total).c_str(),  // 百分比与两个大小文本
                 FormatSpeed(info.speedBps).c_str(),  // 当前下载速度
                 FormatEta(info.speedBps > 0.0 ? remaining / info.speedBps : -1.0).c_str(),  // 速度为 0 时传 -1，表示无法估算
                 info.active, info.workers);  // 活跃连接数与总工作线程数
        line += buffer;  // 追加格式化结果
    } else {  // 总量未知（服务器未给 Content-Length 或未启用分段）
        snprintf(buffer, sizeof(buffer), "%s  %s  conn %d/%d",  // 只显示已下载量、速度和连接数
                 FormatBytes(info.done).c_str(), FormatSpeed(info.speedBps).c_str(),  // 已下载大小与当前速度
                 info.active, info.workers);  // 活跃连接数与总工作线程数
        line += buffer;  // 追加格式化结果
    }
    return line;  // 返回整行文本
}

// 控制台界面：在工作线程的回调中渲染进度与消息
/*
 * 控制台输出协调器。
 * 下载器可能从多个工作线程同时触发回调，这里用互斥锁串行化输出；
 * 真控制台用 '\r' 原地覆盖上一行进度，重定向输出则每 2 秒打印一行，避免刷屏。
 */
struct ConsoleUi {
    ConsoleUi() : console(IsConsoleOutput()), lastPrint(std::chrono::steady_clock::now()) {}  // 初始化：探测控制台能力并记录起始时间

    /*
     * 进度回调：把最新进度渲染到控制台。
     * 参数 info：下载器提供的进度快照。
     * 无返回值；内部加锁，保证多线程输出不交错。
     */
    void OnProgress(const ProgressInfo& info) {
        std::lock_guard<std::mutex> lock(mutex);  // 整个渲染过程持锁，防止其他线程插入输出
        const std::string line = BuildProgressLine(info);  // 先拼好完整行，再一次性写出
        if (console) {  // stdout 是真控制台，可以原地刷新
            std::string output = "\r" + line;  // '\r' 回到行首，覆盖上一帧进度
            if (line.size() < previousLength) {  // 新行比旧行短时会残留旧字符
                output.append(previousLength - line.size(), ' ');  // 用空格擦除残留部分
            }
            fwrite(output.data(), 1, output.size(), stdout);  // 写出整帧内容
            fflush(stdout);  // 立即刷新，让进度实时可见
            previousLength = line.size();  // 记录本帧长度，供下次擦除使用
        } else {  // 输出被重定向，不能用 '\r' 覆盖
            const auto now = std::chrono::steady_clock::now();  // 取当前单调时钟时间
            if (now - lastPrint >= std::chrono::seconds(2)) {  // 至少间隔 2 秒才打印一次
                fwrite(line.data(), 1, line.size(), stdout);  // 输出一行进度
                fputc('\n', stdout);  // 行尾换行
                fflush(stdout);  // 立即刷新
                lastPrint = now;  // 更新上次打印时间
            }
        }
    }

    /*
     * 消息回调：输出提示或错误信息。
     * 参数 text：消息正文；isError：true 时写入 stderr，否则写入 stdout。
     * 无返回值；如果上一帧进度还在同一行，会先换行，避免消息与进度粘连。
     */
    void OnMessage(const std::wstring& text, bool isError) {
        std::lock_guard<std::mutex> lock(mutex);  // 与进度渲染共用一把锁，保证输出顺序
        if (console && previousLength > 0) {  // 控制台上当前行有未换行的进度
            fputc('\n', stdout);  // 先换行，让消息从新行开始
            previousLength = 0;  // 进度行已被结束，重置长度
        }
        const std::string utf8 = NarrowUtf8(text);  // 消息统一转为 UTF-8
        fwrite(utf8.data(), 1, utf8.size(), isError ? stderr : stdout);  // 按类型选择输出流
        fputc('\n', isError ? stderr : stdout);  // 消息单独成行
        fflush(isError ? stderr : stdout);  // 立即刷新对应输出流
    }

    bool console;  // 是否连接真控制台，决定进度刷新方式
    size_t previousLength = 0;  // 上一帧进度文本长度，用于擦除残留字符
    std::chrono::steady_clock::time_point lastPrint;  // 非控制台模式下上次打印的时间
    std::mutex mutex;  // 保护上述状态与 stdout/stderr 的并发访问
};

}  // namespace

/*
 * 程序入口。
 * 流程：设置控制台编码 -> 拆分命令行 -> 解析选项 -> 组装 DownloadConfig ->
 *       注册进度/消息回调 -> 调用 RunDownload -> 按状态返回退出码。
 * 返回：0 表示下载完成；130 表示被 Ctrl+C 中断；1 表示参数错误或下载失败。
 */
int main() {
    SetConsoleOutputCP(CP_UTF8);  // 让控制台按 UTF-8 解释我们写出的字节，中文才能正常显示

    int argc = 0;  // 接收参数个数
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);  // 取完整命令行并按 Windows 规则拆分
    if (argv == nullptr) {  // 拆分失败（内存不足等）
        PrintError(L"无法解析命令行参数");  // 报告错误
        return 1;  // 以失败退出
    }

    std::vector<std::wstring> args;  // 转存为 std::wstring，后续处理更方便
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);  // 从下标 1 开始，跳过程序自身路径
    LocalFree(argv);  // CommandLineToArgvW 分配的数组需要手动释放

    std::wstring url;  // 位置参数：下载地址
    std::wstring output;  // -o 指定的输出文件路径
    std::wstring proxy;  // -p 指定的代理
    std::vector<std::wstring> headers;  // -H 追加的请求头
    int threads = 0;  // -t 指定的线程数；0 表示自动选择
    int retries = 5;  // 每个分片的重试次数，默认 5
    bool resume = true;  // 是否启用断点续传，-n 会关闭

    for (size_t i = 0; i < args.size(); ++i) {  // 逐项解析命令行
        const std::wstring& arg = args[i];  // 当前参数
        if (arg == L"-h" || arg == L"--help") {  // 帮助选项
            Print(kUsage);  // 输出完整帮助
            return 0;  // 帮助属于正常退出
        }

        auto takeValue = [&](const wchar_t* option) -> bool {  // 小工具：检查并消费选项后面的值（按引用捕获 i）
            if (i + 1 >= args.size()) {  // 选项位于末尾，后面没有值
                PrintError(std::wstring(option) + L" 需要一个参数值");  // 报告缺失值
                return false;  // 调用方据此返回 1
            }
            ++i;  // 跳过当前选项，让 i 指向它的值
            return true;  // 值存在
        };

        if (arg == L"-o" || arg == L"--output") {  // 指定输出路径
            if (!takeValue(L"--output")) return 1;  // 缺少值则退出
            output = args[i];  // 消费到的值即为输出路径
        } else if (arg == L"-t" || arg == L"--threads") {  // 指定线程数
            if (!takeValue(L"--threads")) return 1;  // 缺少值则退出
            if (!ParseInt(args[i], threads) || threads < 1 || threads > 128) {  // 必须是 1..128 的整数
                PrintError(L"线程数必须是 1 到 128 之间的整数");  // 报告合法范围
                return 1;  // 参数错误退出
            }
        } else if (arg == L"-r" || arg == L"--retry") {  // 指定重试次数
            if (!takeValue(L"--retry")) return 1;  // 缺少值则退出
            if (!ParseInt(args[i], retries) || retries < 0 || retries > 100) {  // 必须是 0..100 的整数
                PrintError(L"重试次数必须是 0 到 100 之间的整数");  // 报告合法范围
                return 1;  // 参数错误退出
            }
        } else if (arg == L"-p" || arg == L"--proxy") {  // 指定代理
            if (!takeValue(L"--proxy")) return 1;  // 缺少值则退出
            proxy = Trim(args[i]);  // 去掉首尾空白
            if (proxy.compare(0, 7, L"http://") == 0) proxy = proxy.substr(7);  // 去掉 http:// 前缀，WinHTTP 只接受 host:port
            if (proxy.compare(0, 8, L"https://") == 0) proxy = proxy.substr(8);  // 去掉 https:// 前缀（代理本身仍是 HTTP 代理）
            if (proxy.empty()) {  // 去掉前缀后为空说明输入无效
                PrintError(L"代理地址不能为空");  // 报告错误
                return 1;  // 参数错误退出
            }
        } else if (arg == L"-H" || arg == L"--header") {  // 追加自定义请求头
            if (!takeValue(L"--header")) return 1;  // 缺少值则退出
            headers.push_back(Trim(args[i]));  // 去空白后暂存，稍后统一解析
        } else if (arg == L"-n" || arg == L"--no-resume") {  // 禁用断点续传
            resume = false;  // 下载器会忽略已有临时文件
        } else if (!arg.empty() && arg[0] == L'-') {  // 以 '-' 开头但不是已知选项
            PrintError(L"未知选项: " + arg);  // 指出具体未知选项
            Print(kUsage);  // 顺便显示帮助
            return 1;  // 参数错误退出
        } else if (url.empty()) {  // 第一个位置参数作为 URL
            url = arg;
        } else {  // 出现第二个位置参数
            PrintError(L"只能指定一个 URL");  // 报告多余参数
            return 1;  // 参数错误退出
        }
    }

    if (url.empty()) {  // 用户没有提供下载地址
        Print(kUsage);  // 输出帮助引导使用
        return 1;  // 以失败退出
    }

    DownloadConfig config;  // 汇总所有下载参数
    std::wstring parseError;  // 接收 URL 解析错误
    if (!ParseUrl(url, config.url, parseError)) {  // 解析协议、主机、端口、路径
        PrintError(parseError);  // 输出具体解析错误
        return 1;  // 解析失败退出
    }

    config.outputPath = output.empty() ? DefaultFileNameFromUrl(url) : output;  // 未指定 -o 时从 URL 推导文件名
    config.threads = threads > 0 ? threads : AutoThreadCount();  // 未指定 -t 时按 CPU 自动选择
    config.retries = retries;  // 写入每个分片的重试次数
    config.resume = resume;  // 写入是否启用断点续传
    config.http.proxy = proxy;  // 把代理传给 WinHTTP 层

    for (const std::wstring& header : headers) {  // 解析每个 "Key: Value" 形式的请求头
        size_t colon = header.find(L':');  // 找到键与值的分隔冒号
        if (colon == std::wstring::npos || colon == 0) {  // 没有冒号或冒号在开头，格式非法
            PrintError(L"请求头格式应为 \"Key: Value\": " + header);  // 报告原始输入
            return 1;  // 参数错误退出
        }
        std::wstring key = Trim(header.substr(0, colon));  // 冒号前是键，去掉空白
        std::wstring value = Trim(header.substr(colon + 1));  // 冒号后是值，去掉空白
        if (ToLower(key) == L"user-agent") {  // User-Agent 有独立配置项
            config.http.userAgent = value;  // 覆盖默认 User-Agent
        } else {
            config.http.headers.push_back(key + L": " + value);  // 其余请求头原样加入附加头列表
        }
    }

    ConsoleUi ui;  // 创建控制台输出协调器
    DownloadHooks hooks;  // 下载器回调集合
    hooks.onProgress = [&ui](const ProgressInfo& info) { ui.OnProgress(info); };  // 进度回调转交给 ConsoleUi
    hooks.onMessage = [&ui](const std::wstring& text, bool isError) {  // 消息回调转交给 ConsoleUi
        ui.OnMessage(text, isError);  // 根据 isError 选择 stdout 或 stderr
    };

    const DownloadStatus status = RunDownload(config, hooks);  // 阻塞执行下载，直到完成、失败或被中断
    if (status == DownloadStatus::Completed) return 0;  // 全部完成：成功退出码
    if (status == DownloadStatus::Interrupted) return 130;  // 被 Ctrl+C 中断：沿用 shell 的 128+SIGINT 约定
    return 1;  // 其他情况（失败、参数/网络错误）返回 1
}
