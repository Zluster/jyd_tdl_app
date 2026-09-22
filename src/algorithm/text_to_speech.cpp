#include "tdl_app/text_to_speech.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"

#include "algorithm/private/bmrt_utils.hpp"
#include "algorithm/private/tts_pinyin_table.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace {

// 前端 bmodel 的 token 输入定长 32；解码器 latent 输入定长 192 帧。
constexpr int kTokenCapacity = 32;
constexpr int kDecoderFrames = 192;
constexpr int kHopLength = 256;
constexpr int kModelSampleRate = 22050;
constexpr int kOutputSampleRate = 16000;
constexpr int kBos = 1;
constexpr int kEos = 2;
constexpr int kPad = 0;
// 拼音表里没有声母时使用的占位 ID，与 Piper 中文前端一致。
constexpr int kNoInitialId = 3;
constexpr int kCommaId = 72;
constexpr int kPeriodId = 69;

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

std::string joinPath(const std::string &base, const std::string &path) {
  if (path.empty() || path.front() == '/' ||
      (path.size() > 1 && path[1] == ':')) {
    return path;
  }
  return base.empty() || base.back() == '/' ? base + path : base + "/" + path;
}

std::size_t shapeElements(const bm_shape_t &shape) {
  std::size_t elements = 1;
  for (int i = 0; i < shape.num_dims; ++i) {
    if (shape.dims[i] <= 0) return 0;
    elements *= static_cast<std::size_t>(shape.dims[i]);
  }
  return elements;
}

// 取出 UTF-8 文本的下一个字符，返回其字节数（非法字节按单字节处理）。
std::size_t nextChar(const std::string &text, std::size_t offset,
                     std::string *out) {
  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  std::size_t width = 1;
  if ((lead & 0xE0) == 0xC0) {
    width = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    width = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    width = 4;
  }
  if (offset + width > text.size()) width = 1;
  out->assign(text, offset, width);
  return width;
}

std::vector<std::string> splitChars(const std::string &text) {
  std::vector<std::string> chars;
  std::size_t offset = 0;
  while (offset < text.size()) {
    std::string ch;
    offset += nextChar(text, offset, &ch);
    chars.push_back(ch);
  }
  return chars;
}

bool isSpace(const std::string &ch) {
  if (ch.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(ch[0]);
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
  }
  return ch == "\xE3\x80\x80";  // U+3000 全角空格
}

// 半角与全角阿拉伯数字的数值，非数字返回 -1。
int digitValue(const std::string &ch) {
  if (ch.size() == 1 && ch[0] >= '0' && ch[0] <= '9') return ch[0] - '0';
  if (ch.size() == 3 && static_cast<unsigned char>(ch[0]) == 0xEF &&
      static_cast<unsigned char>(ch[1]) == 0xBC) {
    const unsigned char last = static_cast<unsigned char>(ch[2]);
    if (last >= 0x90 && last <= 0x99) return last - 0x90;
  }
  return -1;
}

struct IdEntry {
  const char *name;
  int id;
};

// 声母：按长度降序排列，保证 zh/ch/sh 先于单字母匹配。
const IdEntry kInitialIds[] = {
    {"zh", 18}, {"ch", 19}, {"sh", 20}, {"b", 4},  {"p", 5},  {"m", 6},
    {"f", 7},   {"d", 8},   {"t", 9},   {"n", 10}, {"l", 11}, {"g", 12},
    {"k", 13},  {"h", 14},  {"j", 15},  {"q", 16}, {"x", 17}, {"r", 21},
    {"z", 22},  {"c", 23},  {"s", 24},  {"y", 25}, {"w", 26},
};

const IdEntry kFinalIds[] = {
    {"a", 27},    {"o", 28},    {"e", 29},    {"ai", 30},   {"ei", 31},
    {"ao", 32},   {"ou", 33},   {"an", 34},   {"en", 35},   {"ang", 36},
    {"eng", 37},  {"ong", 38},  {"i", 39},    {"ia", 40},   {"ie", 41},
    {"iao", 42},  {"iu", 43},   {"ian", 44},  {"in", 45},   {"iang", 46},
    {"ing", 47},  {"iong", 48}, {"u", 49},    {"ua", 50},   {"uo", 51},
    {"uai", 52},  {"ui", 53},   {"uan", 54},  {"un", 55},   {"uang", 56},
    {"ueng", 57}, {"v", 58},    {"ve", 59},   {"van", 60},  {"vn", 61},
    {"er", 62},   {"ue", 63},
};

const IdEntry kToneIds[] = {
    {"1", 64}, {"2", 65}, {"3", 66}, {"4", 67}, {"5", 68},
};

const IdEntry kPunctuationIds[] = {
    {"\xE3\x80\x82", 69},  // 。
    {".", 69},
    {"\xEF\xBC\x9F", 70},  // ？
    {"?", 70},
    {"\xEF\xBC\x81", 71},  // ！
    {"!", 71},
    {"\xE2\x80\x94", 72},  // —
    {"\xE2\x80\xA6", 72},  // …
    {"\xE3\x80\x81", 72},  // 、
    {"\xEF\xBC\x8C", 72},  // ，
    {",", 72},
    {"\xEF\xBC\x9A", 72},  // ：
    {":", 72},
    {"\xEF\xBC\x9B", 72},  // ；
    {";", 72},
    {" ", 72},
};

