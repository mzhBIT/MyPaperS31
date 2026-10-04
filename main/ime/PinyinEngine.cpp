#include "ime/PinyinEngine.h"
#include "storage/app_paths.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>
#include <esp_log.h>
#include <esp_timer.h>

namespace ime {
namespace {

constexpr const char *TAG = "PINYIN_IME";

struct Entry { const char *key; const char *values; };

#pragma pack(push, 1)
struct BinaryHeader {
    char magic[4];
    uint32_t version;
    uint32_t count;
};
struct BinaryRecord {
    char key[32];
    uint32_t value_offset;
    uint16_t value_size;
    uint16_t reserved;
};
struct T9IndexHeader {
    char magic[4];
    uint32_t version;
    uint32_t count;
};
struct T9IndexRecord {
    char digits[32];
    uint32_t source_index;
};
struct T9IndexRecordV2 {
    char key[32];
};
#pragma pack(pop)

// Small always-available vocabulary. A larger dictionary can be placed at
// /sdcard/.reader/system/ime/pinyin.txt without rebuilding the firmware.
static const Entry BUILTIN[] = {
    {"a", "啊 阿 呵"}, {"ai", "爱 哎 矮 挨 艾"}, {"an", "安 按 暗 岸 案"},
    {"ang", "昂 盎"}, {"ao", "奥 熬 傲 澳 凹"}, {"ba", "把 八 吧 爸 巴"},
    {"bai", "白 百 摆 败 拜"}, {"ban", "办 半 班 般 版"}, {"bang", "帮 棒 邦 榜"},
    {"bao", "包 报 保 宝 抱"}, {"bei", "被 北 倍 杯 背"}, {"ben", "本 奔 笨"},
    {"bi", "比 必 笔 彼 壁"}, {"bian", "边 变 便 编 遍"}, {"bie", "别 憋"},
    {"bin", "宾 滨 彬"}, {"bing", "并 病 冰 兵"}, {"bo", "波 播 博 伯"},
    {"bu", "不 部 步 布 补"}, {"cai", "才 菜 财 彩 猜"}, {"can", "参 残 惨 餐"},
    {"ce", "测 侧 册 策"}, {"cha", "查 差 茶 插"}, {"chang", "长 常 场 唱 厂"},
    {"che", "车 彻 撤"}, {"chen", "陈 晨 沉 尘"}, {"cheng", "成 城 程 称 承"},
    {"chi", "吃 持 迟 赤 尺"}, {"chong", "重 冲 充 虫"}, {"chu", "出 处 初 除 楚"},
    {"chuan", "传 穿 船 川"}, {"ci", "次 此 词 辞"}, {"cong", "从 聪 丛"},
    {"da", "大 打 达 答"}, {"dai", "带 代 待 戴"}, {"dan", "但 单 担 淡"},
    {"dao", "到 道 导 岛 倒"}, {"de", "的 得 德"}, {"deng", "等 灯 登 邓"},
    {"di", "地 第 低 底 弟"}, {"dian", "点 电 店 典"}, {"dong", "动 东 懂 冬"},
    {"du", "读 度 都 独"}, {"dui", "对 队 堆"}, {"duo", "多 朵 躲 夺"},
    {"e", "饿 额 俄 恶"}, {"en", "嗯 恩"}, {"er", "而 二 儿 耳"},
    {"fa", "发 法 罚"}, {"fan", "反 饭 范 翻"}, {"fang", "方 放 房 防"},
    {"fei", "非 飞 费 肥"}, {"fen", "分 份 粉 奋"}, {"feng", "风 封 峰 丰"},
    {"fu", "服 复 福 负 富"}, {"gan", "干 感 敢 甘"}, {"gang", "刚 港 钢"},
    {"gao", "高 告 稿 搞"}, {"ge", "个 各 歌 格 哥"}, {"gen", "跟 根"},
    {"geng", "更 耕 梗"}, {"gong", "工 公 功 共"}, {"gu", "古 故 顾 股"},
    {"guan", "关 管 官 观"}, {"guang", "光 广 逛"}, {"gui", "规 贵 归 鬼"},
    {"guo", "国 过 果 锅"}, {"hai", "还 海 孩 害"}, {"hao", "好 号 毫 豪"},
    {"he", "和 合 河 何 喝"}, {"hen", "很 狠 恨"}, {"hong", "红 宏 洪"},
    {"hou", "后 候 厚"}, {"hua", "话 花 华 画"}, {"huan", "换 还 欢 环"},
    {"hui", "会 回 灰 汇"}, {"huo", "或 活 火 货"}, {"ji", "机 几 及 记 己"},
    {"jia", "家 加 假 价"}, {"jian", "见 件 间 建 简"}, {"jiang", "将 讲 江 降"},
    {"jiao", "叫 教 交 脚 角"}, {"jie", "接 节 解 界 姐"}, {"jin", "进 今 近 金"},
    {"jing", "经 京 精 景 静"}, {"jiu", "就 九 久 酒"}, {"ju", "句 局 具 举"},
    {"kai", "开 凯 慨"}, {"kan", "看 刊 砍"}, {"ke", "可 科 克 客"},
    {"kong", "空 控 孔"}, {"kuai", "快 块"}, {"lai", "来 赖"},
    {"lao", "老 劳 牢"}, {"le", "了 乐 勒"}, {"li", "里 理 力 利 离"},
    {"lian", "连 联 脸 练"}, {"liang", "两 量 亮 良"}, {"liao", "了 料 聊"},
    {"lin", "林 临 邻"}, {"ling", "领 令 零 灵"}, {"liu", "六 流 留 刘"},
    {"lu", "路 录 陆 炉"}, {"ma", "吗 马 妈 码"}, {"mai", "买 卖 麦"},
    {"mei", "没 每 美 妹"}, {"men", "们 门 闷"}, {"ming", "名 明 命"},
    {"mu", "目 木 母 幕"}, {"na", "那 哪 拿 纳"}, {"nan", "南 难 男"},
    {"neng", "能"}, {"ni", "你 尼 泥"}, {"nian", "年 念 粘"},
    {"peng", "朋 碰 彭"}, {"qi", "起 其 期 气 七"}, {"qian", "前 钱 千 签"},
    {"qing", "请 情 清 青"}, {"qu", "去 取 区 曲"}, {"ran", "然 燃"},
    {"ren", "人 认 任 仁"}, {"ri", "日"}, {"ru", "如 入 乳"},
    {"san", "三 散 伞"}, {"shang", "上 商 伤 尚"}, {"she", "社 设 舍"},
    {"shen", "什 身 深 神"}, {"sheng", "生 声 省 胜"}, {"shi", "是 时 事 十 使"},
    {"shou", "手 受 收 首"}, {"shu", "书 数 属 树"}, {"shui", "水 谁 睡"},
    {"shuo", "说 硕"}, {"si", "四 思 死 司"}, {"song", "送 松 宋"},
    {"ta", "他 她 它 塔"}, {"tai", "太 台 态"}, {"tian", "天 田 填"},
    {"ting", "听 停 厅"}, {"tong", "同 通 童 痛"}, {"tou", "头 投 偷"},
    {"wai", "外 歪"}, {"wan", "万 完 晚 玩"}, {"wang", "王 往 网 忘"},
    {"wei", "为 位 未 味 微"}, {"wen", "问 文 闻 温"}, {"wo", "我 握 窝"},
    {"wu", "无 五 物 务 午"}, {"xi", "系 西 喜 习 希"}, {"xia", "下 夏 吓"},
    {"xian", "现 先 线 县 显"}, {"xiang", "想 向 相 像 乡"}, {"xiao", "小 笑 校 消"},
    {"xin", "新 心 信 欣"}, {"xing", "行 性 星 型"}, {"xue", "学 雪 血"},
    {"ya", "呀 压 牙 亚"}, {"yan", "眼 言 研 验"}, {"yang", "样 养 阳 洋"},
    {"yao", "要 药 摇 邀"}, {"ye", "也 业 夜 页"}, {"yi", "一 以 已 意 易"},
    {"yin", "因 音 引 银"}, {"ying", "应 英 影 营"}, {"yong", "用 永 勇"},
    {"you", "有 又 由 友"}, {"yu", "于 与 语 雨"}, {"yuan", "原 员 远 院"},
    {"yue", "月 越 约 乐"}, {"zai", "在 再 载"}, {"zen", "怎"},
    {"zhan", "站 战 展 占"}, {"zhang", "长 张 章 涨"}, {"zhe", "这 者 着"},
    {"zhen", "真 镇 针 阵"}, {"zheng", "正 整 政 争"}, {"zhi", "之 只 知 直 制"},
    {"zhong", "中 种 重 众"}, {"zhou", "周 州 走 舟"}, {"zhu", "主 住 注 助"},
    {"zi", "自 子 字 资"}, {"zou", "走 奏"}, {"zuo", "做 作 坐 左"},
    {"nihao", "你好"}, {"zhongguo", "中国"}, {"wenjian", "文件"}, {"shuru", "输入"},
    {"yuedu", "阅读"}, {"yinyue", "音乐"}, {"jiangnan", "江南"},
};

static void append_words(const char *text, std::vector<std::string> *out)
{
    if (!text || !out) return;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (p > start) {
            std::string word(start, p - start);
            if (std::find(out->begin(), out->end(), word) == out->end()) out->push_back(std::move(word));
        }
    }
}

using Segmentation = std::vector<const Entry *>;

static bool is_syllable_entry(const Entry &entry)
{
    // Mandarin syllables are at most six ASCII letters. Longer keys in the
    // built-in table are explicit phrase entries and must not participate in
    // automatic syllable segmentation.
    size_t length = std::strlen(entry.key);
    return length > 0 && length <= 6 && std::strchr(entry.key, '\'') == nullptr;
}

static void segment_part(const std::string &part, size_t offset,
                         Segmentation *current,
                         std::vector<Segmentation> *results)
{
    if (!current || !results || results->size() >= 12 || current->size() >= 8) return;
    if (offset == part.size()) {
        if (!current->empty()) results->push_back(*current);
        return;
    }
    for (const auto &entry : BUILTIN) {
        if (!is_syllable_entry(entry)) continue;
        size_t length = std::strlen(entry.key);
        if (offset + length > part.size() || part.compare(offset, length, entry.key) != 0) continue;
        current->push_back(&entry);
        segment_part(part, offset + length, current, results);
        current->pop_back();
        if (results->size() >= 12) return;
    }
}

static std::vector<Segmentation> segment_composition(const std::string &composition)
{
    std::vector<Segmentation> combined(1);
    size_t start = 0;
    while (start <= composition.size()) {
        size_t separator = composition.find('\'', start);
        std::string part = composition.substr(start,
            separator == std::string::npos ? std::string::npos : separator - start);
        if (part.empty()) return {};
        std::vector<Segmentation> part_results;
        Segmentation current;
        segment_part(part, 0, &current, &part_results);
        if (part_results.empty()) return {};

        std::vector<Segmentation> next;
        for (const auto &prefix : combined) {
            for (const auto &suffix : part_results) {
                Segmentation joined = prefix;
                joined.insert(joined.end(), suffix.begin(), suffix.end());
                next.push_back(std::move(joined));
                if (next.size() >= 12) break;
            }
            if (next.size() >= 12) break;
        }
        combined.swap(next);
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return combined;
}

static std::vector<std::string> entry_words(const Entry &entry, size_t limit)
{
    std::vector<std::string> words;
    append_words(entry.values, &words);
    if (words.size() > limit) words.resize(limit);
    return words;
}

static void append_segmented_candidates(const std::string &composition,
                                        std::vector<std::string> *candidates)
{
    if (!candidates) return;
    auto segmentations = segment_composition(composition);
    for (const auto &segmentation : segmentations) {
        // A single syllable is already represented by the exact lookup.
        if (segmentation.size() < 2) continue;
        std::vector<std::string> beam(1);
        for (const Entry *entry : segmentation) {
            std::vector<std::string> words = entry_words(*entry, 3);
            std::vector<std::string> next;
            for (const auto &prefix : beam) {
                for (const auto &word : words) {
                    next.push_back(prefix + word);
                    if (next.size() >= 24) break;
                }
                if (next.size() >= 24) break;
            }
            beam.swap(next);
            if (beam.empty()) break;
        }
        for (auto &candidate : beam) {
            if (std::find(candidates->begin(), candidates->end(), candidate) == candidates->end()) {
                candidates->push_back(std::move(candidate));
                if (candidates->size() >= 64) return;
            }
        }
    }
}

char t9_digit(unsigned char c)
{
    c = static_cast<unsigned char>(std::tolower(c));
    if (c >= 'a' && c <= 'c') return '2';
    if (c >= 'd' && c <= 'f') return '3';
    if (c >= 'g' && c <= 'i') return '4';
    if (c >= 'j' && c <= 'l') return '5';
    if (c >= 'm' && c <= 'o') return '6';
    if (c >= 'p' && c <= 's') return '7';
    if (c >= 't' && c <= 'v') return '8';
    if (c >= 'w' && c <= 'z') return '9';
    if (c == '\'') return '0';
    return '\0';
}

std::string t9_digits(const char *key)
{
    std::string digits;
    if (!key) return digits;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(key); *p; ++p) {
        const char digit = t9_digit(*p);
        if (digit) digits.push_back(digit);
    }
    return digits;
}

void append_unique_key(std::vector<std::string> *keys, const std::string &key,
                       size_t limit)
{
    if (!keys || key.empty() || keys->size() >= limit) return;
    if (std::find(keys->begin(), keys->end(), key) == keys->end()) {
        keys->push_back(key);
    }
}

std::string t9_prefix_for(const char *key, size_t digit_count)
{
    std::string prefix;
    if (!key) return prefix;
    size_t used = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(key);
         *p && used < digit_count; ++p) {
        if (!t9_digit(*p)) continue;
        prefix.push_back(static_cast<char>(std::tolower(*p)));
        ++used;
    }
    return prefix;
}

