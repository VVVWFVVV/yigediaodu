#include "elf_symbol.hpp"

#include <cstring>
#include <elf.h>
#include <fstream>
#include <vector>

namespace xdd {
namespace {

bool range_ok(size_t file_size, unsigned long long off, unsigned long long len) {
    return off <= file_size && len <= file_size - off;
}

}  // namespace

long long elf_resolve_symbol_offset(const std::string& path, const std::string& symbol) {
    if (path.empty() || symbol.empty()) return -1;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return -1;
    f.seekg(0, std::ios::end);
    std::streamoff end = f.tellg();
    if (end <= 0 || end > (std::streamoff)(64 * 1024 * 1024)) return -1;
    f.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf((size_t)end);
    if (!f.read(reinterpret_cast<char*>(buf.data()), end)) return -1;

    if (buf.size() < sizeof(Elf64_Ehdr)) return -1;
    Elf64_Ehdr eh;
    std::memcpy(&eh, buf.data(), sizeof(eh));
    if (eh.e_ident[EI_MAG0] != ELFMAG0 || eh.e_ident[EI_MAG1] != ELFMAG1 ||
        eh.e_ident[EI_MAG2] != ELFMAG2 || eh.e_ident[EI_MAG3] != ELFMAG3) {
        return -1;
    }
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) return -1;
    if (eh.e_shentsize < sizeof(Elf64_Shdr) || eh.e_shnum == 0) return -1;
    if (!range_ok(buf.size(), eh.e_shoff, (unsigned long long)eh.e_shentsize * eh.e_shnum)) {
        return -1;
    }

    int dynsym_idx = -1;
    for (int i = 0; i < (int)eh.e_shnum; ++i) {
        Elf64_Shdr sh;
        size_t off = (size_t)eh.e_shoff + (size_t)i * eh.e_shentsize;
        std::memcpy(&sh, buf.data() + off, sizeof(sh));
        if (sh.sh_type == SHT_DYNSYM) {
            dynsym_idx = i;
            break;
        }
    }
    if (dynsym_idx < 0) return -1;

    Elf64_Shdr dynsym;
    std::memcpy(&dynsym,
                buf.data() + (size_t)eh.e_shoff + (size_t)dynsym_idx * eh.e_shentsize,
                sizeof(dynsym));
    if (dynsym.sh_link >= eh.e_shnum) return -1;
    Elf64_Shdr strtab;
    std::memcpy(&strtab,
                buf.data() + (size_t)eh.e_shoff + (size_t)dynsym.sh_link * eh.e_shentsize,
                sizeof(strtab));
    if (!range_ok(buf.size(), dynsym.sh_offset, dynsym.sh_size) ||
        !range_ok(buf.size(), strtab.sh_offset, strtab.sh_size)) {
        return -1;
    }

    size_t count = (size_t)(dynsym.sh_size / sizeof(Elf64_Sym));
    for (size_t i = 0; i < count; ++i) {
        Elf64_Sym sym;
        std::memcpy(&sym, buf.data() + (size_t)dynsym.sh_offset + i * sizeof(Elf64_Sym),
                    sizeof(sym));
        if (sym.st_name == 0 || sym.st_shndx == SHN_UNDEF) continue;
        if (sym.st_name >= strtab.sh_size) continue;
        const char* name = reinterpret_cast<const char*>(buf.data() + strtab.sh_offset + sym.st_name);
        size_t maxlen = (size_t)strtab.sh_size - sym.st_name;
        size_t n = 0;
        while (n < maxlen && name[n] != '\0') ++n;
        if (n == maxlen || std::string(name, n) != symbol) continue;
        if (sym.st_value == 0) continue;
        // vaddr -> file offset：uprobe_events 需要文件偏移
        for (int p = 0; p < (int)eh.e_phnum; ++p) {
            size_t poff = (size_t)eh.e_phoff + (size_t)p * eh.e_phentsize;
            if (poff + sizeof(Elf64_Phdr) > buf.size()) break;
            Elf64_Phdr ph;
            std::memcpy(&ph, buf.data() + poff, sizeof(ph));
            if (ph.p_type != PT_LOAD) continue;
            if (sym.st_value >= ph.p_vaddr &&
                sym.st_value < ph.p_vaddr + ph.p_filesz) {
                unsigned long long file_off = ph.p_offset + (sym.st_value - ph.p_vaddr);
                if (file_off < buf.size()) return (long long)file_off;
            }
        }
    }
    return -1;
}

}  // namespace xdd