template <std::size_t N>
bool lookupId(const IdEntry (&table)[N], const std::string &name, int *id) {
  for (std::size_t i = 0; i < N; ++i) {
    if (name == table[i].name) {
      *id = table[i].id;
      return true;
    }
  }
  return false;
}

// 汉字读音表：首次使用时把编译进来的两张字符串表 zip 成查表结构。
const std::unordered_map<std::string, std::string> &readingsTable() {
  static const std::unordered_map<std::string, std::string> table = [] {
    const std::string chars(tts_table::kChars);
    const std::string readings(tts_table::kReadings);
    std::vector<std::string> syllables;
    std::size_t start = 0;
    while (start < readings.size()) {
      const std::size_t space = readings.find(' ', start);
      if (space == std::string::npos) {
        syllables.push_back(readings.substr(start));
        break;
      }
      syllables.push_back(readings.substr(start, space - start));
      start = space + 1;
    }
    std::unordered_map<std::string, std::string> built;
    built.reserve(syllables.size() * 2);
    std::size_t offset = 0;
    std::size_t index = 0;
    std::string ch;
    while (offset < chars.size() && index < syllables.size()) {
      offset += nextChar(chars, offset, &ch);
      built.emplace(ch, syllables[index]);
      ++index;
    }
    return built;
  }();
  return table;
}

bool replaceAll(std::string *text, const std::string &from,
                const std::string &to) {
  if (from.empty()) return false;
  std::size_t position = 0;
  while ((position = text->find(from, position)) != std::string::npos) {
    text->replace(position, from.size(), to);
    position += to.size();
  }
  return true;
}

// 一组音素：音节是 [声母, 韵母, 声调, PAD]，标点是 [标点 ID, PAD]。
struct TtsGroup {
  std::vector<int> ids;
  bool punctuation = false;
};

// 一段待解码的 token 序列，附带它由哪些音素组构成（用于超长时再切分）。
struct TtsSegment {
  std::vector<int> tokens;
  std::vector<TtsGroup> groups;
};

const char kDigitChars[] = "零一二三四五六七八九";
const char *const kCardinalUnits[] = {"", "十", "百", "千"};

// 中文数字本身是 3 字节 UTF-8，必须整字取用；按字节下标取会截断成非法序列。
std::string digitChar(int digit) {
  return std::string(kDigitChars + digit * 3, 3);
}

// 1~4 位数字按中文基数读：25 -> 二十五，1005 -> 一千零五。
std::string readCardinal(const std::string &digits) {
  std::size_t begin = digits.find_first_not_of('0');
  const std::string value =
      begin == std::string::npos ? std::string() : digits.substr(begin);
  if (value.empty()) return digitChar(0);
  std::string text;
  bool zero_pending = false;
  const std::size_t length = value.size();
  for (std::size_t index = 0; index < length; ++index) {
    const int digit = value[index] - '0';
    if (digit == 0) {
      zero_pending = true;
      continue;
    }
    if (zero_pending && !text.empty()) text += digitChar(0);
    zero_pending = false;
    text += digitChar(digit);
    text += kCardinalUnits[length - index - 1];
  }
  // 中文把 10~19 读作“十五”而不是“一十五”。
  if (text.compare(0, 6, "一十") == 0) text.erase(0, 3);
  return text;
}

// 数字串的中文读法：1~4 位按基数读，年份与更长的串按位读。
std::string readDigits(const std::string &run, const std::string &following) {
  if (run.size() <= 4 && !(run.size() == 4 && following == "年")) {
    return readCardinal(run);
  }
  std::string text;
  for (char digit : run) text += digitChar(digit - '0');
  return text;
}

// 把数字与空白折算成前端认识的字符。
std::string normalizeText(const std::string &text) {
  const std::vector<std::string> chars = splitChars(text);
  std::string normalized;
  std::size_t index = 0;
  while (index < chars.size()) {
    if (digitValue(chars[index]) >= 0) {
      std::string digits;
      while (index < chars.size() && digitValue(chars[index]) >= 0) {
        digits.push_back(static_cast<char>('0' + digitValue(chars[index])));
        ++index;
      }
      normalized += readDigits(digits, index < chars.size() ? chars[index]
                                                            : std::string());
      continue;
    }
    // 空格本身已有标点 ID，换行/制表符统一折成空格。
    normalized += isSpace(chars[index]) ? std::string(" ") : chars[index];
    ++index;
  }
  return normalized;
}

