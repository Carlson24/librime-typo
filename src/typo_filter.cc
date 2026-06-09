#include "typo_filter.h"
#include <rime/engine.h>
#include <rime/context.h>
#include <rime/schema.h>
#include <rime/candidate.h>
#include <rime/service.h>
#include <rime/common.h>
#include <fstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <marisa.h>

namespace rime {

class WeightedTypoTranslation : public Translation {
public:
  WeightedTypoTranslation(an<Translation> original, an<Translation> corrected, size_t start, size_t end, int mode, bool show_preedit, const std::string& orig_preedit, const std::string& raw_seg)
      : original_(original), corrected_(corrected), start_(start), end_(end), mode_(mode), show_preedit_(show_preedit), orig_preedit_(orig_preedit), raw_seg_(raw_seg) {
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

  an<Candidate> CreateShadow(an<Candidate> c, bool is_exclusive) {
    if (!c) return nullptr;
    std::string final_preedit = c->preedit();
    if (!show_preedit_) {
      if (is_exclusive) {
        size_t non_space_count = 0;
        for (char ch : final_preedit) if (ch != ' ') non_space_count++;
        if (non_space_count == raw_seg_.length()) {
          std::string mapped = "";
          size_t idx = 0;
          for (char ch : final_preedit) {
            if (ch == ' ') mapped += ' '; else mapped += raw_seg_[idx++];
          }
          final_preedit = mapped;
        } else final_preedit = raw_seg_;
      } else {
        final_preedit = orig_preedit_;
      }
    }
    auto cand = New<SimpleCandidate>("shadow", start_, end_, c->text(), c->comment(), final_preedit);
    cand->set_quality(c->quality());
    return cand;
  }

  an<Translation> original_, corrected_;
  size_t start_, end_;
  int mode_;
  bool show_preedit_;
  std::string orig_preedit_, raw_seg_;
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

  std::string user_dir = Service::instance().deployer().user_data_dir.string();
  std::string shared_dir = Service::instance().deployer().shared_data_dir.string();

  std::string user_txt = user_dir + "/" + base_name + ".txt";
  std::string user_bin = user_dir + "/build/" + base_name + ".bin";
  
  std::string shared_bin = shared_dir + "/build/typo_" + input_type + ".bin";

  if (IsTxtNewerThanBin(user_txt, user_bin)) {
    LOG(INFO) << "TypoFilter: JIT Compiling user dictionary: " << user_txt;
    std::ifstream file(user_txt);
    if (file.is_open()) {
      marisa::Keyset keyset;
      std::string line;
      while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos != std::string::npos) {
          std::string typo = line.substr(0, tab_pos);
          std::string corrected = line.substr(tab_pos + 1);
          if (!corrected.empty() && corrected.back() == '\r') corrected.pop_back();
          if (!typo.empty() && typo.back() == '\r') typo.pop_back();
          if (!typo.empty() && !corrected.empty()) {
            std::string merged_key = typo + "\t" + corrected;
            keyset.push_back(merged_key.c_str(), merged_key.length());
          }
        }
      }
      file.close();

      if (keyset.num_keys() > 0) {
        std::string build_dir = user_dir + "/build";
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
        new_trie.save(user_bin.c_str()); 
        LOG(INFO) << "TypoFilter: Compiled successfully to: " << user_bin;
      }
    }
  }

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
      LOG(WARNING) << "TypoFilter: Failed to map binary for: " << base_name;
    }
  }
}