void collect_t9_key(const char *key, const std::string &digits,
                    std::vector<std::string> *exact,
                    std::vector<std::string> *prefix)
{
    if (!key || !*key) return;
    const std::string signature = t9_digits(key);
    if (signature == digits) {
        append_unique_key(exact, key, 32);
    } else if (signature.size() > digits.size() &&
               signature.compare(0, digits.size(), digits) == 0) {
        append_unique_key(prefix, t9_prefix_for(key, digits.size()), 32);
    }
}

bool collect_indexed_binary_t9_v2(FILE *fp, const BinaryHeader &header,
                                  const std::string &digits,
                                  std::vector<std::string> *exact,
                                  std::vector<std::string> *prefix)
{
    if (std::fseek(fp, 0, SEEK_END) != 0) return false;
    const long file_size = std::ftell(fp);
    const uint64_t index_size = sizeof(T9IndexHeader) +
        static_cast<uint64_t>(header.count) * sizeof(T9IndexRecordV2);
    if (file_size < 0 || static_cast<uint64_t>(file_size) < index_size) return false;
    const long index_offset = static_cast<long>(static_cast<uint64_t>(file_size) - index_size);
    if (std::fseek(fp, index_offset, SEEK_SET) != 0) return false;
    T9IndexHeader index_header = {};
    if (std::fread(&index_header, 1, sizeof(index_header), fp) != sizeof(index_header) ||
        std::memcmp(index_header.magic, "PY92", 4) != 0 || index_header.version != 2 ||
        index_header.count != header.count) return false;

    auto read_index = [&](uint32_t index, T9IndexRecordV2 *record) {
        const uint64_t offset = static_cast<uint64_t>(index_offset) +
                                sizeof(T9IndexHeader) +
                                static_cast<uint64_t>(index) * sizeof(T9IndexRecordV2);
        return std::fseek(fp, static_cast<long>(offset), SEEK_SET) == 0 &&
               std::fread(record, 1, sizeof(*record), fp) == sizeof(*record);
    };
    uint32_t low = 0;
    uint32_t high = index_header.count;
    T9IndexRecordV2 indexed = {};
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        if (!read_index(mid, &indexed)) return false;
        indexed.key[sizeof(indexed.key) - 1] = '\0';
        if (t9_digits(indexed.key) < digits) low = mid + 1;
        else high = mid;
    }

    // PY92 stores pinyin beside its signature. After lower_bound, matching
    // records are sequential and no random reads of the PYD1 table are needed.
    const uint64_t first_offset = static_cast<uint64_t>(index_offset) +
        sizeof(T9IndexHeader) + static_cast<uint64_t>(low) * sizeof(T9IndexRecordV2);
    if (std::fseek(fp, static_cast<long>(first_offset), SEEK_SET) != 0) return false;
    size_t scanned = 0;
    while (low < index_header.count && scanned < 256 &&
           (exact->size() < 24 || prefix->size() < 24)) {
        if (std::fread(&indexed, 1, sizeof(indexed), fp) != sizeof(indexed)) return false;
        ++low;
        ++scanned;
        indexed.key[sizeof(indexed.key) - 1] = '\0';
        const std::string signature = t9_digits(indexed.key);
        const bool is_exact = digits == signature;
        if (signature.compare(0, digits.size(), digits) != 0) break;
        if (is_exact) append_unique_key(exact, indexed.key, 32);
        else append_unique_key(prefix, t9_prefix_for(indexed.key, digits.size()), 32);
    }
    return true;
}

