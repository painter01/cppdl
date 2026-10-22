#pragma once

#include <string>  // std::string / std::wstring：格式化结果的返回类型

// 通用的大小 / 速度 / 时间格式化，控制台与 GUI 共用
/*
 * 本文件只声明纯格式化函数：不进行 I/O、不持有全局状态，
 * 因此控制台版（main.cpp）和 GUI 版都可以在工作线程中安全调用。
 */

/*
 * 把字节数格式化为可读的大小文本。
 * 参数 bytes：字节数；负数表示大小未知。
 * 返回：UTF-8 字符串，例如 "0 B"、"1.50 KB"、"2.00 MB"。
 * 说明：按 1024 进位，最多换算到 TB；小于 1 KB 时以整数 B 输出。
 */
std::string FormatBytes(long long bytes);

/*
 * 把每秒字节数格式化为下载速度文本。
 * 参数 bytesPerSecond：速度值（字节/秒）；0、负数、NaN 均按 0 处理。
 * 返回：UTF-8 字符串，例如 "512 B/s"、"1.2 MB/s"。
 */
std::string FormatSpeed(double bytesPerSecond);

/*
 * 把剩余秒数格式化为 ETA（预计剩余时间）文本。
 * 参数 seconds：剩余秒数；负数、NaN 或无穷大表示无法估算。
 * 返回：不足 1 小时为 "MM:SS"，达到 1 小时为 "H:MM:SS"；无法估算时返回 "--:--"。
 */
std::string FormatEta(double seconds);

/*
 * 计算 part 占 total 的百分比，保留一位小数。
 * 参数 part：已完成量；total：总量。
 * 返回：宽字符字符串，例如 "42.5%"；total <= 0 时返回 "0%"。
 */
std::wstring FormatPercent(long long part, long long total);