// 带声调拼音 -> [声母 ID, 韵母 ID, 声调 ID, PAD]。
bool syllableIds(const std::string &syllable, std::vector<int> *ids,
                 std::string *error) {
  if (syllable.size() < 2) {
    setError(error, "无法转换拼音: " + syllable);
    return false;
  }
  std::string lowered;
  lowered.reserve(syllable.size());
  for (char raw : syllable) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    lowered.push_back(byte >= 'A' && byte <= 'Z'
                          ? static_cast<char>(byte - 'A' + 'a')
                          : raw);
  }
  const char tone = lowered.back();
  if (tone < '1' || tone > '5') {
    setError(error, "无法转换拼音: " + syllable);
    return false;
  }
  for (std::size_t i = 0; i + 1 < lowered.size(); ++i) {
    const unsigned char byte = static_cast<unsigned char>(lowered[i]);
    if ((byte >= 'a' && byte <= 'z') || byte == ':' || byte >= 0x80) continue;
    setError(error, "无法转换拼音: " + syllable);
    return false;
  }
  std::string base = lowered.substr(0, lowered.size() - 1);
  replaceAll(&base, "u:", "v");
  replaceAll(&base, "\xC3\xBC", "v");  // ü

  std::string initial;
  for (const IdEntry &entry : kInitialIds) {
    const std::string name(entry.name);
    if (base.compare(0, name.size(), name) == 0) {
      initial = name;
      break;
    }
  }
  std::string final = base.substr(initial.size());
  if (initial == "j" || initial == "q" || initial == "x") {
    if (final == "u") {
      final = "v";
    } else if (final == "ue") {
      final = "ve";
    } else if (final == "uan") {
      final = "van";
    } else if (final == "un") {
      final = "vn";
    }
  }
  int initial_id = kNoInitialId;
  lookupId(kInitialIds, initial, &initial_id);
  int final_id = 0;
  if (!lookupId(kFinalIds, final, &final_id)) {
    setError(error, "不支持的拼音韵母: " + final + " (" + syllable + ")");
    return false;
  }
  int tone_id = 0;
  lookupId(kToneIds, std::string(1, tone), &tone_id);
  ids->assign({initial_id, final_id, tone_id, kPad});
  return true;
}

struct RawChar {
  std::string ch;
  std::string syllable;
  bool punctuation = false;
};

// 文本 -> 音素组。标点自成一组，汉字按读音拆成音节组。
bool textGroups(const std::string &text, std::vector<TtsGroup> *groups,
                std::string *error) {
  const std::vector<std::string> chars = splitChars(normalizeText(text));
  std::size_t begin = 0;
  std::size_t end = chars.size();
  while (begin < end && isSpace(chars[begin])) ++begin;
  while (end > begin && isSpace(chars[end - 1])) --end;

  const std::unordered_map<std::string, std::string> &readings =
      readingsTable();
  std::vector<RawChar> raw;
  raw.reserve(end - begin);
  for (std::size_t index = begin; index < end; ++index) {
    RawChar item;
    item.ch = chars[index];
    int punctuation_id = 0;
    if (lookupId(kPunctuationIds, item.ch, &punctuation_id)) {
      item.punctuation = true;
    } else {
      const auto found = readings.find(item.ch);
      if (found == readings.end()) {
        setError(error, "暂不支持字符: " + item.ch +
                            "（仅支持中文、数字与标点）");
        return false;
      }
      item.syllable = found->second;
    }
    raw.push_back(std::move(item));
  }

  // 两个最常见的连读变调：不 + 四声 -> 二声；一 + 四声 -> 二声，否则四声。
  for (std::size_t index = 0; index + 1 < raw.size(); ++index) {
    const std::string &next = raw[index + 1].syllable;
    if (next.empty()) continue;
    const char next_tone = next.back();
    if (raw[index].ch == "不" && next_tone == '4') {
      raw[index].syllable.back() = '2';
    } else if (raw[index].ch == "一") {
      raw[index].syllable.back() = next_tone == '4' ? '2' : '4';
    }
  }

  for (const RawChar &item : raw) {
    TtsGroup group;
    if (item.punctuation) {
      int punctuation_id = 0;
      lookupId(kPunctuationIds, item.ch, &punctuation_id);
      group.ids.assign({punctuation_id, kPad});
      group.punctuation = true;
    } else if (!syllableIds(item.syllable, &group.ids, error)) {
      return false;
    }
    groups->push_back(std::move(group));
  }
  return true;
}

// 按 token 预算把音素组切成若干段，每段不超过 32 个 token（含 BOS/EOS）。
bool packGroups(const std::vector<TtsGroup> &groups,
                std::vector<TtsSegment> *segments, std::string *error) {
  if (groups.empty()) {
    setError(error, "请输入需要合成的中文");
    return false;
  }
  std::vector<TtsSegment> packed;
  std::vector<TtsGroup> current;
  std::vector<int> payload;

  auto finish = [&](bool has_pause, int pause_id) {
    if (current.empty()) return true;
    std::vector<int> tail = payload;
    if (has_pause) {
      tail.push_back(pause_id);
      tail.push_back(kPad);
    }
    TtsSegment segment;
    segment.tokens.reserve(tail.size() + 2);
    segment.tokens.push_back(kBos);
    segment.tokens.insert(segment.tokens.end(), tail.begin(), tail.end());
    segment.tokens.push_back(kEos);
    if (static_cast<int>(segment.tokens.size()) > kTokenCapacity) {
      setError(error, "TTS 单段 token 超过模型容量");
      return false;
    }
    segment.groups = std::move(current);
    current.clear();
    payload.clear();
    packed.push_back(std::move(segment));
    return true;
  };

  for (const TtsGroup &group : groups) {
    // 段尾要预留 EOS，或 逗号 + PAD + EOS。
    const int reserve = group.punctuation ? 1 : 3;
    if (1 + static_cast<int>(payload.size()) +
            static_cast<int>(group.ids.size()) + reserve >
        kTokenCapacity) {
      if (!finish(true, kCommaId)) return false;
    }
    current.push_back(group);
    payload.insert(payload.end(), group.ids.begin(), group.ids.end());
    if (group.punctuation) {
      if (!finish(false, 0)) return false;
    }
  }
  if (!finish(true, kPeriodId)) return false;
  *segments = std::move(packed);
  return true;
}

}  // namespace
}  // namespace tdl_app