bool collect_indexed_binary_t9_v1(FILE *fp, const BinaryHeader &header,
                                  const std::string &digits,
                                  std::vector<std::string> *exact,
                                  std::vector<std::string> *prefix)
{
    if (std::fseek(fp, 0, SEEK_END) != 0) return false;
    const long file_size = std::ftell(fp);
    const uint64_t index_size = sizeof(T9IndexHeader) +
        static_cast<uint64_t>(header.count) * sizeof(T9IndexRecord);
    if (file_size < 0 || static_cast<uint64_t>(file_size) < index_size) return false;
    const long index_offset = static_cast<long>(static_cast<uint64_t>(file_size) - index_size);
    if (std::fseek(fp, index_offset, SEEK_SET) != 0) return false;
    T9IndexHeader index_header = {};
    if (std::fread(&index_header, 1, sizeof(index_header), fp) != sizeof(index_header) ||
        std::memcmp(index_header.magic, "PY91", 4) != 0 || index_header.version != 1 ||
        index_header.count != header.count) return false;

    auto read_index = [&](uint32_t index, T9IndexRecord *record) {
        const uint64_t offset = static_cast<uint64_t>(index_offset) +
                                sizeof(T9IndexHeader) +
                                static_cast<uint64_t>(index) * sizeof(T9IndexRecord);
        return std::fseek(fp, static_cast<long>(offset), SEEK_SET) == 0 &&
               std::fread(record, 1, sizeof(*record), fp) == sizeof(*record);
    };
    uint32_t low = 0;
    uint32_t high = index_header.count;
    T9IndexRecord indexed = {};
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        if (!read_index(mid, &indexed)) return false;
        indexed.digits[sizeof(indexed.digits) - 1] = '\0';
        if (std::strcmp(indexed.digits, digits.c_str()) < 0) low = mid + 1;
        else high = mid;
    }

    FILE *source = std::fopen(app_paths::pinyin_dictionary_file(), "rb");
    if (!source) return false;
    bool ok = true;
    for (uint32_t index = low; index < index_header.count; ++index) {
        if (!read_index(index, &indexed)) {
            ok = false;
            break;
        }
        indexed.digits[sizeof(indexed.digits) - 1] = '\0';
        const bool is_exact = digits == indexed.digits;
        const bool is_prefix = std::strncmp(indexed.digits, digits.c_str(), digits.size()) == 0;
        if (!is_prefix) break;
        if ((is_exact && exact->size() >= 32) || (!is_exact && prefix->size() >= 32)) continue;
        BinaryRecord source_record = {};
        const uint64_t source_offset = sizeof(BinaryHeader) +
            static_cast<uint64_t>(indexed.source_index) * sizeof(BinaryRecord);
        if (indexed.source_index >= header.count ||
            std::fseek(source, static_cast<long>(source_offset), SEEK_SET) != 0 ||
            std::fread(&source_record, 1, sizeof(source_record), source) != sizeof(source_record)) {
            ok = false;
            break;
        }
        source_record.key[sizeof(source_record.key) - 1] = '\0';
        if (is_exact) append_unique_key(exact, source_record.key, 32);
        else append_unique_key(prefix, t9_prefix_for(source_record.key, digits.size()), 32);
        if (exact->size() >= 32 && prefix->size() >= 32) break;
    }
    std::fclose(source);
    return ok;
}

