#pragma once
#include "model.h"
namespace zen {
// 格式版本和长度上限均在读取时验证；未来版本不会被旧程序覆盖。
struct Writer {
    std::vector<uint8_t> bytes;
    template <class T> void Pod(const T &x) {
        auto *p = (const uint8_t *)&x;
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void Text(const std::wstring &s) {
        Pod((uint32_t)s.size());
        auto *p = (const uint8_t *)s.data();
        bytes.insert(bytes.end(), p, p + s.size() * 2);
    }
};
struct Reader {
    const std::vector<uint8_t> &bytes;
    size_t pos = 0;
    template <class T> T Pod() {
        if (pos + sizeof(T) > bytes.size())
            throw std::runtime_error("文件截断");
        if constexpr (std::is_same_v<T, bool>) {
            if (bytes[pos] > 1)
                throw std::runtime_error("布尔值无效");
        }
        T x;
        memcpy(&x, bytes.data() + pos, sizeof(T));
        pos += sizeof(T);
        return x;
    }
    uint32_t Count(uint32_t limit = 4096) {
        auto n = Pod<uint32_t>();
        if (n > limit)
            throw std::runtime_error("数量超过上限");
        return n;
    }
    std::wstring Text() {
        auto n = Count(32768);
        if (pos + n * 2 > bytes.size())
            throw std::runtime_error("字符串截断");
        std::wstring s(n, 0);
        memcpy(s.data(), bytes.data() + pos, n * 2);
        pos += n * 2;
        if (s.find(L'\0') != std::wstring::npos)
            throw std::runtime_error("字符串含空字符");
        return s;
    }
};
bool WriteEnvelope(const std::filesystem::path &, uint32_t magic, const std::vector<uint8_t> &, bool backup,
                   uint32_t version = 1);
std::vector<uint8_t> ReadEnvelope(const std::filesystem::path &, uint32_t magic, uint32_t maxVersion = 1,
                                  uint32_t *version = nullptr);
// 配置写到指定路径，最近关闭和各标签导航历史写到 DataDir()/history。
// 配置引用同代历史，保存失败不提交配置；读取失败尝试完整备份配对。
bool SaveModel(const Model &, const std::filesystem::path &);
bool LoadModel(Model &, const std::filesystem::path &, bool &recovered, bool &future,
               bool *obsolete = nullptr);
} // namespace zen