namespace tdl_app {
namespace {

class RuntimeNet;

// 渲染一组音节，按真实时长裁剪后追加到 waveform。
//
// 前端输出真实时长后，如果语音本身超过解码器 192 帧预算，就在音节边界上
// 二分成两段分别合成再首尾相接；若越界的只是句尾静音，则直接截断。
// 函数体要用到后面第三个匿名命名空间块里的 RuntimeNet 与时长工具，所以定义
// 放在文件末尾的“渲染实现”一节。
bool render(const RuntimeNet &frontend, const RuntimeNet &decoder,
            const std::vector<TtsGroup> &groups, std::vector<float> *waveform,
            TtsSynthesisStats *stats, std::string *error);

// decoder 的 float32 波形（[-1, 1]）量化为 int16 PCM。
//
// 快路径用 copysign(0.5) 实现四舍五入，与 Python 参考实现逐样本一致；只有
// 解码结果越界时才回退到带削顶的路径，正常语音走不到那里。
bool quantizePcm16(const std::vector<float> &waveform,
                   std::vector<std::int16_t> *pcm, std::string *error) {
  pcm->resize(waveform.size());
  for (std::size_t index = 0; index < waveform.size(); ++index) {
    const float sample = waveform[index];
    if (!std::isfinite(sample)) {
      setError(error, "decoder 输出包含非有限数");
      return false;
    }
    const double scaled = static_cast<double>(sample) * 32767.0;
    const double fast =
        std::trunc(scaled + std::copysign(0.5, static_cast<double>(sample)));
    if (fast >= -32768.0 && fast <= 32767.0) {
      (*pcm)[index] = static_cast<std::int16_t>(fast);
    } else {
      (*pcm)[index] = static_cast<std::int16_t>(std::nearbyint(scaled));
    }
  }
  return true;
}

// audioop.ratecv(width=2, nchannels=1, weightA=1, weightB=0, state=None) 的
// 等价实现：整数相位插值，输出按 int16 截断。
// 工程按 gnu++14 编译，标准库还没有 std::gcd，这里自带一个。
int greatestCommonDivisor(int a, int b) {
  if (a < 0) a = -a;
  if (b < 0) b = -b;
  while (b != 0) {
    const int remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

std::vector<std::int16_t> resamplePcm16(const std::vector<std::int16_t> &input,
                                        int in_rate, int out_rate) {
  std::vector<std::int16_t> output;
  if (input.empty() || in_rate <= 0 || out_rate <= 0) return output;
  const int divisor = greatestCommonDivisor(in_rate, out_rate);
  in_rate /= divisor;
  out_rate /= divisor;
  output.reserve(static_cast<std::size_t>(
      static_cast<double>(input.size()) * out_rate / in_rate + 8.0));

  std::ptrdiff_t remaining = static_cast<std::ptrdiff_t>(input.size());
  std::size_t cursor = 0;
  int phase = -out_rate;
  int previous = 0;
  int current = 0;
  for (;;) {
    while (phase < 0) {
      if (remaining == 0) return output;
      previous = current;
      // 与 audioop 的 GETSAMPLE32(2) 一致：int16 左移 16 位放进 int。
      current = static_cast<int>(input[cursor]) * 65536;
      ++cursor;
      --remaining;
      phase += out_rate;
    }
    while (phase >= 0) {
      const double value =
          (static_cast<double>(previous) * phase +
           static_cast<double>(current) * (out_rate - phase)) /
          out_rate;
      // 与 audioop 的 SETSAMPLE32(2) 一致：算术右移 16 位后取低 16 位。
      output.push_back(
          static_cast<std::int16_t>(static_cast<int>(value) >> 16));
      phase -= in_rate;
    }
  }
}

void putLittleEndian(std::uint8_t *target, std::uint32_t value) {
  target[0] = static_cast<std::uint8_t>(value & 0xFF);
  target[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  target[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  target[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

// 写标准 44 字节头的单声道 16 位 WAV，与 Python wave 模块输出一致。
bool writeWav(const std::string &path, const std::vector<std::int16_t> &samples,
              int sample_rate, std::string *error) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    setError(error, "无法写入 WAV 文件: " + path);
    return false;
  }
  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(samples.size() * 2);
  std::uint8_t header[44] = {};
  std::memcpy(header, "RIFF", 4);
  putLittleEndian(header + 4, 36 + data_bytes);
  std::memcpy(header + 8, "WAVEfmt ", 8);
  putLittleEndian(header + 16, 16);
  header[20] = 1;  // PCM
  header[22] = 1;  // 单声道
  putLittleEndian(header + 24, static_cast<std::uint32_t>(sample_rate));
  putLittleEndian(header + 28,
                  static_cast<std::uint32_t>(sample_rate) * 2);
  header[32] = 2;  // 块对齐
  header[34] = 16;  // 位深
  std::memcpy(header + 36, "data", 4);
  putLittleEndian(header + 40, data_bytes);
  output.write(reinterpret_cast<const char *>(header), sizeof(header));
  if (!samples.empty()) {
    output.write(reinterpret_cast<const char *>(samples.data()),
                 static_cast<std::streamsize>(data_bytes));
  }
  if (!output) {
    setError(error, "写入 WAV 文件失败: " + path);
    return false;
  }
  return true;
}

}  // namespace
}  // namespace tdl_app

namespace tdl_app {
namespace {

// 把一段逐 token 时长累加成 latent 帧数（非有限值按 0 处理）。
double spanFrames(const std::vector<float> &values) {
  double total = 0.0;
  for (float value : values) {
    if (std::isfinite(value) && value > 0.0f) total += value;
  }
  return total;
}

// 与 Python round() 一致的四舍五入（就近取偶）。
int roundedFrames(double frames) {
  return static_cast<int>(std::nearbyint(frames));
}

std::vector<float> sliceFloats(const std::vector<float> &values,
                               std::size_t begin, std::size_t end) {
  if (begin > values.size()) begin = values.size();
  if (end > values.size()) end = values.size();
  if (end < begin) end = begin;
  return std::vector<float>(values.begin() + begin, values.begin() + end);
}

int framesOf(const std::vector<float> &durations) {
  return roundedFrames(spanFrames(durations));
}

// 语音（含标点）实际占用的帧数，忽略句尾的 PAD/EOS 静音。
//
// 前端会在句尾额外铺一段 PAD 静音和 EOS，这段即使越过 192 帧预算，截掉也
// 听不出来；只有真正的语音越界才需要切分，否则会平白多出一段段间停顿。
int speechFrames(const std::vector<float> &durations,
                 const std::vector<int> &tokens) {
  std::size_t last = 0;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index] == kPad || tokens[index] == kEos) continue;
    last = index + 1;
  }
  return roundedFrames(spanFrames(sliceFloats(durations, 0, last)));
}

// 在解码器帧预算内，找出能放下的最大完整音节组数量。
//
// durations 对应整段 [BOS] + payload + [EOS]，下标 0 是 BOS，因此从下标 1
// 开始累加音节。段尾的停顿与 EOS 同样占用 192 帧预算，必须一并扣掉，否则
// 最后一个音节会被 latent 截掉。
int splitIndex(const std::vector<TtsGroup> &groups,
               const std::vector<float> &durations, int total_tokens) {
  std::size_t syllable_tokens = 0;
  for (const TtsGroup &group : groups) syllable_tokens += group.ids.size();
  const double overhead =
      spanFrames(sliceFloats(durations, 0, 1)) +
      spanFrames(sliceFloats(durations, 1 + syllable_tokens,
                             static_cast<std::size_t>(total_tokens)));
  const double budget = kDecoderFrames - overhead;
  std::size_t cursor = 1;
  double frames = 0.0;
  int count = 0;
  for (const TtsGroup &group : groups) {
    const std::size_t width = group.ids.size();
    if (cursor + width > durations.size()) break;
    const double span =
        spanFrames(sliceFloats(durations, cursor, cursor + width));
    if (frames + span > budget) break;
    frames += span;
    cursor += width;
    ++count;
  }
  return count;
}

// 单个多输入 bmodel 的加载与推理（TTS 前端是 tokens + mask 两路输入，
// 不能复用只支持单输入的 bmrt_utils.hpp::Session）。
class RuntimeNet {
 public:
  ~RuntimeNet() { reset(); }

  RuntimeNet(const RuntimeNet &) = delete;
  RuntimeNet &operator=(const RuntimeNet &) = delete;
  RuntimeNet() = default;

  bool load(bm_handle_t handle, const std::string &path, std::string *error) {
    reset();
    runtime_ = tdl_app::bmrt_runtime::createRuntime(handle);
    if (!runtime_ || !bmrt_load_bmodel(runtime_, path.c_str())) {
      setError(error, "failed to load TTS bmodel: " + path);
      reset();
      return false;
    }
    const char **names = nullptr;
    if (bmrt_get_network_number(runtime_) != 1) {
      setError(error, "TTS bmodel must have exactly one network: " + path);
      reset();
      return false;
    }
    bmrt_get_network_names(runtime_, &names);
    if (!names) {
      setError(error, "TTS bmodel has no network name: " + path);
      reset();
      return false;
    }
    name_ = names[0];
    std::free(names);
    network_ = bmrt_get_network_info(runtime_, name_.c_str());
    if (!network_ || network_->is_dynamic || network_->stage_num != 1) {
      setError(error, "TTS bmodel must have one static network: " + path);
      reset();
      return false;
    }
    return true;
  }

  bool run(const std::vector<void *> &inputs,
           std::vector<std::vector<std::uint8_t>> *outputs,
           std::vector<bm_shape_t> *output_shapes) const {
    if (!network_ ||
        inputs.size() != static_cast<std::size_t>(network_->input_num)) {
      return false;
    }
    const bm_stage_info_t &stage = network_->stages[0];
    std::vector<bm_shape_t> input_shapes(
        stage.input_shapes, stage.input_shapes + network_->input_num);
    outputs->assign(network_->output_num, std::vector<std::uint8_t>());
    std::vector<void *> output_ptrs(network_->output_num);
    output_shapes->assign(network_->output_num, bm_shape_t{});
    for (int i = 0; i < network_->output_num; ++i) {
      (*outputs)[i].resize(network_->max_output_bytes[i]);
      output_ptrs[i] = (*outputs)[i].data();
    }
    return bmrt_launch_data(runtime_, name_.c_str(), inputs.data(),
                            input_shapes.data(), network_->input_num,
                            output_ptrs.data(), output_shapes->data(),
                            network_->output_num, true);
  }

  const bm_net_info_t *network() const { return network_; }

  void reset() {
    network_ = nullptr;
    name_.clear();
    if (runtime_) {
      // CV184X 的 a53lite bmodel 在卸载最后一个 kernel module 时可能抛异常，
      // 此时设备缓冲已经释放，不应该把正常退出变成 std::terminate。
      try {
        tdl_app::bmrt_runtime::destroyRuntime(runtime_);
      } catch (const std::exception &exception) {
        std::fprintf(stderr, "TTS bmrt_destroy 在退出时被忽略: %s\n",
                     exception.what());
      } catch (...) {
        std::fprintf(stderr, "TTS bmrt_destroy 在退出时被忽略: 未知异常\n");
      }
      runtime_ = nullptr;
    }
  }

 private:
  void *runtime_ = nullptr;
  const bm_net_info_t *network_ = nullptr;
  std::string name_;
};

// bmodel 输出缓冲区按 max_output_bytes 分配，可能比真实数据长，必须按
// 张量形状裁掉尾巴后再解释。
bool sliceOutput(const std::vector<std::uint8_t> &source,
                 const bm_shape_t &shape, bm_data_type_t type,
                 std::vector<std::uint8_t> *out, std::string *error) {
  std::size_t element_bytes = 0;
  if (type == BM_FLOAT32) {
    element_bytes = 4;
  } else if (type == BM_BFLOAT16) {
    element_bytes = 2;
  } else {
    setError(error, "TTS bmodel 输出不是 float32/bf16");
    return false;
  }
  const std::size_t wanted = shapeElements(shape) * element_bytes;
  if (source.size() < wanted) {
    setError(error, "TTS bmodel 输出缓冲区不足");
    return false;
  }
  out->assign(source.begin(), source.begin() + wanted);
  return true;
}

bool toFloat(const std::vector<std::uint8_t> &bytes, bm_data_type_t type,
             std::vector<float> *values, std::string *error) {
  if (type == BM_FLOAT32) {
    if (bytes.size() % 4 != 0) {
      setError(error, "TTS float32 输出长度非法");
      return false;
    }
    values->resize(bytes.size() / 4);
    if (!bytes.empty()) std::memcpy(values->data(), bytes.data(), bytes.size());
    return true;
  }
  if (type == BM_BFLOAT16) {
    if (bytes.size() % 2 != 0) {
      setError(error, "TTS bf16 输出长度非法");
      return false;
    }
    const std::size_t count = bytes.size() / 2;
    values->resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      const std::uint32_t raw =
          static_cast<std::uint32_t>(bytes[index * 2]) |
          (static_cast<std::uint32_t>(bytes[index * 2 + 1]) << 8);
      const std::uint32_t bits = raw << 16;
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      (*values)[index] = value;
    }
    return true;
  }
  setError(error, "TTS bmodel 输出不是 float32/bf16");
  return false;
}

// 读取某个输出张量并转成 float 序列。
bool readOutput(const std::vector<std::vector<std::uint8_t>> &outputs,
                const bm_net_info_t *network, int index,
                std::vector<float> *values, std::string *error) {
  if (!network || index < 0 || index >= network->output_num ||
      index >= static_cast<int>(outputs.size())) {
    setError(error, "TTS bmodel 输出索引越界");
    return false;
  }
  std::vector<std::uint8_t> slice;
  if (!sliceOutput(outputs[index], network->stages[0].output_shapes[index],
                   network->output_dtypes[index], &slice, error)) {
    return false;
  }
  return toFloat(slice, network->output_dtypes[index], values, error);
}

}  // namespace
}  // namespace tdl_app