bool collect_indexed_binary_t9(FILE *fp, const BinaryHeader &header,
                               const std::string &digits,
                               std::vector<std::string> *exact,
                               std::vector<std::string> *prefix)
{
    if (collect_indexed_binary_t9_v2(fp, header, digits, exact, prefix)) return true;
    return collect_indexed_binary_t9_v1(fp, header, digits, exact, prefix);
}

bool collect_binary_t9(const std::string &digits,
                       std::vector<std::string> *exact,
                       std::vector<std::string> *prefix,
                       bool allow_full_scan)
{
    FILE *fp = std::fopen(app_paths::pinyin_dictionary_file(), "rb");
    if (!fp) return false;
    BinaryHeader header = {};
    const bool usable = std::fread(&header, 1, sizeof(header), fp) == sizeof(header) &&
                        std::memcmp(header.magic, "PYD1", 4) == 0 && header.version == 1 &&
                        header.count > 0 && header.count < 1000000;
    if (!usable) {
        std::fclose(fp);
        return false;
    }
    if (collect_indexed_binary_t9(fp, header, digits, exact, prefix)) {
        std::fclose(fp);
        return true;
    }
    // Old PYD1 files do not have the appended PY91 index. Preserve their
    // compatibility, but only pay for a complete scan when the built-in
    // syllables did not produce any usable interpretation.
    if (!allow_full_scan) {
        std::fclose(fp);
        return false;
    }
    std::fseek(fp, sizeof(BinaryHeader), SEEK_SET);
    BinaryRecord records[64] = {};
    uint32_t remaining = header.count;
    while (remaining != 0) {
        const size_t requested = std::min<size_t>(remaining, 64);
        const size_t count = std::fread(records, sizeof(BinaryRecord), requested, fp);
        if (count == 0) break;
        for (size_t i = 0; i < count; ++i) {
            records[i].key[sizeof(records[i].key) - 1] = '\0';
            collect_t9_key(records[i].key, digits, exact, prefix);
        }
        remaining -= static_cast<uint32_t>(count);
        if (count != requested) break;
    }
    std::fclose(fp);
    return false;
}

