// cppdl_gui - 基于 Win32 原生 API 的多线程下载器窗口（支持同时下载多个链接）
// MSVC 2015 Update 2+: 强制所有字符串字面量以 UTF-8 存储并按 UTF-8
// 解释（无论系统代码页）
#pragma execution_character_set("utf-8")

#include <windows.h> // Win32 核心 API：窗口类、控件、消息循环、PostMessage 等

#include <commctrl.h> // 公共控件库：进度条 PROGRESS_CLASSW、UpDown 微调、ListView 报告视图
#include <commdlg.h> // 通用对话框库：GetSaveFileNameW 弹出“另存为”窗口

#include <algorithm>  // std::max / std::min
#include <atomic>     // std::atomic<bool>，per-task 停止标志
#include <cstdio>     // snprintf
#include <cstdlib>    // std::wcstol
#include <cwctype>    // std::towlower
#include <filesystem> // std::filesystem::path — 定位 exe 所在目录存放状态文件
#include <fstream>    // std::ofstream / std::ifstream — JSON 持久化
#include <memory>     // std::unique_ptr
#include <sstream>    // std::ostringstream — 拼 JSON
#include <string>     // std::wstring / std::string
#include <thread>     // std::thread
#include <utility>    // std::pair
#include <vector>     // std::vector<DownloadTask>

#include "downloader.h" // RunDownload / RequestDownloadStop / DownloadConfig / ProgressInfo
#include "format.h" // FormatBytes / FormatSpeed / FormatEta / WidenUtf8
#include "http.h"   // HttpOptions
#include "url.h"    // ParseUrl / DefaultFileNameFromUrl / Url

namespace { // 匿名命名空间

// ------------------------------------------------------------------ 常量与消息

/**
 * 资源 ID：与 app.rc 中的定义保持一致。
 */
constexpr int IDI_APPICON = 101; // 应用图标资源 ID

/**
 * 自定义窗口消息。wParam 携带 taskID（高 16 位）和状态码（低 16 位，仅
 * WM_APP_DONE 使用）。 WM_APP_PROGRESS：wParam = taskID，lParam = 堆上 new 的
 * ProgressInfo*（UI 线程负责释放）。 WM_APP_MESSAGE ：wParam = taskID，lParam =
 * 堆上 new 的 pair<wstring,bool>*（UI 线程负责释放）。 WM_APP_DONE    ：wParam
 * = (taskID << 16) | (int)DownloadStatus，无堆内存。
 */
const UINT WM_APP_PROGRESS = WM_APP + 1;
const UINT WM_APP_MESSAGE = WM_APP + 2;
const UINT WM_APP_DONE = WM_APP + 3;

/**
 * 从 WM_APP_DONE 的 wParam 解析出 taskID 和 DownloadStatus。
 */
inline int ExtractTaskIdFromDONE(WPARAM wp) {
  return static_cast<int>(wp >> 16);
}
inline DownloadStatus ExtractStatusFromDONE(WPARAM wp) {
  return static_cast<DownloadStatus>(wp & 0xFFFF);
}
inline WPARAM EncodeDoneWParam(int taskId, DownloadStatus st) {
  return static_cast<WPARAM>((taskId << 16) | static_cast<int>(st));
}

// ------------------------------------------------------------------ 控件 ID

enum ControlId {
  IDC_URL = 101,
  IDC_OUTPUT,
  IDC_BROWSE,
  IDC_THREADS,
  IDC_THREADS_SPIN,
  IDC_RETRY,
  IDC_RETRY_SPIN,
  IDC_PROXY,
  IDC_HEADERS,
  IDC_RESUME,
  IDC_ADD,        // "添加下载" 按钮（原 "开始"）
  IDC_STOP,       // 停止选中的任务
  IDC_RETRY_TASK, // 重新启动选中的已停止/失败任务
  IDC_REMOVE,     // 移除已完成/失败的任务
  IDC_LIST,       // ListView 任务列表
  IDC_LOG,        // 选中任务的日志显示
};

// ------------------------------------------------------------------
// 任务数据结构

/**
 * 一条下载任务的完整运行时状态。每个任务有自己独立的原子停止标志、
 * 工作线程和 ListView 行号；线程结束后 status 被置为
 * Completed/Interrupted/Failed。
 */
struct DownloadTask {
  int id = 0;            // 唯一任务 ID（从 1 递增）
  DownloadConfig config; // 拷贝一份下载配置
  std::atomic<bool> stopFlag{
      false}; // per-task 停止标志（供 downloader 层轮询）
  std::atomic<bool> interruptedFlag{false}; // per-task 中断标记
  std::thread worker; // 工作线程对象（joinable 时才 join）
  DownloadStatus status = DownloadStatus::Completed; // 最终状态
  ProgressInfo lastProgress; // 最近一次进度快照（UI 用）
  std::wstring fileName;     // 显示用文件名
  std::wstring log;     // 本任务的日志缓冲（工作线程通过 PostMessage 追加）
  int listIndex = -1;   // ListView 中的行号（-1 = 尚未添加）
  bool running = false; // 工作线程是否正在运行
};

/**
 * 任务池：所有活跃与已结束任务的容器。
 * 访问规则：只在 UI 线程读写；工作线程绝不直接修改。
 * 用 unique_ptr 存每个任务，因为 DownloadTask 含 std::atomic 和 std::thread，
 * 两者都不可拷贝（仅可移动但 vector 内部重排也会出问题）。
 */
std::vector<std::unique_ptr<DownloadTask>> g_tasks;
int g_nextTaskId = 1; // 下一个可用的任务 ID（单调递增，永不重置）

// ------------------------------------------------------------------
// 全局控件句柄

// ----- 视觉主题（仅外观，不影响任何功能） -----
// 调色板：浅灰窗口底 + 深灰正文，接近 Windows 11 现代应用的观感
constexpr COLORREF kColorWindowBg = RGB(243, 244, 246); // 窗口背景（浅灰）
constexpr COLORREF kColorControlText = RGB(31, 41, 55); // 输入框正文（近黑）
constexpr COLORREF kColorLabelText = RGB(75, 85, 99);   // 标签文字（中灰）
constexpr COLORREF kColorStatusOk = RGB(22, 128, 80);   // 状态列：已完成（绿）
constexpr COLORREF kColorStatusFail = RGB(200, 40, 40); // 状态列：失败（红）
constexpr COLORREF kColorStatusRun = RGB(20, 90, 180);  // 状态列：下载中（蓝）

HFONT g_hFont = nullptr;     // 全局 UI 字体：Segoe UI 9pt
HFONT g_hFontBold = nullptr; // 分组框标题：Segoe UI Bold 9pt
HFONT g_hFontMono = nullptr; // 日志区字体：Consolas 9pt（等宽对齐）
HBRUSH g_hbrBg = nullptr;    // 窗口背景画刷

HWND g_hwnd = nullptr;
HWND g_hUrl = nullptr;
HWND g_hOutput = nullptr;
HWND g_hBrowse = nullptr;
HWND g_hThreads = nullptr;
HWND g_hThreadsSpin = nullptr;
HWND g_hRetry = nullptr;
HWND g_hRetrySpin = nullptr;
HWND g_hProxy = nullptr;
HWND g_hHeaders = nullptr;
HWND g_hResume = nullptr;
HWND g_hAdd = nullptr;        // "添加下载" 按钮
HWND g_hStop = nullptr;       // "停止选中" 按钮
HWND g_hResumeTask = nullptr; // "继续" 按钮（重启已停止/失败的任务）
HWND g_hRemove = nullptr;     // "移除" 按钮
HWND g_hList = nullptr;       // ListView 任务列表
HWND g_hLog = nullptr;        // 日志编辑框
HWND g_hGroup = nullptr;      // "下载设置" 分组框

// ------------------------------------------------------------------ 工具函数

std::wstring GetText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring text(static_cast<size_t>(length), L'\0');
  GetWindowTextW(control, text.data(), length + 1);
  return text;
}