namespace tdl_app {
namespace {

// 渲染实现（render 需要完整的 RuntimeNet 类型，因此定义放在这里）。
bool render(const RuntimeNet &frontend, const RuntimeNet &decoder,
            const std::vector<TtsGroup> &groups, std::vector<float> *waveform,
            TtsSynthesisStats *stats, std::string *error) {
  std::vector<TtsSegment> segments;
  if (!packGroups(groups, &segments, error)) return false;
  if (segments.size() != 1) {
    for (const TtsSegment &segment : segments) {
      if (!render(frontend, decoder, segment.groups, waveform, stats, error)) {
        return false;
      }
    }
    return true;
  }
  const TtsSegment &segment = segments[0];

  std::vector<std::int32_t> ids(kTokenCapacity, kPad);
  std::vector<float> mask(kTokenCapacity, 0.0f);
  for (std::size_t index = 0; index < segment.tokens.size(); ++index) {
    ids[index] = segment.tokens[index];
    mask[index] = 1.0f;
  }

  std::vector<void *> inputs = {ids.data(), mask.data()};
  std::vector<std::vector<std::uint8_t>> outputs;
  std::vector<bm_shape_t> shapes;
  if (!frontend.run(inputs, &outputs, &shapes)) {
    setError(error, "TTS 前端推理失败");
    return false;
  }

  const bm_net_info_t *frontend_net = frontend.network();
  // 前端两个输出在 bmodel 里都是 3 维（latent [1,192,192]、durations
  // [1,1,32]），所以不能按维度个数区分，改为按元素个数：与 token 容量相同
  // 的那个是 durations，剩下的（更大的那个）是 latent。
  std::vector<float> latent;
  std::vector<float> durations;
  for (int index = 0; index < frontend_net->output_num; ++index) {
    std::vector<float> values;
    if (!readOutput(outputs, frontend_net, index, &values, error)) return false;
    if (values.size() == static_cast<std::size_t>(kTokenCapacity)) {
      durations = std::move(values);
    } else if (values.size() >= latent.size()) {
      latent = std::move(values);
    }
  }
  if (latent.empty() || durations.empty()) {
    setError(error, "TTS 前端输出张量缺失");
    return false;
  }

  const int frames =
      framesOf(sliceFloats(durations, 0, segment.tokens.size()));
  int decoded_frames = frames;
  if (decoded_frames > kDecoderFrames) {
    // 只有真正的语音越过 192 帧才切分；如果越界的只是句尾静音，直接截断
    // 更自然，切分反而会插入一段多余的停顿。
    if (speechFrames(durations, segment.tokens) > kDecoderFrames) {
      const int index = splitIndex(segment.groups, durations,
                                   static_cast<int>(segment.tokens.size()));
      if (index > 0 && index < static_cast<int>(segment.groups.size())) {
        const std::vector<TtsGroup> head(segment.groups.begin(),
                                         segment.groups.begin() + index);
        const std::vector<TtsGroup> tail(segment.groups.begin() + index,
                                         segment.groups.end());
        if (!render(frontend, decoder, head, waveform, stats, error)) {
          return false;
        }
        return render(frontend, decoder, tail, waveform, stats, error);
      }
    }
    // 无法再切（或只是尾部静音超界）时，按预算截断。
    decoded_frames = kDecoderFrames;
  }
  if (decoded_frames <= 0) return true;

  std::vector<void *> decoder_inputs = {latent.data()};
  std::vector<std::vector<std::uint8_t>> pcm_bytes;
  std::vector<bm_shape_t> pcm_shapes;
  if (!decoder.run(decoder_inputs, &pcm_bytes, &pcm_shapes)) {
    setError(error, "TTS 解码推理失败");
    return false;
  }
  std::vector<float> samples;
  if (!readOutput(pcm_bytes, decoder.network(), 0, &samples, error)) {
    return false;
  }
  const std::size_t wanted =
      static_cast<std::size_t>(decoded_frames) * kHopLength;
  if (samples.size() < wanted) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "decoder 输出长度不足: 需要 %lu 样本, 实际 %lu",
                  static_cast<unsigned long>(wanted),
                  static_cast<unsigned long>(samples.size()));
    setError(error, message);
    return false;
  }
  waveform->insert(waveform->end(), samples.begin(), samples.begin() + wanted);
  stats->rendered += 1;
  stats->frames += decoded_frames;
  return true;
}

}  // namespace