void collect_text_t9(const std::string &digits,
                     std::vector<std::string> *exact,
                     std::vector<std::string> *prefix)
{
    FILE *fp = std::fopen(app_paths::pinyin_text_file(), "rb");
    if (!fp) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        char *tab = std::strchr(line, '\t');
        if (tab) *tab = '\0';
        else {
            char *space = std::strchr(line, ' ');
            if (space) *space = '\0';
        }
        collect_t9_key(line, digits, exact, prefix);
    }
    std::fclose(fp);
}

}  // namespace

PinyinEngine::PinyinEngine(size_t page_size) : _page_size(std::max<size_t>(1, page_size)) {}

void PinyinEngine::set_composition(const std::string &composition)
{
    _composition.clear();
    for (unsigned char c : composition) {
        if (std::isalpha(c)) _composition.push_back(static_cast<char>(std::tolower(c)));
        else if (c == '\'') _composition.push_back('\'');
    }
    _page = 0;
    rebuild();
}

void PinyinEngine::rebuild()
{
    _candidates.clear();
    if (_composition.empty()) return;
    for (const auto &entry : BUILTIN) {
        if (_composition == entry.key) append_words(entry.values, &_candidates);
    }
    if (_composition.size() >= 2) {
        for (const auto &entry : BUILTIN) {
            if (_composition != entry.key &&
                std::strncmp(entry.key, _composition.c_str(), _composition.size()) == 0) {
                append_words(entry.values, &_candidates);
                if (_candidates.size() >= 64) break;
            }
        }
    }
    if (!load_binary_candidates()) load_external_candidates();
    append_segmented_candidates(_composition, &_candidates);
}