std::wstring Trim(const std::wstring &s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && (s[begin] == L' ' || s[begin] == L'\t' ||
                         s[begin] == L'\r' || s[begin] == L'\n')) {
    ++begin;
  }
  while (end > begin && (s[end - 1] == L' ' || s[end - 1] == L'\t' ||
                         s[end - 1] == L'\r' || s[end - 1] == L'\n')) {
    --end;
  }
  return s.substr(begin, end - begin);
}

std::wstring ToLower(std::wstring s) {
  for (wchar_t &ch : s)
    ch = static_cast<wchar_t>(std::towlower(ch));
  return s;
}

int EditInt(HWND edit, int fallback) {
  const std::wstring text = Trim(GetText(edit));
  if (text.empty())
    return fallback;
  wchar_t *end = nullptr;
  const long value = std::wcstol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != L'\0' || value < 0)
    return fallback;
  return static_cast<int>(value);
}

int AutoThreadCount() {
  unsigned hardware = std::thread::hardware_concurrency();
  if (hardware == 0)
    hardware = 4;
  return std::max(4, std::min(32, static_cast<int>(hardware) * 2));
}

/**
 * 把任务状态转换为 ListView 显示用的中文文本。
 */
std::wstring StatusToText(DownloadStatus st) {
  switch (st) {
  case DownloadStatus::Completed:
    return L"已完成";
  case DownloadStatus::Interrupted:
    return L"已停止";
  case DownloadStatus::Failed:
    return L"失败";
  }
  return L"运行中"; // 默认值（DownloadStatus 之外的情况）
}

/**
 * 找到 taskID 对应的任务在 g_tasks 中的下标。
 * 未找到时返回 -1。
 */
int FindTaskIndex(int taskId) {
  for (size_t i = 0; i < g_tasks.size(); ++i) {
    if (g_tasks[i]->id == taskId)
      return static_cast<int>(i);
  }
  return -1;
}

/**
 * 构建一个 DownloadConfig（从界面控件读取）。
 * 失败时 err 携带中文错误信息。
 */
bool BuildConfigFromUi(DownloadConfig &cfg, std::wstring &err) {
  const std::wstring url = Trim(GetText(g_hUrl));
  if (!ParseUrl(url, cfg.url, err))
    return false;

  cfg.outputPath = Trim(GetText(g_hOutput));
  if (cfg.outputPath.empty())
    cfg.outputPath = DefaultFileNameFromUrl(url);

  cfg.threads = std::max(1, std::min(64, EditInt(g_hThreads, 16)));
  cfg.retries = std::max(0, std::min(100, EditInt(g_hRetry, 5)));
  cfg.resume = SendMessageW(g_hResume, BM_GETCHECK, 0, 0) == BST_CHECKED;

  std::wstring proxy = Trim(GetText(g_hProxy));
  if (proxy.compare(0, 7, L"http://") == 0)
    proxy = proxy.substr(7);
  if (proxy.compare(0, 8, L"https://") == 0)
    proxy = proxy.substr(8);
  cfg.http.proxy = proxy;

  const std::wstring rawHeaders = GetText(g_hHeaders);
  size_t pos = 0;
  while (pos <= rawHeaders.size()) {
    size_t eol = rawHeaders.find(L'\n', pos);
    std::wstring line = Trim(rawHeaders.substr(
        pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos));
    pos = (eol == std::wstring::npos) ? rawHeaders.size() + 1 : eol + 1;
    if (line.empty())
      continue;

    const size_t colon = line.find(L':');
    if (colon == std::wstring::npos || colon == 0) {
      err = L"请求头格式应为 \"Key: Value\": " + line;
      return false;
    }
    std::wstring key = Trim(line.substr(0, colon));
    std::wstring value = Trim(line.substr(colon + 1));
    if (ToLower(key) == L"user-agent") {
      cfg.http.userAgent = value;
    } else {
      cfg.http.headers.push_back(key + L": " + value);
    }
  }
  return true;
}

/**
 * URL 变化时自动推导默认文件名。
 */
void MaybeFillOutputName() {
  if (!GetText(g_hOutput).empty())
    return;
  const std::wstring url = Trim(GetText(g_hUrl));
  Url parsed;
  std::wstring err;
  if (ParseUrl(url, parsed, err)) {
    SetWindowTextW(g_hOutput, DefaultFileNameFromUrl(url).c_str());
  }
}

// ------------------------------------------------------------------ ListView
// 辅助

/**
 * 向 ListView 添加一列。
 */
void ListViewAddColumn(int index, const wchar_t *title, int width) {
  LVCOLUMNW col{};
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  col.iSubItem = index;
  col.cx = width;
  col.pszText = const_cast<LPWSTR>(title); // LVM_INSERTCOLUMNW 会临时读取
  ListView_InsertColumn(g_hList, index, &col);
}

/**
 * 向 ListView 插入一行，返回新行的下标。
 */