// TextToSpeech 的实现体：模型加载、文本前处理、NPU 推理与卸载都在这里，
// 对外只暴露 load / run / synthesize / tokens 四个动作。
class TextToSpeech::Impl {
 public:
  ~Impl() { reset(); }

  bool load(const ModelSessionConfig &config, std::string *error) {
    reset();
    ModelDescriptor descriptor;
    if (!loadModelDescriptor(config.model_spec, &descriptor, error)) {
      return false;
    }
    const char *required[] = {"frontend_model", "decoder_model"};
    for (const char *key : required) {
      if (descriptor.extra.find(key) == descriptor.extra.end()) {
        setError(error, std::string("TTS 模型描述缺少 ") + key + ": " +
                            config.model_spec);
        return false;
      }
    }
    // CV184X 的 a53lite 模型必须先指定固件路径，否则 bmrt 会去加载
    // 默认的 libbm1688 kernel module。
    if (!config.firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.firmware.c_str(), 0);
    }
    if (!bmrt_runtime::acquireDevice(&handle_, error) ||
        !frontend_.load(handle_,
                        joinPath(descriptor.descriptor_dir,
                                 descriptor.extra["frontend_model"]),
                        error) ||
        !decoder_.load(handle_,
                       joinPath(descriptor.descriptor_dir,
                                descriptor.extra["decoder_model"]),
                       error)) {
      reset();
      return false;
    }
    loaded_ = true;
    return true;
  }

  // 合成整段文本，输出 16 kHz、单声道、有符号 16 位小端 PCM。
  bool run(const std::string &text, std::vector<std::int16_t> *pcm16le_mono,
           std::string *error) {
    if (!loaded_) {
      setError(error, "TTS 模型尚未加载，请先 load()");
      return false;
    }
    if (!pcm16le_mono) {
      setError(error, "TTS 输出缓冲区为空");
      return false;
    }
    stats_ = TtsSynthesisStats();
    std::vector<TtsGroup> groups;
    std::vector<TtsSegment> segments;
    if (!textGroups(text, &groups, error) ||
        !packGroups(groups, &segments, error)) {
      return false;
    }
    stats_.chunks = static_cast<int>(segments.size());

    std::vector<float> waveform;
    if (!render(frontend_, decoder_, groups, &waveform, &stats_, error)) {
      return false;
    }
    std::vector<std::int16_t> model_pcm;
    if (!quantizePcm16(waveform, &model_pcm, error)) return false;
    std::vector<std::int16_t> output =
        resamplePcm16(model_pcm, kModelSampleRate, kOutputSampleRate);
    if (output.empty()) {
      setError(error, "TTS 没有生成任何音频");
      return false;
    }
    stats_.samples = static_cast<long>(output.size());
    stats_.duration = static_cast<double>(output.size()) /
                      static_cast<double>(kOutputSampleRate);
    *pcm16le_mono = std::move(output);
    return true;
  }

  bool synthesize(const std::string &text, const std::string &wav_path,
                  std::string *error) {
    std::vector<std::int16_t> pcm;
    if (!run(text, &pcm, error)) return false;
    return writeWav(wav_path, pcm, kOutputSampleRate, error);
  }

  // 只做文本前处理，不需要加载模型，便于离线检查切分结果。
  bool tokens(const std::string &text, std::vector<std::vector<int>> *chunks,
              std::string *error) {
    if (!chunks) {
      setError(error, "TTS token 输出缓冲区为空");
      return false;
    }
    std::vector<TtsGroup> groups;
    std::vector<TtsSegment> segments;
    if (!textGroups(text, &groups, error) ||
        !packGroups(groups, &segments, error)) {
      return false;
    }
    chunks->clear();
    chunks->reserve(segments.size());
    for (const TtsSegment &segment : segments) {
      chunks->push_back(segment.tokens);
    }
    return true;
  }

  bool initialized() const { return loaded_; }

  const TtsSynthesisStats &stats() const { return stats_; }

  void reset() {
    loaded_ = false;
    // 先销毁各自的 runtime，再归还设备：a53lite 在设备释放后再卸载
    // kernel module 会失败。
    frontend_.reset();
    decoder_.reset();
    bmrt_runtime::releaseDevice(&handle_);
  }

 private:
  RuntimeNet frontend_;
  RuntimeNet decoder_;
  bm_handle_t handle_ = nullptr;
  bool loaded_ = false;
  TtsSynthesisStats stats_;
};