std::string TypoFilter::GetCorrectedInput(const std::string& input, int& correction_count, size_t& max_correction_len, const std::string& segment_tag, bool is_pinyin) const {
  correction_count = 0;
  max_correction_len = 0;
  if (!trie_loaded_ || input.empty() || !translator_) return "";
  if (segment_tag.empty()) return "";

  std::string corrected = "";
  bool has_correction = false;
  const size_t MAX_SCAN_LEN = 20;

  for (char c : input) {
    corrected += c;
    size_t tail_len = corrected.length();
    
    if (!is_pinyin && tail_len % 2 != 0) {
        continue;
    }

    for (size_t scan_len = std::min(tail_len, MAX_SCAN_LEN); scan_len >= 1; --scan_len) {
      if (!is_pinyin && scan_len % 2 != 0) continue;

      std::string scan_input = corrected.substr(tail_len - scan_len);
      std::string query_prefix = scan_input + "\t";

      marisa::Agent agent;
      agent.set_query(query_prefix.c_str());

      if (trie_.predictive_search(agent)) {
        std::string found_key(agent.key().ptr(), agent.key().length());
        size_t tab_pos = found_key.find('\t');
        if (tab_pos != std::string::npos) {
          std::string target_corrected = found_key.substr(tab_pos + 1);
          bool valid = true;

          if (!is_pinyin) {
            valid = false;
            Segment seg_corr(0, target_corrected.length());
            seg_corr.tags.insert(segment_tag);
            auto trans_corr = translator_->Query(target_corrected, seg_corr);
            std::string type_corr = "";
            if (trans_corr && !trans_corr->exhausted()) {
              if (auto c = trans_corr->Peek()) type_corr = c->type();
            }

            if (type_corr == "phrase" || type_corr == "user_phrase") {
              valid = true;
            }
          }

          if (valid) {
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

an<Translation> TypoFilter::Apply(an<Translation> translation, CandidateList* candidates) {
  if (!is_enabled_ || !engine_ || !translation || !translator_) return translation;

  Context* ctx = engine_->context();
  if (!ctx || !ctx->get_option("corrector")) return translation;

  std::string raw_input = ctx->input();
  if (raw_input.empty()) return translation;

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
  std::string corrected_input = GetCorrectedInput(raw_input, correction_count, max_correction_len, segment_tag, is_pinyin);

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
  if (!corrected_translation || (corrected_translation->exhausted() && !original_failed)) return translation;

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
  if (auto c = corrected_translation->Peek()) {
    corr_type = c->type();
    corr_quality = c->quality();
    std::string text = c->text();
    size_t utf8_char_count = 0;
    for (char ch : text) if ((ch & 0xC0) != 0x80) utf8_char_count++;
    if (utf8_char_count == 1) is_single_syllable = true;
  }

  int override_mode = 0; 
  
  if (original_failed || is_partial_match) {
    override_mode = 2;
  }
  else if (is_pinyin) {
    bool corr_is_word = (corr_type == "phrase" || corr_type == "user_phrase");

    if (raw_input.length() <= 3) {
      override_mode = 0;
    } else {
      if (orig_type == "sentence" && corr_is_word) {
        override_mode = 2;
      } 
      else if (raw_input.length() > 4 && corr_quality > orig_quality) {
        override_mode = 1;
      } 
      else if (is_single_syllable && max_correction_len >= 4) {
        override_mode = 2;
      } 
      else if (correction_count >= 2) {
        override_mode = 2;
      } 
      else {
        override_mode = 0; 
      }
    }
  } 
  else {
    bool orig_is_word = (orig_type == "phrase" || orig_type == "user_phrase");
    bool corr_is_word = (corr_type == "phrase" || corr_type == "user_phrase");

    if (correction_count >= 2) {
      override_mode = 2;
    } 
    else if (orig_is_word && !corr_is_word) {
      return translation;
    } 
    else if (!orig_is_word && corr_is_word) {
      override_mode = 2;
    } 
    else {
      if (corr_quality >= orig_quality) {
        override_mode = 1;
      } else {
        override_mode = 0;
      }
    }
  }

  std::string raw_segment = (end > start && end <= raw_input.length()) ? raw_input.substr(start, end - start) : raw_input;
  std::string original_preedit = "";

  if (override_mode != 2) {
    if (!original_failed) {
      if (auto orig_c = translation->Peek()) original_preedit = orig_c->preedit();
    }
    if (original_preedit.empty()) original_preedit = raw_segment;
  }

  return New<WeightedTypoTranslation>(translation, corrected_translation, start, raw_input.length(), override_mode, show_corrected_preedit_, original_preedit, raw_segment);
}

TypoTranslation::TypoTranslation(an<Translation> original, an<Translation> corrected, size_t start, size_t end, bool exclusive_override, bool show_corrected_preedit, const std::string& original_preedit, const std::string& raw_segment)
    : original_(original), corrected_(corrected), start_(start), end_(end), original_preedit_(original_preedit), raw_segment_(raw_segment), show_corrected_preedit_(show_corrected_preedit) {}
bool TypoTranslation::Next() { return false; }
an<Candidate> TypoTranslation::Peek() { return nullptr; }
void TypoTranslation::EvaluateState() {}

}  // namespace rime