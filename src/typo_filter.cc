// Copyright (c) 2026 amzxyz <rcm@qq.com>. All rights reserved.
// Licensed under BSD 3-Clause License.

#include <rime/common.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/candidate.h>
#include <rime/gear/translator_commons.h>
#include <rime/service.h>

#include <fstream>
#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include <marisa.h>

#include "typo_filter.h"

namespace rime {

// 自定义影子类：完美继承注释，同时强制接管 preedit 编码显示
class TypoShadowCandidate : public ShadowCandidate {
public:
  TypoShadowCandidate(an<Candidate> item, const std::string& type, const std::string& text,
                      const std::string& comment, bool inherit_comment, const std::string& custom_preedit)
      : ShadowCandidate(item, type, text, comment, inherit_comment),
        custom_preedit_(custom_preedit) {}

  std::string preedit() const override { return custom_preedit_; }
private:
  std::string custom_preedit_;
};

// 动态排序候选流
class WeightedTypoTranslation : public Translation {
public:
  WeightedTypoTranslation(an<Translation> original, an<Translation> corrected, size_t start, size_t end,
                          int mode, bool show_preedit, const std::string& orig_preedit,
                          const std::string& raw_seg, const std::string& delimiters,
                          const std::string& hint)
      : original_(original), corrected_(corrected), start_(start), end_(end), mode_(mode),
        show_preedit_(show_preedit), orig_preedit_(orig_preedit), raw_seg_(raw_seg), delimiters_(delimiters),
        hint_(hint) {
    if (mode_ == 2) {
      state_ = State::kExclusive;
      original_ = nullptr;
    } else if (mode_ == 1) {
      state_ = State::kCorrectedFirst;
    } else {
      state_ = State::kOriginalFirst;
    }
    EvaluateState();
  }

  bool Next() override {
    if (exhausted()) return false;
    switch (state_) {
      case State::kExclusive: corrected_->Next(); break;
      case State::kOriginalFirst: original_->Next(); state_ = State::kOriginalFirst_YieldCorrected; break;
      case State::kOriginalFirst_YieldCorrected: state_ = State::kYieldOriginalRemaining; break;
      case State::kCorrectedFirst: state_ = State::kCorrectedFirst_YieldOriginal; break;
      case State::kCorrectedFirst_YieldOriginal: original_->Next(); state_ = State::kYieldOriginalRemaining; break;
      case State::kYieldOriginalRemaining: original_->Next(); break;
      case State::kExhausted: return false;
    }
    EvaluateState();
    return !exhausted();
  }

  an<Candidate> Peek() override {
    if (exhausted()) return nullptr;
    switch (state_) {
      case State::kExclusive: return CreateShadow(corrected_->Peek(), true);
      case State::kOriginalFirst:
      case State::kCorrectedFirst_YieldOriginal:
      case State::kYieldOriginalRemaining: return original_->Peek();
      case State::kOriginalFirst_YieldCorrected:
      case State::kCorrectedFirst: return CreateShadow(corrected_->Peek(), false);
      case State::kExhausted: return nullptr;
    }
    return nullptr;
  }

private:
  enum class State {
    kExclusive, kOriginalFirst, kOriginalFirst_YieldCorrected,
    kCorrectedFirst, kCorrectedFirst_YieldOriginal, kYieldOriginalRemaining, kExhausted
  };

  void EvaluateState() {
    while (state_ != State::kExhausted) {
      switch (state_) {
        case State::kExclusive:
          if (corrected_ && !corrected_->exhausted()) { set_exhausted(false); return; }
          state_ = State::kExhausted; break;
        case State::kOriginalFirst:
          if (original_ && !original_->exhausted()) { set_exhausted(false); return; }
          state_ = State::kOriginalFirst_YieldCorrected; break;
        case State::kOriginalFirst_YieldCorrected:
          if (corrected_ && !corrected_->exhausted()) { set_exhausted(false); return; }
          state_ = State::kYieldOriginalRemaining; break;
        case State::kCorrectedFirst:
          if (corrected_ && !corrected_->exhausted()) { set_exhausted(false); return; }
          state_ = State::kCorrectedFirst_YieldOriginal; break;
        case State::kCorrectedFirst_YieldOriginal:
          if (original_ && !original_->exhausted()) { set_exhausted(false); return; }
          state_ = State::kExhausted; break;
        case State::kYieldOriginalRemaining:
          if (original_ && !original_->exhausted()) { set_exhausted(false); return; }
          state_ = State::kExhausted; break;
        case State::kExhausted: set_exhausted(true); return;
      }
    }
    set_exhausted(true);
  }