TextToSpeech::TextToSpeech() : impl_(new Impl()) {}

TextToSpeech::~TextToSpeech() { delete impl_; }

bool TextToSpeech::load(const Config &config, std::string *error) {
  config_ = config;
  return impl_->load(config, error);
}

bool TextToSpeech::load(const std::string &model_spec, std::string *error) {
  return load(Config::fromSpec(model_spec), error);
}

bool TextToSpeech::load(const std::string &model_spec,
                        const std::string &firmware, std::string *error) {
  return load(Config::fromSpec(model_spec, firmware), error);
}

bool TextToSpeech::run(const std::string &text,
                       std::vector<std::int16_t> *pcm16le_mono,
                       std::string *error) {
  return impl_->run(text, pcm16le_mono, error);
}

bool TextToSpeech::synthesize(const std::string &text,
                              const std::string &wav_path, std::string *error) {
  return impl_->synthesize(text, wav_path, error);
}

bool TextToSpeech::tokens(const std::string &text,
                          std::vector<std::vector<int>> *chunks,
                          std::string *error) {
  return impl_->tokens(text, chunks, error);
}

bool TextToSpeech::initialized() const { return impl_->initialized(); }

const TtsSynthesisStats &TextToSpeech::lastStats() const {
  return impl_->stats();
}

void TextToSpeech::reset() { impl_->reset(); }

}  // namespace tdl_app
