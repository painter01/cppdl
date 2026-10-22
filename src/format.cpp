#include "format.h"  // 本文件实现的函数声明

#include <cmath>   // std::isfinite：判断 ETA 的秒数是否为有限值
#include <cstdio>  // snprintf：把数字格式化进栈上缓冲区

#include "url.h"  // WidenUtf8：把 UTF-8 结果转成宽字符，供 GUI 显示

/*
 * 把字节数格式化为可读的大小文本。
 * 参数 bytes：字节数；负数表示大小未知。
 * 返回：UTF-8 字符串，例如 "0 B"、"1.50 KB"、"2.00 MB"。
 * 处理策略：从 B 开始按 1024 连续换算，最多到 TB，避免出现 EB 等罕见单位。
 */
std::string FormatBytes(long long bytes) {
    if (bytes < 0) return "未知";  // 下载器用 -1 表示长度未知，这里给出中文占位符
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};  // 单位表，下标与换算次数一一对应
    double value = static_cast<double>(bytes);  // 用 double 保存换算后的数值，支持小数
    int unit = 0;  // 当前单位下标，0 表示字节
    while (value >= 1024.0 && unit < 4) {  // 还能再进一级且尚未到 TB 时继续
        value /= 1024.0;  // 除以 1024 换算到下一级单位
        ++unit;  // 单位下标前进一位
    }
    char buffer[64];  // 栈缓冲区：最长输出约 "xxxx.xx TB"，64 字节足够
    if (unit == 0) {  // 仍以字节为单位时保持整数显示
        snprintf(buffer, sizeof(buffer), "%lld B", bytes);  // long long 用 %lld 输出，避免精度损失
    } else {
        snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);  // 其余单位统一保留两位小数
    }
    return buffer;  // 由 char 缓冲区隐式构造 std::string
}

/*
 * 把每秒字节数格式化为下载速度文本。
 * 参数 bytesPerSecond：速度值；非正数或 NaN 会被归零。
 * 返回：UTF-8 字符串，按量级选择 B/s、KB/s、MB/s、GB/s，小数位依次为 0/1/1/2。
 */
std::string FormatSpeed(double bytesPerSecond) {
    if (!(bytesPerSecond > 0.0)) bytesPerSecond = 0.0;  // 用取反写法同时拦下 NaN、0 和负数
    char buffer[64];  // 栈缓冲区，容纳数字与单位
    if (bytesPerSecond < 1024.0) {  // 不足 1 KB/s
        snprintf(buffer, sizeof(buffer), "%.0f B/s", bytesPerSecond);  // 字节速度取整显示
    } else if (bytesPerSecond < 1024.0 * 1024.0) {  // 1 KB/s 到 1 MB/s 之间
        snprintf(buffer, sizeof(buffer), "%.1f KB/s", bytesPerSecond / 1024.0);  // 换算为 KB/s 并保留一位小数
    } else if (bytesPerSecond < 1024.0 * 1024.0 * 1024.0) {  // 1 MB/s 到 1 GB/s 之间
        snprintf(buffer, sizeof(buffer), "%.1f MB/s", bytesPerSecond / (1024.0 * 1024.0));  // 换算为 MB/s
    } else {  // 1 GB/s 及以上
        snprintf(buffer, sizeof(buffer), "%.2f GB/s", bytesPerSecond / (1024.0 * 1024.0 * 1024.0));  // 换算为 GB/s，保留两位小数
    }
    return buffer;  // 隐式转换为 std::string
}

/*
 * 把剩余秒数格式化为 ETA 文本。
 * 参数 seconds：剩余秒数；负数、NaN、无穷大都视为无法估算。
 * 返回：无法估算返回 "--:--"；不足 1 小时返回 "MM:SS"；否则返回 "H:MM:SS"。
 */
std::string FormatEta(double seconds) {
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) return "--:--";  // NaN 与负数让比较失败；无穷大由 isfinite 拦截
    long long total = static_cast<long long>(seconds + 0.5);  // 加 0.5 后截断，实现四舍五入到整秒
    char buffer[32];  // 缓冲区足够放下 "H:MM:SS"
    if (total >= 3600) {  // 超过 1 小时，使用三段式
        snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", total / 3600,  // 小时部分不补零
                 (total % 3600) / 60, total % 60);  // 分钟与秒均补足两位
    } else {
        snprintf(buffer, sizeof(buffer), "%02lld:%02lld", total / 60, total % 60);  // 不足 1 小时只显示分:秒
    }
    return buffer;  // 隐式转换为 std::string
}

/*
 * 计算 part/total 的百分比并保留一位小数。
 * 参数 part：已完成量；total：总量。
 * 返回：宽字符字符串；total <= 0 时直接返回 "0%"（不显示小数）。
 */
std::wstring FormatPercent(long long part, long long total) {
    if (total <= 0) return L"0%";  // 总量未知或为 0，无法计算比例
    double percent = 100.0 * static_cast<double>(part) / static_cast<double>(total);  // 先转 double 再乘除，避免整数除法截断
    char buffer[32];  // 缓冲区容纳 "100.0%" 之类的文本
    snprintf(buffer, sizeof(buffer), "%.1f%%", percent);  // %% 输出字面百分号
    return WidenUtf8(buffer);  // 转为宽字符供 GUI 使用
}