bool PinyinEngine::load_binary_candidates()
{
    FILE *fp = std::fopen(app_paths::pinyin_dictionary_file(), "rb");
    if (!fp) return false;
    BinaryHeader header = {};
    bool usable = std::fread(&header, 1, sizeof(header), fp) == sizeof(header) &&
                  std::memcmp(header.magic, "PYD1", 4) == 0 && header.version == 1 &&
                  header.count > 0 && header.count < 1000000;
    if (!usable) {
        std::fclose(fp);
        return false;
    }

    // Find the first dictionary key not less than the composition. This is
    // both the exact-match position and the beginning of the prefix range.
    uint32_t low = 0, high = header.count;
    BinaryRecord record = {};
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        long pos = static_cast<long>(sizeof(BinaryHeader) + static_cast<uint64_t>(mid) * sizeof(BinaryRecord));
        if (std::fseek(fp, pos, SEEK_SET) != 0 ||
            std::fread(&record, 1, sizeof(record), fp) != sizeof(record)) break;
        record.key[sizeof(record.key) - 1] = '\0';
        if (std::strcmp(record.key, _composition.c_str()) < 0) low = mid + 1;
        else high = mid;
    }

    const bool allow_prefix = _composition.size() >= 2;
    uint32_t scanned = 0;
    for (uint32_t index = low; index < header.count && scanned < 96 && _candidates.size() < 64;
         ++index, ++scanned) {
        long record_pos = static_cast<long>(sizeof(BinaryHeader) +
                                             static_cast<uint64_t>(index) * sizeof(BinaryRecord));
        if (std::fseek(fp, record_pos, SEEK_SET) != 0 ||
            std::fread(&record, 1, sizeof(record), fp) != sizeof(record)) break;
        record.key[sizeof(record.key) - 1] = '\0';
        bool exact = _composition == record.key;
        bool prefix = allow_prefix &&
                      std::strncmp(record.key, _composition.c_str(), _composition.size()) == 0;
        if (!exact && !prefix) break;
        if (record.value_size == 0 || record.value_size > 8192 ||
            std::fseek(fp, static_cast<long>(record.value_offset), SEEK_SET) != 0) continue;
        std::string values(record.value_size, '\0');
        if (std::fread(values.data(), 1, values.size(), fp) == values.size()) {
            append_words(values.c_str(), &_candidates);
        }
    }
    std::fclose(fp);
    return true;
}