int ListViewAddRow(int taskId, const std::wstring &fileName) {
  LVITEMW item{};
  item.mask = LVIF_TEXT | LVIF_PARAM;
  item.iItem = ListView_GetItemCount(g_hList);
  item.iSubItem = 0;
  item.pszText = const_cast<LPWSTR>(fileName.c_str());
  item.lParam = taskId; // lParam 存 taskID，便于后续根据行反查任务
  ListView_InsertItem(g_hList, &item);
  // 其余子项先填空字符串
  for (int col = 1; col < 5; ++col) {
    ListView_SetItemText(g_hList, item.iItem, col, const_cast<LPWSTR>(L""));
  }
  return item.iItem;
}

/**
 * 更新 ListView 指定行的全部内容。
 * col 顺序：0 文件名, 1 进度, 2 速度, 3 已下载/总大小, 4 状态
 */
void ListViewUpdateRow(int row, const DownloadTask &task) {
  // 进度文本
  wchar_t progress[64];
  if (task.lastProgress.total > 0) {
    double pct = 100.0 * static_cast<double>(task.lastProgress.done) /
                 static_cast<double>(task.lastProgress.total);
    if (pct > 100.0)
      pct = 100.0;
    swprintf_s(progress, L"%.1f%%", pct);
  } else if (task.lastProgress.done > 0) {
    swprintf_s(progress, L"下载中");
  } else if (task.running) {
    swprintf_s(progress, L"连接中...");
  } else {
    swprintf_s(progress, L"");
  }
  ListView_SetItemText(g_hList, row, 1, progress);

  // 速度
  std::string speedUtf8 = FormatSpeed(task.lastProgress.speedBps);
  std::wstring speed = WidenUtf8(speedUtf8);
  ListView_SetItemText(g_hList, row, 2, const_cast<LPWSTR>(speed.c_str()));

  // 已下载/总大小
  std::string sizeUtf8;
  if (task.lastProgress.total > 0) {
    sizeUtf8 = FormatBytes(task.lastProgress.done) + " / " +
               FormatBytes(task.lastProgress.total);
  } else if (task.lastProgress.done > 0) {
    sizeUtf8 = FormatBytes(task.lastProgress.done) + " / 未知";
  } else {
    sizeUtf8 = "";
  }
  std::wstring sizeText = WidenUtf8(sizeUtf8);
  ListView_SetItemText(g_hList, row, 3, const_cast<LPWSTR>(sizeText.c_str()));

  // 状态（运行中的任务显示 "下载中 N/M"）
  if (task.running) {
    wchar_t runStatus[64];
    swprintf_s(runStatus, L"下载中 %d/%d", task.lastProgress.active,
               task.lastProgress.workers);
    ListView_SetItemText(g_hList, row, 4, runStatus);
  } else {
    // 用局部变量保存，避免 ListView_SetItemText 读取临时对象销毁后的悬空指针
    std::wstring statusText = StatusToText(task.status);
    ListView_SetItemText(g_hList, row, 4, statusText.data());
  }
}

/**
 * 根据 ListView 行号取 taskID；未选中返回 -1。
 */
int GetSelectedTaskId() {
  const int row = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
  if (row < 0)
    return -1;
  LVITEMW item{};
  item.mask = LVIF_PARAM;
  item.iItem = row;
  item.iSubItem = 0;
  if (!ListView_GetItem(g_hList, &item))
    return -1;
  return static_cast<int>(item.lParam);
}

/**
 * 把任务的日志（工作线程通过 PostMessage 发来的）追加到日志框。
 */
void AppendTaskLog(int taskId, const std::wstring &text, bool /*isError*/) {
  const int idx = FindTaskIndex(taskId);
  if (idx < 0)
    return;
  g_tasks[idx]->log += text + L"\r\n";

  const int selectedId = GetSelectedTaskId();
  if (selectedId == taskId) {
    if (g_tasks[idx]->log.size() > 60000) {
      g_tasks[idx]->log = text + L"\r\n";
    }
    SetWindowTextW(g_hLog, g_tasks[idx]->log.c_str());
    SendMessageW(g_hLog, EM_SETSEL, GetWindowTextLengthW(g_hLog),
                 GetWindowTextLengthW(g_hLog));
    SendMessageW(g_hLog, EM_REPLACESEL, TRUE, 0);
  }
}

/**
 * 根据 ListView 选中行刷新下方的日志框内容。
 */
void RefreshLogFromSelection() {
  const int id = GetSelectedTaskId();
  if (id < 0) {
    SetWindowTextW(g_hLog, L"");
    return;
  }
  const int idx = FindTaskIndex(id);
  if (idx < 0)
    return;
  SetWindowTextW(g_hLog, g_tasks[idx]->log.c_str());
  SendMessageW(g_hLog, EM_SETSEL, GetWindowTextLengthW(g_hLog),
               GetWindowTextLengthW(g_hLog));
  SendMessageW(g_hLog, EM_REPLACESEL, TRUE, 0);
}

/**
 * 更新"停止"和"移除"按钮的启用状态。
 */
void UpdateButtons() {
  const int id = GetSelectedTaskId();
  if (id < 0) {
    EnableWindow(g_hStop, FALSE);
    EnableWindow(g_hResumeTask, FALSE);
    EnableWindow(g_hRemove, FALSE);
    return;
  }
  const int idx = FindTaskIndex(id);
  if (idx < 0) {
    EnableWindow(g_hStop, FALSE);
    EnableWindow(g_hResumeTask, FALSE);
    EnableWindow(g_hRemove, FALSE);
    return;
  }
  const bool running = g_tasks[idx]->running;
  EnableWindow(g_hStop, running ? TRUE : FALSE);
  EnableWindow(g_hResumeTask, running ? FALSE : TRUE);
  EnableWindow(g_hRemove, running ? FALSE : TRUE);
}

// ------------------------------------------------------------------ 任务持久化

/**
 * 状态文件路径 — 放在 exe 所在目录：cppdl_gui_tasks.json。
 * 关闭窗口时把 g_tasks 序列化到该文件，下次启动时自动加载。
 */
std::wstring StateFilePath() {
  wchar_t modulePath[MAX_PATH]{};
  GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
  std::filesystem::path p(modulePath);
  p.replace_filename(L"cppdl_gui_tasks.json");
  return p.wstring();
}

std::string JsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x",
                      static_cast<unsigned char>(ch));
        out += buf;
      } else {
        out += ch;
      }
    }
  }
  return out;
}

