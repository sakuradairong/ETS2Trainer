// common.h —— 公共定义：字符串转换、日志、时间、格式化
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ets2 {

// ---------- 字符串 ----------
std::string  W2U(const std::wstring& w);          // UTF-16 -> UTF-8
std::wstring U2W(const std::string& s);           // UTF-8  -> UTF-16
std::string  fmt(const char* f, ...);             // printf 风格 -> std::string
std::wstring fmtW(const wchar_t* f, ...);         // printf 风格 -> std::wstring
std::string  trim(const std::string& s);
bool         startsWith(const std::string& s, const char* prefix);
std::string  hexAddr(uint64_t v);                 // 0x0000000000ABCDEF

// ---------- 时间 ----------
uint64_t nowMs();
std::string localTimeString(uint64_t unixSeconds);

// ---------- 格式化 ----------
std::string formatInt(int64_t v);
std::string formatSize(uint64_t bytes);

// ---------- 日志（界面底部 + 文件）----------
void        logLine(const std::string& text);
std::vector<std::string>  logSnapshot();   // 线程安全地复制一份日志
void        logClear();
void        logSaveToFile(const std::wstring& path);

}  // namespace ets2