  // 构建候选词并注入视觉提示
  an<Candidate> CreateShadow(an<Candidate> c, bool is_exclusive) {
    if (!c) return nullptr;
    std::string final_preedit = c->preedit();

    // 如果设置为 false，算回原始编码
    if (!show_preedit_) {
      if (is_exclusive) {
        size_t non_delim_count = 0;
        for (char ch : final_preedit) {
          if (delimiters_.find(ch) == std::string::npos) non_delim_count++;
        }

        size_t raw_non_delim_count = 0;
        for (char ch : raw_seg_) {
          if (delimiters_.find(ch) == std::string::npos) raw_non_delim_count++;
        }

        if (non_delim_count == raw_non_delim_count) {
          std::string mapped = "";
          size_t idx = 0;
          for (char ch : final_preedit) {
            if (delimiters_.find(ch) != std::string::npos) {
              mapped += ch;
            } else {
              while (idx < raw_seg_.length() && delimiters_.find(raw_seg_[idx]) != std::string::npos) {
                mapped += raw_seg_[idx++];
              }
              if (idx < raw_seg_.length()) mapped += raw_seg_[idx++];
            }
          }
          while (idx < raw_seg_.length() && delimiters_.find(raw_seg_[idx]) != std::string::npos) {
            mapped += raw_seg_[idx++];
          }
          final_preedit = mapped;
        } else {
          final_preedit = raw_seg_;
        }
      } else {
        final_preedit = orig_preedit_;
      }

      an<Candidate> genuine = Candidate::GetGenuineCandidate(c);
      if (auto phrase = As<Phrase>(genuine)) {
        phrase->set_preedit(final_preedit);
      } else if (auto simple = As<SimpleCandidate>(genuine)) {
        simple->set_preedit(final_preedit);
      }
    }

    if (is_exclusive) {
      auto cand = New<TypoShadowCandidate>(c, c->type(), c->text(), c->comment(), true, final_preedit);
      cand->set_quality(c->quality());
      return cand;
    } else {
      auto cand = New<TypoShadowCandidate>(c, "typo", c->text(), hint_, false, final_preedit);
      cand->set_quality(c->quality());
      return cand;
    }
  }