void PinyinEngine::load_external_candidates()
{
    FILE *fp = std::fopen(app_paths::pinyin_text_file(), "rb");
    if (!fp) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        char *tab = std::strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        bool exact = _composition == line;
        bool prefix = _composition.size() >= 2 &&
                      std::strncmp(line, _composition.c_str(), _composition.size()) == 0;
        if (exact || prefix) {
            append_words(tab, &_candidates);
            if (_candidates.size() >= 64) break;
        }
    }
    std::fclose(fp);
}

size_t PinyinEngine::page_count() const
{
    return std::max<size_t>(1, (_candidates.size() + _page_size - 1) / _page_size);
}

bool PinyinEngine::next_page()
{
    if (_page + 1 >= page_count()) return false;
    ++_page;
    return true;
}

bool PinyinEngine::previous_page()
{
    if (_page == 0) return false;
    --_page;
    return true;
}

std::string PinyinEngine::candidate_on_page(size_t slot) const
{
    size_t index = _page * _page_size + slot;
    return index < _candidates.size() ? _candidates[index] : std::string();
}

Pinyin9Engine::Pinyin9Engine(size_t page_size)
    : _page_size(std::max<size_t>(1, page_size)), _engine(_page_size)
{
}

std::string Pinyin9Engine::digits_for(const std::string &pinyin)
{
    return t9_digits(pinyin.c_str());
}

void Pinyin9Engine::set_digits(const std::string &digits)
{
    _digits.clear();
    for (char c : digits) {
        if (c >= '2' && c <= '9') _digits.push_back(c);
        else if (c == '0' && !_digits.empty() && _digits.back() != '0') _digits.push_back(c);
    }
    rebuild();
}

bool Pinyin9Engine::append_digit(char digit)
{
    if (digit < '2' || digit > '9') {
        if (digit != '0' || _digits.empty() || _digits.back() == '0') return false;
    }
    if (_digits.size() >= 31) return false;
    _digits.push_back(digit);
    rebuild();
    return true;
}

bool Pinyin9Engine::erase_last()
{
    if (_digits.empty()) return false;
    _digits.pop_back();
    rebuild();
    return true;
}

void Pinyin9Engine::clear()
{
    _digits.clear();
    _compositions.clear();
    _interpretation = 0;
    _unmatched = false;
    _engine.set_composition("");
}