void SaveTasks() {
  std::ostringstream ss;
  ss << "[\n";
  bool first = true;
  for (const auto &task : g_tasks) {
    if (!first)
      ss << ",\n";
    first = false;
    const std::string urlUtf8 = NarrowUtf8(task->config.url.raw);
    const std::string outputUtf8 = NarrowUtf8(task->config.outputPath);
    const std::string proxyUtf8 = NarrowUtf8(task->config.http.proxy);
    const std::string uaUtf8 = NarrowUtf8(task->config.http.userAgent);
    ss << "  {\n";
    ss << "    \"url\": \"" << JsonEscape(urlUtf8) << "\",\n";
    ss << "    \"output\": \"" << JsonEscape(outputUtf8) << "\",\n";
    ss << "    \"threads\": " << task->config.threads << ",\n";
    ss << "    \"retries\": " << task->config.retries << ",\n";
    ss << "    \"resume\": " << (task->config.resume ? "true" : "false")
       << ",\n";
    ss << "    \"proxy\": \"" << JsonEscape(proxyUtf8) << "\",\n";
    ss << "    \"userAgent\": \"" << JsonEscape(uaUtf8) << "\",\n";
    ss << "    \"headers\": [";
    bool hFirst = true;
    for (const auto &h : task->config.http.headers) {
      if (!hFirst)
        ss << ", ";
      hFirst = false;
      ss << "\"" << JsonEscape(NarrowUtf8(h)) << "\"";
    }
    ss << "],\n";
    const char *statusStr = "interrupted";
    switch (task->status) {
    case DownloadStatus::Completed:
      statusStr = "completed";
      break;
    case DownloadStatus::Interrupted:
      statusStr = "interrupted";
      break;
    case DownloadStatus::Failed:
      statusStr = "failed";
      break;
    }
    ss << "    \"status\": \"" << statusStr << "\"\n";
    ss << "  }";
  }
  ss << "\n]\n";

  const std::wstring path = StateFilePath();
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open())
    return;
  const std::string json = ss.str();
  out.write("\xEF\xBB\xBF", 3); // UTF-8 BOM
  out.write(json.data(), json.size());
}

// --- 极简 JSON 解析器：只处理我们自己写的格式 ---
bool GetJsonString(const std::string &json, size_t hint, const std::string &key,
                   std::string &out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle, hint);
  if (pos == std::string::npos)
    return false;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (pos >= json.size() || json[pos] != '"')
    return false;
  ++pos;
  std::string val;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      char next = json[pos + 1];
      if (next == 'n')
        val += '\n';
      else if (next == 'r')
        val += '\r';
      else if (next == 't')
        val += '\t';
      else
        val += next;
      pos += 2;
    } else {
      val += json[pos];
      ++pos;
    }
  }
  out = val;
  return true;
}

bool GetJsonInt(const std::string &json, size_t hint, const std::string &key,
                int &out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle, hint);
  if (pos == std::string::npos)
    return false;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (pos >= json.size())
    return false;
  const char *start = json.data() + pos;
  char *end = nullptr;
  long val = std::strtol(start, &end, 10);
  if (end == start)
    return false;
  out = static_cast<int>(val);
  return true;
}

bool GetJsonBool(const std::string &json, size_t hint, const std::string &key,
                 bool &out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle, hint);
  if (pos == std::string::npos)
    return false;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (json.compare(pos, 4, "true") == 0) {
    out = true;
    return true;
  }
  if (json.compare(pos, 5, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

bool GetJsonStringArray(const std::string &json, size_t hint,
                        const std::string &key,
                        std::vector<std::wstring> &out) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle, hint);
  if (pos == std::string::npos)
    return false;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (pos >= json.size() || json[pos] != '[')
    return false;
  ++pos;
  while (pos < json.size() && json[pos] != ']') {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',' ||
                                 json[pos] == '\n' || json[pos] == '\t'))
      ++pos;
    if (pos >= json.size() || json[pos] == ']')
      break;
    if (json[pos] != '"') {
      ++pos;
      continue;
    }
    ++pos;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        val += json[pos + 1];
        pos += 2;
      } else {
        val += json[pos];
        ++pos;
      }
    }
    out.push_back(WidenUtf8(val));
    if (pos < json.size())
      ++pos;
  }
  return true;
}

