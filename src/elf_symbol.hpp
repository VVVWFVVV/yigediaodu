#pragma once
#include <string>

namespace xdd {

// 在 ELF64 共享库的动态符号表里查找符号，返回它在文件中的偏移
// （uprobe_events 需要的 file offset，不是 vaddr）。找不到/解析失败返回 -1。
//
// 用途：frame.symbol_off 是写死的，OTA/换设备后 adreno 库重排会失效。
// 运行时从 /vendor/lib64/libEGL_adreno.so 解析 eglSwapBuffers，成功则优先使用，
// 失败才回退配置值。
long long elf_resolve_symbol_offset(const std::string& path, const std::string& symbol);

}  // namespace xdd