const std::string &Pinyin9Engine::composition() const
{
    static const std::string empty;
    return _compositions.empty() ? empty : _compositions[_interpretation];
}

const std::string &Pinyin9Engine::interpretation_at(size_t index) const
{
    static const std::string empty;
    return index < _compositions.size() ? _compositions[index] : empty;
}

void Pinyin9Engine::select_interpretation(size_t index)
{
    if (_compositions.empty()) {
        _interpretation = 0;
        _engine.set_composition("");
        return;
    }
    _interpretation = std::min(index, _compositions.size() - 1);
    // An unmatched spelling remains visible, but must not produce candidates
    // for a shorter prefix or be mistaken for a dictionary interpretation.
    _engine.set_composition(_unmatched ? "" : _compositions[_interpretation]);
}

bool Pinyin9Engine::next_interpretation()
{
    if (_interpretation + 1 >= _compositions.size()) return false;
    select_interpretation(_interpretation + 1);
    return true;
}

bool Pinyin9Engine::previous_interpretation()
{
    if (_interpretation == 0 || _compositions.empty()) return false;
    select_interpretation(_interpretation - 1);
    return true;
}

void Pinyin9Engine::rebuild()
{
    const int64_t start_us = esp_timer_get_time();
    const std::string previous = composition();
    _compositions.clear();
    _interpretation = 0;
    _unmatched = false;
    if (_digits.empty()) {
        _engine.set_composition("");
        return;
    }

    std::vector<std::string> exact;
    std::vector<std::string> prefix;
    static const char *PREFERRED[] = {
        "ni", "hao", "wo", "shi", "de", "le", "ma", "you", "zai", "ren",
        "zhong", "guo", "men", "yao", "he", "yi", "bu", "zhe", "na", "shuo",
        "nihao", "zhongguo", "wenjian", "shuru", "yuedu", "yinyue", "jiangnan",
    };
    for (const char *key : PREFERRED) collect_t9_key(key, _digits, &exact, &prefix);
    for (const auto &entry : BUILTIN) collect_t9_key(entry.key, _digits, &exact, &prefix);
    // One digit cannot identify a useful syllable yet. For complete built-in
    // syllables the normal PinyinEngine will still load the full SD candidate
    // list, so avoid rescanning every PYD1 record on those common key presses.
    bool scanned_dictionary = false;
    bool indexed_dictionary = false;
    if (_digits.size() > 1) {
        const bool allow_full_scan = exact.empty();
        indexed_dictionary = collect_binary_t9(_digits, &exact, &prefix,
                                                allow_full_scan);
        scanned_dictionary = allow_full_scan && !indexed_dictionary;
    }
    if (exact.empty() && prefix.empty()) collect_text_t9(_digits, &exact, &prefix);

    for (const auto &key : exact) append_unique_key(&_compositions, key, 24);
    for (const auto &key : prefix) append_unique_key(&_compositions, key, 24);
    if (_compositions.empty()) {
        // Keep the selected spelling's shared prefix after append/backspace.
        // Unknown suffix keys use their first letter, so an invalid sequence
        // is still visible without restoring the space-consuming digit label.
        const std::string previous_digits = t9_digits(previous.c_str());
        size_t shared = 0;
        while (shared < previous_digits.size() && shared < _digits.size() &&
               previous_digits[shared] == _digits[shared]) ++shared;
        std::string fallback = t9_prefix_for(previous.c_str(), shared);
        constexpr char first_letters[] = "'\0adgjmptw";
        for (size_t i = shared; i < _digits.size(); ++i) {
            fallback.push_back(first_letters[_digits[i] - '0']);
        }
        _compositions.push_back(std::move(fallback));
        _unmatched = true;
    }
    select_interpretation(0);
    ESP_LOGI(TAG, "nine-key digits=%s pinyin=%s options=%u dictionary_scan=%d indexed=%d time=%lld ms",
             _digits.c_str(), composition().c_str(),
             static_cast<unsigned>(_compositions.size()), scanned_dictionary ? 1 : 0,
             indexed_dictionary ? 1 : 0,
             (esp_timer_get_time() - start_us) / 1000);
}

}  // namespace ime