void LoadTasks() {
  const std::wstring path = StateFilePath();
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return;
  std::string json((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());

  // 跳过 UTF-8 BOM
  if (json.size() >= 3 && json[0] == '\xEF' && json[1] == '\xBB' &&
      json[2] == '\xBF') {
    json = json.substr(3);
  }

  size_t pos = 0;
  while ((pos = json.find('{', pos)) != std::string::npos) {
    size_t end = json.find('}', pos);
    if (end == std::string::npos)
      break;
    std::string obj = json.substr(pos, end - pos + 1);

    std::string urlUtf8, outputUtf8, proxyUtf8, uaUtf8, statusStr;
    int threads = 16, retries = 5;
    bool resume = true;
    std::vector<std::wstring> headers;

    if (!GetJsonString(obj, 0, "url", urlUtf8)) {
      pos = end + 1;
      continue;
    }
    GetJsonString(obj, 0, "output", outputUtf8);
    GetJsonInt(obj, 0, "threads", threads);
    GetJsonInt(obj, 0, "retries", retries);
    GetJsonBool(obj, 0, "resume", resume);
    GetJsonString(obj, 0, "proxy", proxyUtf8);
    GetJsonString(obj, 0, "userAgent", uaUtf8);
    GetJsonStringArray(obj, 0, "headers", headers);
    GetJsonString(obj, 0, "status", statusStr);

    auto task = std::make_unique<DownloadTask>();
    task->id = g_nextTaskId++; // 分配唯一 ID（恢复任务也走同一序列）
    // 必须调用 ParseUrl 填充 host/path/secure/port —— 只设 url.raw 会导致后续
    // HttpStream::Open 里 WinHttpConnect 因 url.host 为空而失败！
    std::wstring urlWide = WidenUtf8(urlUtf8);
    std::wstring urlErr;
    if (!ParseUrl(urlWide, task->config.url, urlErr)) {
      pos = end + 1;
      continue; // URL 损坏，跳过这条
    }
    task->config.outputPath = WidenUtf8(outputUtf8);
    task->config.threads = threads;
    task->config.retries = retries;
    task->config.resume = resume;
    task->config.http.proxy = WidenUtf8(proxyUtf8);
    task->config.http.userAgent = WidenUtf8(uaUtf8);
    task->config.http.headers = std::move(headers);
    // 和 AddDownloadTask 保持一致：显示 outputPath 的
    // basename，尊重用户上次设置的保存名
    task->fileName =
        std::filesystem::path(task->config.outputPath).filename().wstring();
    if (task->fileName.empty())
      task->fileName = DefaultFileNameFromUrl(task->config.url.raw);
    task->running = false;

    if (statusStr == "completed")
      task->status = DownloadStatus::Completed;
    else if (statusStr == "failed")
      task->status = DownloadStatus::Failed;
    else
      task->status = DownloadStatus::Interrupted;

    task->listIndex = ListViewAddRow(task->id, task->fileName);
    ListViewUpdateRow(task->listIndex, *task);
    g_tasks.push_back(std::move(task));

    pos = end + 1;
  }
}

// ------------------------------------------------------------------ 任务操作

/**
 * 添加一个新下载任务（UI 线程）。
 * 从界面读取配置、分配任务 ID、构造线程上下文、启动工作线程。
 * 工作线程内部 RunDownload 会通过 DownloadHooks 把进度/消息 PostMessage 回来。
 */
void AddDownloadTask() {
  DownloadConfig cfg;
  std::wstring err;
  if (!BuildConfigFromUi(cfg, err)) {
    MessageBoxW(g_hwnd, err.c_str(), L"输入错误", MB_OK | MB_ICONERROR);
    return;
  }

  auto task = std::make_unique<DownloadTask>();
  task->id = g_nextTaskId++;
  task->config = std::move(cfg);
  // 显示名取 outputPath 的最后一段 —— 用户可能显式改了保存文件名（如
  // 123.gguf）， 不能总是用 URL 推导的名字。std::filesystem::path 同时处理 / 和
  // \。
  task->fileName =
      std::filesystem::path(task->config.outputPath).filename().wstring();
  if (task->fileName.empty())
    task->fileName = DefaultFileNameFromUrl(task->config.url.raw);

  task->listIndex = ListViewAddRow(task->id, task->fileName);
  ListViewUpdateRow(task->listIndex, *task);

  task->running = true;

  DownloadHooks hooks;
  hooks.stopFlag = &task->stopFlag;
  hooks.interruptedFlag = &task->interruptedFlag;
  const int tid = task->id;
  hooks.onProgress = [hwnd = g_hwnd, tid](const ProgressInfo &info) {
    auto *copy = new ProgressInfo(info);
    if (!PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(tid),
                      reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  };
  hooks.onMessage = [hwnd = g_hwnd, tid](const std::wstring &text,
                                         bool isError) {
    auto *copy = new std::pair<std::wstring, bool>(text, isError);
    if (!PostMessageW(hwnd, WM_APP_MESSAGE, static_cast<WPARAM>(tid),
                      reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  };

  task->worker =
      std::thread([cfg = task->config, hooks, hwnd = g_hwnd, tid]() mutable {
        DownloadStatus st = RunDownload(cfg, hooks);
        PostMessageW(hwnd, WM_APP_DONE, EncodeDoneWParam(tid, st), 0);
      });

  g_tasks.push_back(std::move(task));
  UpdateButtons();
}

void StopSelectedTask() {
  const int id = GetSelectedTaskId();
  if (id < 0)
    return;
  const int idx = FindTaskIndex(id);
  if (idx < 0 || !g_tasks[idx]->running)
    return;

  g_tasks[idx]->interruptedFlag.store(true, std::memory_order_relaxed);
  g_tasks[idx]->stopFlag.store(true, std::memory_order_relaxed);
}

void RemoveSelectedTask() {
  const int id = GetSelectedTaskId();
  if (id < 0)
    return;
  const int idx = FindTaskIndex(id);
  if (idx < 0 || g_tasks[idx]->running)
    return;

  const int row = g_tasks[idx]->listIndex;
  ListView_DeleteItem(g_hList, row);
  g_tasks.erase(g_tasks.begin() + idx);

  for (size_t i = 0; i < g_tasks.size(); ++i) {
    if (g_tasks[i]->listIndex > row) {
      g_tasks[i]->listIndex -= 1;
    }
  }

  SetWindowTextW(g_hLog, L"");
  UpdateButtons();
}

/**
 * 重新启动选中的已停止/失败任务（UI 线程）。
 * 为旧的 stopFlag/interruptedFlag 清零，重新创建 DownloadHooks 和工作线程。
 * 复用原有 DownloadConfig — 断点续传由 downloader 层自动处理。
 */
void ResumeSelectedTask() {
  const int id = GetSelectedTaskId();
  if (id < 0)
    return;
  const int idx = FindTaskIndex(id);
  if (idx < 0 || g_tasks[idx]->running)
    return;

  auto &task = *g_tasks[idx];

  // 清零 per-task 标志（旧的 stopFlag/interruptedFlag 还活着，直接 reset 就行）
  task.stopFlag.store(false, std::memory_order_relaxed);
  task.interruptedFlag.store(false, std::memory_order_relaxed);
  task.running = true;

  DownloadHooks hooks;
  hooks.stopFlag = &task.stopFlag;
  hooks.interruptedFlag = &task.interruptedFlag;
  const int tid = task.id;
  hooks.onProgress = [hwnd = g_hwnd, tid](const ProgressInfo &info) {
    auto *copy = new ProgressInfo(info);
    if (!PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(tid),
                      reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  };
  hooks.onMessage = [hwnd = g_hwnd, tid](const std::wstring &text,
                                         bool isError) {
    auto *copy = new std::pair<std::wstring, bool>(text, isError);
    if (!PostMessageW(hwnd, WM_APP_MESSAGE, static_cast<WPARAM>(tid),
                      reinterpret_cast<LPARAM>(copy))) {
      delete copy;
    }
  };

  // 清空旧日志的末尾（保留失败原因那段，让用户知道为什么重启）
  if (!task.log.empty()) {
    task.log += L"\r\n--- 重新启动 ---\r\n";
  }

  task.worker =
      std::thread([cfg = task.config, hooks, hwnd = g_hwnd, tid]() mutable {
        DownloadStatus st = RunDownload(cfg, hooks);
        PostMessageW(hwnd, WM_APP_DONE, EncodeDoneWParam(tid, st), 0);
      });

  AppendTaskLog(task.id, L"已重新启动（断点续传）", false);
  ListViewUpdateRow(task.listIndex, task);
  UpdateButtons();
}

// ------------------------------------------------------------------ 消息处理

void OnProgress(int taskId, ProgressInfo *info) {
  std::unique_ptr<ProgressInfo> guard(info);
  const int idx = FindTaskIndex(taskId);
  if (idx < 0)
    return;
  g_tasks[idx]->lastProgress = *info;
  if (g_tasks[idx]->listIndex >= 0) {
    ListViewUpdateRow(g_tasks[idx]->listIndex, *g_tasks[idx]);
  }
}

void OnMessage(int taskId, std::pair<std::wstring, bool> *message) {
  std::unique_ptr<std::pair<std::wstring, bool>> guard(message);
  AppendTaskLog(taskId, message->first, message->second);
}

void OnDone(int taskId, DownloadStatus st) {
  const int idx = FindTaskIndex(taskId);
  if (idx < 0)
    return;

  if (g_tasks[idx]->worker.joinable()) {
    g_tasks[idx]->worker.join();
  }

  g_tasks[idx]->running = false;
  g_tasks[idx]->status = st;

  if (g_tasks[idx]->listIndex >= 0) {
    ListViewUpdateRow(g_tasks[idx]->listIndex, *g_tasks[idx]);
  }

  const wchar_t *summary = L"";
  switch (st) {
  case DownloadStatus::Completed:
    summary = L"[完成] 下载已成功完成";
    break;
  case DownloadStatus::Interrupted:
    summary = L"[停止] 已中断（下次运行相同命令可从断点继续）";
    break;
  case DownloadStatus::Failed:
    summary = L"[失败] 下载失败，详见上方日志";
    break;
  }
  AppendTaskLog(taskId, summary, st == DownloadStatus::Failed);

  UpdateButtons();
}

// ------------------------------------------------------------------ 界面创建

/**
 * 创建全局字体与背景画刷。
 * 以 96 DPI 为基准用负高度指定字号（负值 = 字符高度而非行高），
 * 高 DPI 下由系统按清单中的 Per-Monitor 声明自动缩放窗口。
 */
void CreateThemeObjects() {
  g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                        L"Segoe UI");
  if (g_hFont == nullptr) // 兜底：极端环境下退回系统 GUI 字体
    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          L"Microsoft YaHei UI");

  g_hFontBold = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
  g_hFontMono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FIXED_PITCH, L"Consolas");
  g_hbrBg = CreateSolidBrush(kColorWindowBg);
}

HWND MakeControl(const wchar_t *className, DWORD style, int x, int y, int w,
                 int h, HMENU id, const wchar_t *text = nullptr) {
  HWND control = CreateWindowExW(
      0, className, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, g_hwnd, id,
      reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(g_hwnd, GWLP_HINSTANCE)),
      nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
  return control;
}

void MakeLabel(int x, int y, int w, const wchar_t *text) {
  MakeControl(L"STATIC", SS_LEFT, x, y, w, 18, nullptr, text);
}

// ---- 布局常量：所有间距集中定义，保证整窗节奏一致 ----
constexpr int kMargin = 16;   // 窗口四周边距
constexpr int kLabelX = 34;   // 分组框内标签起始 x
constexpr int kFieldX = 106;  // 分组框内输入控件起始 x
constexpr int kCtrlH = 26;    // 输入框/按钮统一高度
constexpr int kBtnH = 32;     // 主操作按钮高度（更大更易点）

/**
 * 分组框标题（用 STATIC 模拟，避免 BS_GROUPBOX 的主题灰字）。
 */
void MakeGroupTitle(int x, int y, const wchar_t *text) {
  HWND title = MakeControl(L"STATIC", SS_LEFT, x, y, 200, 20, nullptr, text);
  SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
}

void CreateControls() {
  // ---- 下载设置分组框 ----
  HWND group = g_hGroup = MakeControl(
      L"BUTTON", BS_GROUPBOX | WS_TABSTOP, kMargin, 12, 0, 220,
      nullptr, nullptr); // 宽度在 LayoutControls 里跟随窗口
  SendMessageW(group, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
  MakeGroupTitle(kLabelX + 4, 6, L" 下载设置 ");

  constexpr int rowY1 = 34, rowY2 = 66, rowY3 = 98;
  constexpr int headersY = 130, headersH = 58;
  constexpr int checkY = 198;

  MakeLabel(kLabelX, rowY1 + 5, 64, L"URL:");
  g_hUrl = MakeControl(
      L"EDIT", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kFieldX,
      rowY1, 0, kCtrlH, // 宽度在 LayoutControls 里计算
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_URL)));

  MakeLabel(kLabelX, rowY2 + 5, 64, L"保存到:");
  g_hOutput = MakeControl(
      L"EDIT", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kFieldX, rowY2, 0,
      kCtrlH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OUTPUT)));
  g_hBrowse = MakeControl(
      L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, 0, rowY2, 88, kCtrlH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE)), L"浏览...");

  MakeLabel(kLabelX, rowY3 + 5, 64, L"线程数:");
  g_hThreads =
      MakeControl(L"EDIT", WS_BORDER | ES_NUMBER, kFieldX, rowY3, 60, kCtrlH,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THREADS)));
  g_hThreadsSpin = MakeControl(
      UPDOWN_CLASSW,
      UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_HOTTRACK, 0, 0, 0,
      0, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THREADS_SPIN)));
  SendMessageW(g_hThreadsSpin, UDM_SETBUDDY,
               reinterpret_cast<WPARAM>(g_hThreads), 0);
  SendMessageW(g_hThreadsSpin, UDM_SETRANGE32, 1, 64);
  SendMessageW(g_hThreadsSpin, UDM_SETPOS32, 0, AutoThreadCount());

  constexpr int retryLabelX = kFieldX + 76;
  MakeLabel(retryLabelX, rowY3 + 5, 40, L"重试:");
  g_hRetry =
      MakeControl(L"EDIT", WS_BORDER | ES_NUMBER, retryLabelX + 44, rowY3, 60,
                  kCtrlH,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RETRY)));
  g_hRetrySpin = MakeControl(
      UPDOWN_CLASSW,
      UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_HOTTRACK, 0, 0, 0,
      0, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RETRY_SPIN)));
  SendMessageW(g_hRetrySpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(g_hRetry),
               0);
  SendMessageW(g_hRetrySpin, UDM_SETRANGE32, 0, 100);
  SendMessageW(g_hRetrySpin, UDM_SETPOS32, 0, 5);

  constexpr int proxyLabelX = kFieldX + 200;
  MakeLabel(proxyLabelX, rowY3 + 5, 40, L"代理:");
  g_hProxy = MakeControl(
      L"EDIT", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, proxyLabelX + 44,
      rowY3, 0, kCtrlH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROXY)));

  MakeLabel(kLabelX, headersY + 5, 64, L"请求头:");
  g_hHeaders =
      MakeControl(L"EDIT",
                  WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                      ES_AUTOVSCROLL | ES_WANTRETURN,
                  kFieldX, headersY, 0, headersH,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HEADERS)));

  g_hResume = MakeControl(
      L"BUTTON", WS_TABSTOP | BS_AUTOCHECKBOX, kFieldX, checkY, 110, 20,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESUME)), L"断点续传");
  SendMessageW(g_hResume, BM_SETCHECK, BST_CHECKED, 0);

  // ---- 操作按钮区（从右往左排：移除 → 继续 → 停止 → 添加） ----
  constexpr int btnRowY = 244;
  g_hRemove = MakeControl(
      L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, 0, btnRowY, 76, kBtnH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REMOVE)), L"移除");
  g_hResumeTask = MakeControl(
      L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, 0, btnRowY, 84, kBtnH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RETRY_TASK)), L"继续");
  g_hStop = MakeControl(
      L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, 0, btnRowY, 84, kBtnH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STOP)), L"停止");
  g_hAdd = MakeControl(
      L"BUTTON", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, btnRowY, 96, kBtnH,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ADD)), L"添加下载");

  // ---- 任务列表 ----
  g_hList = MakeControl(
      WC_LISTVIEWW,
      WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      kMargin, 290, 0, 170,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)));
  // 现代观感：整行选择 + 平滑滚动/双缓冲 + 主题表头（不画老式网格线）
  SendMessageW(g_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
               LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  ListViewAddColumn(0, L"文件名", 190);
  ListViewAddColumn(1, L"进度", 76);
  ListViewAddColumn(2, L"速度", 88);
  ListViewAddColumn(3, L"已下载 / 总大小", 150);
  ListViewAddColumn(4, L"状态", 100);

  // ---- 日志框（显示选中任务的详细日志，等宽字体便于对齐查看） ----
  g_hLog = MakeControl(
      L"EDIT",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0,
      0, 0, 0, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)));
  SendMessageW(g_hLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontMono),
               TRUE);

  UpdateButtons();
}