  an<Translation> original_, corrected_;
  size_t start_, end_;
  int mode_;
  bool show_preedit_;
  std::string orig_preedit_, raw_seg_, delimiters_, hint_;
  State state_;
};

static bool IsTxtNewerThanBin(const std::string& txt_path, const std::string& bin_path) {
  struct stat txt_stat, bin_stat;
  if (stat(bin_path.c_str(), &bin_stat) != 0) return true;
  if (stat(txt_path.c_str(), &txt_stat) != 0) return false;
  return txt_stat.st_mtime > bin_stat.st_mtime;
}

TypoFilter::TypoFilter(const Ticket& ticket) : Filter(ticket) {
  if (!ticket.engine || !ticket.schema || !ticket.schema->config()) return;
  Config* config = ticket.schema->config();
  config->GetBool("typo/show_corrected_preedit", &show_corrected_preedit_);
  config->GetString("typo/correction_hint", &correction_hint_);
  std::string translator_ns = "translator";
  config->GetString("typo/translator", &translator_ns);

  Ticket t(ticket.engine, translator_ns);
  auto* comp = Translator::Require("script_translator");
  if (comp) translator_.reset(comp->Create(t));
  if (!translator_) return;

  std::string input_type = "", custom_file = "";
  config->GetString("typo/input_type", &input_type);
  config->GetString("typo/custom_file", &custom_file);

  if (input_type == "auto") input_type = DetectInputType(config);
  if (input_type.empty() && custom_file.empty()) return;

  is_enabled_ = true;
  LoadCorrections(ticket.engine, input_type, custom_file);
  config->GetInt("typo/max_scan_len", &max_scan_len_);
}

std::string TypoFilter::DetectInputType(Config* config) const {
  std::unordered_map<std::string, std::string> markers;
  an<ConfigMap> marker_map = config->GetMap("typo/markers");
  if (marker_map) {
    for (auto it = marker_map->begin(); it != marker_map->end(); ++it) {
      auto val = As<ConfigValue>(it->second);
      if (val) markers[it->first] = val->str();
    }
  }
  if (markers.empty()) return "";

  an<ConfigList> algebra = config->GetList("speller/algebra");
  if (!algebra) return "";

  for (size_t i = 0; i < algebra->size(); ++i) {
    auto val = As<ConfigValue>(algebra->GetAt(i));
    if (!val) continue;
    std::string rule = val->str();
    for (const auto& pair : markers) {
      if (rule.find(pair.first) != std::string::npos) {
          LOG(INFO) << "TypoFilter: Auto-detected input type: " << pair.second << " via marker: " << pair.first;
          return pair.second;
      }
    }
  }
  return "";
}

void TypoFilter::LoadCorrections(Engine* engine, const std::string& input_type, const std::string& custom_file) {
  std::string base_name = custom_file.empty() ? ("typo_" + input_type) : custom_file;

  if (current_key_ == base_name) return;
  current_key_ = base_name;
  trie_loaded_ = false;
  trie_.clear();

  const auto& deployer = Service::instance().deployer();
  std::string user_dir = deployer.user_data_dir.string();
  std::string shared_dir = deployer.shared_data_dir.string();

  std::string user_txt = user_dir + "/" + base_name + ".txt";
  std::string user_bin = user_dir + "/build/" + base_name + ".bin";
  std::string shared_txt = shared_dir + "/" + base_name + ".txt";
  std::string shared_bin = shared_dir + "/build/" + base_name + ".bin";

  auto jit_compile =
      [&](const std::string& txt_path, const std::string& bin_path,
          const std::string& label) {
        LOG(INFO) << "TypoFilter: JIT Compiling " << label
                  << " dictionary: " << txt_path;
        std::ifstream file(txt_path);
        if (!file.is_open()) return;
        marisa::Keyset keyset;
        std::string line;
        while (std::getline(file, line)) {
          if (line.empty() || line[0] == '#') continue;
          size_t tab_pos = line.find('\t');
          if (tab_pos != std::string::npos) {
            std::string typo = line.substr(0, tab_pos);
            std::string corrected = line.substr(tab_pos + 1);
            if (!corrected.empty() && corrected.back() == '\r')
              corrected.pop_back();
            if (!typo.empty() && typo.back() == '\r') typo.pop_back();
            if (!typo.empty() && !corrected.empty()) {
              std::string merged_key = typo + "\t" + corrected;
              keyset.push_back(merged_key.c_str(), merged_key.length());
            }
          }
        }
        file.close();

        if (keyset.num_keys() == 0) return;

        std::string build_dir =
            bin_path.substr(0, bin_path.find_last_of("/\\"));
        struct stat st;
        if (stat(build_dir.c_str(), &st) == -1) {
#ifdef _WIN32
          _mkdir(build_dir.c_str());
#else
          mkdir(build_dir.c_str(), 0755);
#endif
        }
        marisa::Trie new_trie;
        new_trie.build(keyset);
        new_trie.save(bin_path.c_str());
        LOG(INFO) << "TypoFilter: Compiled successfully to: " << bin_path;
      };

  // user_txt > shared_txt → user_bin
  if (IsTxtNewerThanBin(user_txt, user_bin)) {
    jit_compile(user_txt, user_bin, "user");
  } else if (IsTxtNewerThanBin(shared_txt, user_bin)) {
    jit_compile(shared_txt, user_bin, "shared");
  }

  // common_txt → common_bin
  std::string common_dir = deployer.common_data_dir.string();
  if (!common_dir.empty()) {
    std::string common_txt = common_dir + "/typo/" + base_name + ".txt";
    std::string common_bin = common_dir + "/typo/" + base_name + ".bin";
    if (IsTxtNewerThanBin(common_txt, common_bin)) {
      jit_compile(common_txt, common_bin, "common");
    }
  }

  // load: user_bin → shared_bin → common_bin
  try {
    trie_.mmap(user_bin.c_str());
    trie_loaded_ = true;
    LOG(INFO) << "TypoFilter: Loaded user binary: " << user_bin;
  } catch (...) {
    try {
      trie_.mmap(shared_bin.c_str());
      trie_loaded_ = true;
      LOG(INFO) << "TypoFilter: Loaded shared binary: " << shared_bin;
    } catch (...) {
      if (!common_dir.empty()) {
        try {
          std::string common_bin = common_dir + "/typo/" + base_name + ".bin";
          trie_.mmap(common_bin.c_str());
          trie_loaded_ = true;
          LOG(INFO) << "TypoFilter: Loaded common binary: " << common_bin;
        } catch (...) {
          LOG(WARNING) << "TypoFilter: Failed to map binary for: " << base_name;
        }
      } else {
        LOG(WARNING) << "TypoFilter: Failed to map binary for: " << base_name;
      }
    }
  }
}

// 局部查询引擎（带汉字词提取，用于整句印证）
std::string TypoFilter::GetCorrectedInput(const std::string& input, int& correction_count, size_t& max_correction_len,
                                          const std::string& segment_tag, bool is_pinyin, std::string& out_local_text,
                                          const std::string& delimiters) const {
  correction_count = 0;
  max_correction_len = 0;
  out_local_text = "";
  if (!trie_loaded_ || input.empty() || !translator_) return "";
  if (segment_tag.empty()) return "";

  std::string corrected = "";
  bool has_correction = false;

  for (char c : input) {
    corrected += c;
    size_t tail_len = corrected.length();

    std::string clean_corrected = "";
    for (char ch : corrected) {
      if (delimiters.find(ch) == std::string::npos) clean_corrected += ch;
    }
    if (!is_pinyin && clean_corrected.length() % 2 != 0) continue;

    for (size_t scan_len = std::min(tail_len, static_cast<size_t>(max_scan_len_)); scan_len >= 1; --scan_len) {
      std::string scan_input = corrected.substr(tail_len - scan_len);

      // 剥离手敲的分隔符（如 '），得到纯字母用于查库
      std::string clean_scan_input = "";
      for (char ch : scan_input) {
        if (delimiters.find(ch) == std::string::npos) clean_scan_input += ch;
      }
      if (!is_pinyin && clean_scan_input.length() % 2 != 0) continue;
      if (clean_scan_input.empty()) continue;

      std::string query_prefix = clean_scan_input + "\t"; // 拿纯字母去查树
      marisa::Agent agent;
      agent.set_query(query_prefix.c_str());

      if (trie_.predictive_search(agent)) {
        std::string found_key(agent.key().ptr(), agent.key().length());
        size_t tab_pos = found_key.find('\t');
        if (tab_pos != std::string::npos) {
          std::string target_corrected = found_key.substr(tab_pos + 1);
          bool valid = true;

          if (!is_pinyin) {
            auto cache_it = query_cache_.find(target_corrected);
            if (cache_it != query_cache_.end()) {
              valid = cache_it->second.first;
              out_local_text = cache_it->second.second;
            } else {
              valid = false;
              Segment seg_corr(0, target_corrected.length());
              seg_corr.tags.insert(segment_tag);
              auto trans_corr = translator_->Query(target_corrected, seg_corr);
              if (trans_corr && !trans_corr->exhausted()) {
                if (auto c = trans_corr->Peek()) {
                  std::string type_corr = c->type();
                  if (type_corr == "phrase" || type_corr == "user_phrase") {
                    valid = true;
                    out_local_text = c->text();
                  }
                }
              }
              query_cache_[target_corrected] = {valid, out_local_text};
            }
          }

          if (valid) {
            // 跨越替换：把包含错误分隔符的尾段一起吃掉，换成干净的正确编码
            corrected = corrected.substr(0, tail_len - scan_len) + target_corrected;
            has_correction = true;
            correction_count++;
            if (scan_len > max_correction_len) max_correction_len = scan_len;
            break;
          }
        }
      }
    }
  }
  return has_correction ? corrected : "";
}

// 全局决策引擎
an<Translation> TypoFilter::Apply(an<Translation> translation, CandidateList* candidates) {
  if (!is_enabled_ || !engine_ || !translation || !translator_) return translation;

  Context* ctx = engine_->context();
  if (!ctx || !ctx->get_option("corrector")) return translation;

  std::string raw_input = ctx->input();
  if (raw_input.empty()) return translation;

  std::string delimiters = " '";
  an<ConfigValue> delim_conf = engine_->schema()->config()->GetValue("speller/delimiter");
  if (delim_conf && !delim_conf->str().empty()) {
    delimiters = delim_conf->str();
  }

  std::string itype;
  engine_->schema()->config()->GetString("typo/input_type", &itype);
  if (itype == "auto") itype = DetectInputType(engine_->schema()->config());

  bool is_pinyin = (itype == "pinyin");

  std::string segment_tag = "";
  if (!ctx->composition().empty()) {
    const auto& tags = ctx->composition().back().tags;
    if (!tags.empty()) segment_tag = *tags.begin();
  }
  if (segment_tag.empty()) {
    an<ConfigList> custom_tags = engine_->schema()->config()->GetList("typo/tags");
    if (custom_tags && custom_tags->size() > 0) {
      auto val = As<ConfigValue>(custom_tags->GetAt(0));
      if (val) segment_tag = val->str();
    }
    if (segment_tag.empty()) segment_tag = "abc";
  }

  int correction_count = 0;
  size_t max_correction_len = 0;
  std::string local_corr_text = ""; // 接收局部提取的汉字

  std::string corrected_input = GetCorrectedInput(raw_input, correction_count, max_correction_len,
                                                  segment_tag, is_pinyin, local_corr_text, delimiters);

  if (corrected_input.empty() || corrected_input == raw_input) return translation;

  bool original_failed = translation->exhausted();

  size_t start = 0;
  size_t end = raw_input.length();
  if (!ctx->composition().empty()) {
    start = ctx->composition().back().start;
    end = ctx->composition().back().end;
  }
  bool is_partial_match = (end < raw_input.length());

  Segment seg(0, corrected_input.length());
  if (!ctx->composition().empty()) seg.tags = ctx->composition().back().tags;
  if (seg.tags.empty()) return translation;

  an<Translation> corrected_translation = translator_->Query(corrected_input, seg);
  if (!corrected_translation || (corrected_translation->exhausted() && !original_failed)) {
    return translation;
  }

  std::string orig_type = "";
  double orig_quality = 0.0;
  if (!original_failed) {
    if (auto orig_c = translation->Peek()) {
      orig_type = orig_c->type();
      orig_quality = orig_c->quality();
    }
  }

  std::string corr_type = "";
  double corr_quality = 0.0;
  bool is_single_syllable = false;
  std::string global_corr_text = ""; // 接收全局翻译结果

  if (auto c = corrected_translation->Peek()) {
    corr_type = c->type();
    corr_quality = c->quality();
    global_corr_text = c->text();

    std::string text = c->text();
    size_t utf8_char_count = 0;
    for (char ch : text) if ((ch & 0xC0) != 0x80) utf8_char_count++;
    if (utf8_char_count == 1) is_single_syllable = true;
  }

  int override_mode = 0;

  if (original_failed || is_partial_match) {
    override_mode = 2; // 原版废码，无条件纠错独占救场
  }
  else if (is_pinyin) {
    // 全拼 (Pinyin) 核心规则
    bool corr_is_word = (corr_type == "phrase" || corr_type == "user_phrase");

    if (raw_input.length() <= 3) {
      override_mode = 0; // 3码以下纠错必次选
    } else {
      if (orig_type == "sentence" && corr_is_word) {
        override_mode = 2; // 破句子被真词碾压，独占
      }
      else if (raw_input.length() > 4 && corr_quality > orig_quality) {
        override_mode = 1; // 纠错得分高，抢占第一
      }
      else if (is_single_syllable && max_correction_len >= 4) {
        override_mode = 2; // 单音节精准越权
      }
      else if (correction_count >= 2) {
        override_mode = 2; // 连环错越权
      }
      else {
        override_mode = 0;
      }
    }
  }
  else {
    // 双拼 (真值表决战与动态生命周期印证)
    bool orig_is_word = (orig_type == "phrase" || orig_type == "user_phrase");
    bool corr_is_word = (corr_type == "phrase" || corr_type == "user_phrase");

    // 局部词必须在当前长度生成的全局整句中成功对齐:加一个逻辑,加一个立即，gelo>geli但没有最终用到句子中
    // 只要查找不到，说明发生了跨音节拆分导致的语义坍塌，立即执行抑制，退回原版首选
    // 当用户按 Backspace 回删，只要再次满足对齐，该纠错词会瞬间在生命周期中被再次激活。
    if (!local_corr_text.empty() && !global_corr_text.empty()) {
      if (global_corr_text.find(local_corr_text) == std::string::npos) {
        return translation;
      }
    }

    if (correction_count >= 2) {
      override_mode = 2; // 发生两次以上的纠错连招，直接踢掉原版
    }
    else if (orig_is_word && !corr_is_word) {
      return translation; // 原版是词，纠错变句，属于误杀
    }
    else if (!orig_is_word && corr_is_word) {
      override_mode = 2; // 原版是句，纠错真词，碾压
    }
    else if (orig_is_word && corr_is_word) {
      override_mode = 0; // 如果大家都是词，尊重原版第一，纠错屈居次选
    }
    else {
      // 如果都是拼凑出的长句（模型作用下质量通常同为 0.0），平局原版赢
      if (corr_quality > orig_quality) {
        override_mode = 1;
      } else {
        override_mode = 0;
      }
    }
  }

  std::string raw_segment = (end > start && end <= raw_input.length()) ?
                            raw_input.substr(start, end - start) : raw_input;
  std::string original_preedit = "";

  if (override_mode != 2) {
    if (!original_failed) {
      if (auto orig_c = translation->Peek()) original_preedit = orig_c->preedit();
    }
    if (original_preedit.empty()) original_preedit = raw_segment;
  }

  return New<WeightedTypoTranslation>(translation, corrected_translation, start, raw_input.length(),
                                      override_mode, show_corrected_preedit_, original_preedit,
                                      raw_segment, delimiters, correction_hint_);
}

}  // namespace rime