void BrowseOutput() {
  wchar_t fileBuffer[MAX_PATH] = L"";
  const std::wstring derived = DefaultFileNameFromUrl(Trim(GetText(g_hUrl)));
  wcsncpy_s(fileBuffer, derived.c_str(), _TRUNCATE);

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFile = fileBuffer;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"选择保存位置";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_NOREADONLYRETURN;
  if (GetSaveFileNameW(&ofn)) {
    SetWindowTextW(g_hOutput, fileBuffer);
  }
}

void LayoutControls(int width, int height) {
  if (g_hList == nullptr || width <= 0 || height <= 0)
    return;

  // 与 CreateControls 中的行坐标保持一致
  constexpr int kGroupTop = 12;
  constexpr int kRowY1 = 34, kRowY2 = 66, kRowY3 = 98;
  constexpr int kHeadersY = 130, kHeadersH = 58;
  constexpr int kGroupH = 220;
  constexpr int kBtnRowY = 244, kBtnH = 32;
  constexpr int kListY = 290, kListH = 170;
  constexpr int kCtrlH = 26;
  constexpr int kButtonGap = 8;

  const int right = width - kMargin;
  const int groupW = width - 2 * kMargin;
  const int fieldW = right - kMargin - kFieldX;

  // 分组框 + 内部控件
  MoveWindow(g_hGroup, kMargin, kGroupTop, groupW, kGroupH, TRUE);
  MoveWindow(g_hUrl, kFieldX, kRowY1, fieldW, kCtrlH, TRUE);
  MoveWindow(g_hOutput, kFieldX, kRowY2,
             right - 88 - kButtonGap - kFieldX, kCtrlH, TRUE);
  MoveWindow(g_hBrowse, right - 88, kRowY2, 88, kCtrlH, TRUE);
  MoveWindow(g_hProxy, kFieldX + 244, kRowY3,
             right - kFieldX - 244, kCtrlH, TRUE);
  MoveWindow(g_hHeaders, kFieldX, kHeadersY, fieldW, kHeadersH, TRUE);

  // 4 个操作按钮从右往左排：移除 → 继续 → 停止 → 添加
  constexpr int kWRemove = 76, kWResume = 84, kWStop = 84, kWAdd = 96;
  const int removeX = right - kWRemove;
  const int resumeX = removeX - kButtonGap - kWResume;
  const int stopX = resumeX - kButtonGap - kWStop;
  const int addX = stopX - kButtonGap - kWAdd;
  MoveWindow(g_hRemove, removeX, kBtnRowY, kWRemove, kBtnH, TRUE);
  MoveWindow(g_hResumeTask, resumeX, kBtnRowY, kWResume, kBtnH, TRUE);
  MoveWindow(g_hStop, stopX, kBtnRowY, kWStop, kBtnH, TRUE);
  MoveWindow(g_hAdd, addX, kBtnRowY, kWAdd, kBtnH, TRUE);

  MoveWindow(g_hList, kMargin, kListY, width - 2 * kMargin, kListH, TRUE);

  // 日志框吃掉 ListView 下方到窗口底部的剩余空间
  int logTop = kListY + kListH + 10;
  int logHeight = height - logTop - kMargin;
  if (logHeight < 60)
    logHeight = 60;
  MoveWindow(g_hLog, kMargin, logTop, width - 2 * kMargin, logHeight, TRUE);
}

// ------------------------------------------------------------------ 窗口过程

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam,
                         LPARAM lParam) {
  switch (message) {
  case WM_CREATE:
    return 0;

  case WM_SIZE:
    LayoutControls(LOWORD(lParam), HIWORD(lParam));
    return 0;

  case WM_GETMINMAXINFO: {
    auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
    RECT minRect{0, 0, 540, 620}; // 窗口最小客户区：留足 ListView + 日志框
    AdjustWindowRectEx(&minRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    info->ptMinTrackSize.x = minRect.right - minRect.left;
    info->ptMinTrackSize.y = minRect.bottom - minRect.top;
    return 0;
  }

  case WM_COMMAND: {
    const int id = LOWORD(wParam);
    const int code = HIWORD(wParam);
    if (id == IDC_ADD && code == BN_CLICKED) {
      AddDownloadTask();
    } else if (id == IDC_STOP && code == BN_CLICKED) {
      StopSelectedTask();
    } else if (id == IDC_RETRY_TASK && code == BN_CLICKED) {
      ResumeSelectedTask();
    } else if (id == IDC_REMOVE && code == BN_CLICKED) {
      RemoveSelectedTask();
    } else if (id == IDC_BROWSE && code == BN_CLICKED) {
      BrowseOutput();
    } else if (id == IDC_URL && code == EN_CHANGE) {
      MaybeFillOutputName();
    }
    return 0;
  }

  // 浅灰窗口背景
  case WM_ERASEBKGND: {
    HDC dc = reinterpret_cast<HDC>(wParam);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, g_hbrBg);
    return 1;
  }

  // 标签 / 分组框文字：中灰字 + 透明底（融入窗口背景）
  case WM_CTLCOLORSTATIC: {
    HDC dc = reinterpret_cast<HDC>(wParam);
    const HWND from = reinterpret_cast<HWND>(lParam);
    if (from == g_hLog) {
      // 只读日志框走 CTLCOLORSTATIC：白底深字，像普通输入框
      SetTextColor(dc, kColorControlText);
      SetBkColor(dc, RGB(255, 255, 255));
      static HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
      return reinterpret_cast<LRESULT>(white);
    }
    SetTextColor(dc, kColorLabelText);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(g_hbrBg);
  }

  // 输入框：白底 + 近黑正文
  case WM_CTLCOLOREDIT: {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, kColorControlText);
    SetBkColor(dc, RGB(255, 255, 255));
    static HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    return reinterpret_cast<LRESULT>(white);
  }

  case WM_NOTIFY: {
    // ListView 选择变化 — 必须走 WM_NOTIFY，不能在 WM_COMMAND 里处理
    auto *hdr = reinterpret_cast<NMHDR *>(lParam);
    if (hdr->idFrom == IDC_LIST && hdr->code == LVN_ITEMCHANGED) {
      RefreshLogFromSelection();
      UpdateButtons();
      return 0;
    }
    // 状态列文字着色：已完成绿 / 失败红 / 下载中蓝（纯视觉，不改数据）
    if (hdr->idFrom == IDC_LIST && hdr->code == NM_CUSTOMDRAW) {
      auto *cd = reinterpret_cast<NMLVCUSTOMDRAW *>(lParam);
      switch (cd->nmcd.dwDrawStage) {
      case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
      case CDDS_ITEMPREPAINT: {
        const int idx =
            FindTaskIndex(static_cast<int>(cd->nmcd.dwItemSpec));
        if (idx >= 0) {
          const DownloadTask &task = *g_tasks[idx];
          if (task.running) {
            cd->clrText = kColorStatusRun;
          } else if (task.status == DownloadStatus::Completed) {
            cd->clrText = kColorStatusOk;
          } else if (task.status == DownloadStatus::Failed) {
            cd->clrText = kColorStatusFail;
          } else {
            return CDRF_DODEFAULT;
          }
          return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
      }
      default:
        return CDRF_DODEFAULT;
      }
    }
    // 其他 WM_NOTIFY（如 EN_CHANGE 编辑框通知）交给默认处理
    return DefWindowProcW(hwnd, message, wParam, lParam);
  }

  case WM_APP_PROGRESS:
    OnProgress(static_cast<int>(wParam),
               reinterpret_cast<ProgressInfo *>(lParam));
    return 0;

  case WM_APP_MESSAGE:
    OnMessage(static_cast<int>(wParam),
              reinterpret_cast<std::pair<std::wstring, bool> *>(lParam));
    return 0;

  case WM_APP_DONE:
    OnDone(ExtractTaskIdFromDONE(wParam), ExtractStatusFromDONE(wParam));
    return 0;

  case WM_CLOSE: {
    // 先停止所有运行中的任务，并等待线程退出（让 .dlstate 完整落盘）
    for (auto &task : g_tasks) {
      if (task->running) {
        task->interruptedFlag.store(true, std::memory_order_relaxed);
        task->stopFlag.store(true, std::memory_order_relaxed);
      }
    }
    for (auto &task : g_tasks) {
      if (task->worker.joinable()) {
        task->worker.join();
      }
    }
    // 保存任务列表到 JSON —— 下次启动自动恢复
    SaveTasks();
    g_tasks.clear();
    DestroyWindow(hwnd);
    return 0;
  }

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;

  default:
    return DefWindowProcW(hwnd, message, wParam, lParam);
  }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES |
              ICC_LISTVIEW_CLASSES; // 新增 ListView 公共控件类
  InitCommonControlsEx(&icc);

  // 先建好字体 / 配色，注册窗口类时才能直接引用背景画刷
  CreateThemeObjects();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON)); // 应用图标
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = g_hbrBg; // 浅灰现代底色（原为 COLOR_BTNFACE 灰面板色）
  wc.lpszClassName = L"cppdl_gui_window";
  wc.hIconSm = wc.hIcon;
  if (!RegisterClassExW(&wc))
    return 1;

  RECT client{0, 0, 620, 640}; // 初始客户区（匹配新布局节奏）
  AdjustWindowRectEx(&client, WS_OVERLAPPEDWINDOW, FALSE, 0);

  g_hwnd =
      CreateWindowExW(0, wc.lpszClassName, L"cppdl 多线程下载器",
                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                      client.right - client.left, client.bottom - client.top,
                      nullptr, nullptr, instance, nullptr);
  if (g_hwnd == nullptr)
    return 1;

  CreateControls();
  RECT area{};
  GetClientRect(g_hwnd, &area);
  LayoutControls(area.right - area.left, area.bottom - area.top);
  // 恢复上次关闭前保存的任务（已停止/已完成状态，用户可点击"继续"续传）
  LoadTasks();
  UpdateButtons();
  ShowWindow(g_hwnd, showCmd);
  UpdateWindow(g_hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(g_hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  const int exitCode = static_cast<int>(msg.wParam);
  DeleteObject(g_hFont);
  DeleteObject(g_hFontBold);
  DeleteObject(g_hFontMono);
  DeleteObject(g_hbrBg);
  return exitCode;
}
