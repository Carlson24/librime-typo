#include "typo_filter.h"
#include <rime/engine.h>
#include <rime/context.h>
#include <rime/schema.h>
#include <rime/candidate.h>
#include <rime/service.h>
#include <fstream>
#include <algorithm>

namespace rime {

static void WriteDebugLog(const std::string& msg) {
  std::string log_path =
      Service::instance().deployer().user_data_dir.string() + "/typo_debug.log";
  std::ofstream out(log_path, std::ios::app);
  if (out.is_open()) {
    out << "[Typo Debug] " << msg << std::endl;
  }
}

TypoFilter::TypoFilter(const Ticket& ticket) : Filter(ticket) {
  if (!ticket.engine || !ticket.schema || !ticket.schema->config()) return;

  Config* config = ticket.schema->config();

  // 读取配置，默认不直接显示纠正后的完美拼音，而是采用高阶替换
  config->GetBool("typo/show_corrected_preedit", &show_corrected_preedit_);

  std::string translator_ns = "translator";
  config->GetString("typo/translator", &translator_ns);

  Ticket t(ticket.engine, translator_ns);
  auto* comp = Translator::Require("script_translator");
  if (comp) translator_.reset(comp->Create(t));

  if (!translator_) return;

  std::string input_type = "";
  std::string custom_file = "";
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
      if (rule.find(pair.first) != std::string::npos) return pair.second;
    }
  }
  return "";
}

void TypoFilter::LoadCorrections(Engine* engine, const std::string& input_type, const std::string& custom_file) {
  std::string target_key = custom_file.empty() ? input_type : ("custom_" + custom_file);
  if (current_key_ == target_key) return;

  correction_map_.clear();
  min_depth_ = 0;
  max_depth_ = 0;
  current_key_ = target_key;

  std::string file_path = !custom_file.empty() 
      ? Service::instance().deployer().user_data_dir.string() + "/typo_" + custom_file + ".txt"
      : Service::instance().deployer().shared_data_dir.string() + "/typo/typo_" + input_type + ".txt";

  std::ifstream file(file_path);
  if (!file.is_open()) {
    if (!custom_file.empty()) {
      file_path = Service::instance().deployer().shared_data_dir.string() + "/typo/typo_" + custom_file + ".txt";
      file.open(file_path);
    }
    if (!file.is_open()) return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    size_t tab_pos = line.find('\t');
    if (tab_pos != std::string::npos) {
      std::string corrected = line.substr(0, tab_pos);
      std::string typo = line.substr(tab_pos + 1);
      if (!typo.empty() && typo.back() == '\r') typo.pop_back();
      if (!typo.empty() && !corrected.empty()) {
        size_t typo_len = typo.length();
        if (min_depth_ == 0 || typo_len < min_depth_) min_depth_ = typo_len;
        if (typo_len > max_depth_) max_depth_ = typo_len;
        correction_map_[typo] = corrected;
      }
    }
  }
  file.close();
}

std::string TypoFilter::GetCorrectedInput(const std::string& input, int& correction_count, size_t& max_correction_len) const {
  correction_count = 0;
  max_correction_len = 0;
  if (min_depth_ == 0 || input.length() < min_depth_) return "";

  std::string corrected = "";
  bool has_correction = false;

  for (char c : input) {
    corrected += c;
    size_t tail_len = corrected.length();
    for (size_t scan_len = std::min(tail_len, max_depth_); scan_len >= min_depth_; --scan_len) {
      std::string scan_input = corrected.substr(tail_len - scan_len);
      auto it = correction_map_.find(scan_input);
      if (it != correction_map_.end()) {
        corrected = corrected.substr(0, tail_len - scan_len) + it->second;
        has_correction = true;
        correction_count++;
        if (scan_len > max_correction_len) max_correction_len = scan_len;
        break;
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

  int correction_count = 0;
  size_t max_correction_len = 0;
  std::string corrected_input = GetCorrectedInput(raw_input, correction_count, max_correction_len);
  if (corrected_input.empty()) return translation;

  Segment seg(0, corrected_input.length());
  an<ConfigList> custom_tags = engine_->schema()->config()->GetList("typo/tags");
  if (custom_tags && custom_tags->size() > 0) {
    for (size_t i = 0; i < custom_tags->size(); ++i) {
      auto val = As<ConfigValue>(custom_tags->GetAt(i));
      if (val) seg.tags.insert(val->str());
    }
  } else if (!ctx->composition().empty()) {
    seg.tags = ctx->composition().back().tags;
  }

  if (seg.tags.empty()) return translation;

  an<Translation> corrected_translation = translator_->Query(corrected_input, seg);
  if (!corrected_translation) return translation;

  bool exclusive_override = (max_correction_len >= 4 || correction_count >= 2);

  size_t start = 0;
  size_t end = raw_input.length();
  if (!ctx->composition().empty()) {
    start = ctx->composition().back().start;
    end = ctx->composition().back().end;
  }

  std::string raw_segment = (end > start && end <= raw_input.length()) ? raw_input.substr(start, end - start) : raw_input;
  std::string original_preedit = "";

  if (!exclusive_override) {
    if (translation && !translation->exhausted()) {
      if (auto orig_c = translation->Peek()) {
        original_preedit = orig_c->preedit();
      }
    }
    if (original_preedit.empty()) {
      original_preedit = raw_segment;
    }
  }

  return New<TypoTranslation>(translation, corrected_translation, start, end, exclusive_override, show_corrected_preedit_, original_preedit, raw_segment);
}

// ==========================================
// 专业级复合翻译流：TypoTranslation
// ==========================================

TypoTranslation::TypoTranslation(an<Translation> original, 
                                 an<Translation> corrected, 
                                 size_t start, 
                                 size_t end,
                                 bool exclusive_override,
                                 bool show_corrected_preedit,
                                 const std::string& original_preedit,
                                 const std::string& raw_segment)
    : original_(original), 
      corrected_(corrected), 
      start_(start), 
      end_(end),
      original_preedit_(original_preedit),
      raw_segment_(raw_segment),
      show_corrected_preedit_(show_corrected_preedit) {
      
  if (exclusive_override) {
    state_ = State::kExclusiveOverride;
    original_ = nullptr; 
  } else {
    state_ = State::kYieldOriginalFirst;
  }
  EvaluateState();
}

bool TypoTranslation::Next() {
  if (exhausted()) return false;

  switch (state_) {
    case State::kExclusiveOverride:
      corrected_->Next();
      break;
    case State::kYieldOriginalFirst:
      original_->Next();
      state_ = State::kYieldCorrectedShadow;
      break;
    case State::kYieldCorrectedShadow:
      state_ = State::kYieldOriginalRemaining;
      break;
    case State::kYieldOriginalRemaining:
      original_->Next();
      break;
    case State::kExhausted:
      return false;
  }
  
  EvaluateState();
  return !exhausted();
}

an<Candidate> TypoTranslation::Peek() {
  if (exhausted()) return nullptr;

  switch (state_) {
    case State::kExclusiveOverride: {
      auto c = corrected_->Peek();
      if (!c) return nullptr;

      std::string final_preedit = c->preedit();

      // 🎯 魔法降临：等价位置映射算法 (Space Mapping)
      // 如果要求保留错误拼写，但依然想要纠错引擎的完美音节切分
      if (!show_corrected_preedit_) {
        size_t non_space_count = 0;
        for (char ch : final_preedit) {
          if (ch != ' ') non_space_count++;
        }
        
        // 只有当长度严格匹配时（非缺字漏字导致的纠错），才进行等价位置替换
        if (non_space_count == raw_segment_.length()) {
          std::string mapped_preedit = "";
          size_t idx = 0;
          for (char ch : final_preedit) {
            if (ch == ' ') {
              mapped_preedit += ' ';
            } else {
              mapped_preedit += raw_segment_[idx++];
            }
          }
          final_preedit = mapped_preedit; // 完美映射：dign xai lai
        } else {
          // 长度不匹配（如 lne -> leng）则安全回退，直接抛出无空格的生肉串
          final_preedit = raw_segment_;
        }
      }

      auto cand = New<SimpleCandidate>("shadow", start_, end_, c->text(), c->comment(), final_preedit);
      cand->set_quality(c->quality());
      return cand;
    }
    case State::kYieldOriginalFirst:
    case State::kYieldOriginalRemaining:
      return original_->Peek();

    case State::kYieldCorrectedShadow: {
      auto c = corrected_->Peek();
      if (!c) return nullptr;
      // [未全替换]：严格保留原来的原生 original_preedit_
      auto cand = New<SimpleCandidate>("shadow", start_, end_, c->text(), c->comment(), original_preedit_);
      double q = (reference_quality_ > 0) ? reference_quality_ - 0.1 : c->quality();
      cand->set_quality(q);
      return cand;
    }
    case State::kExhausted:
      return nullptr;
  }
  return nullptr;
}

void TypoTranslation::EvaluateState() {
  while (state_ != State::kExhausted) {
    switch (state_) {
      case State::kExclusiveOverride:
        if (corrected_ && !corrected_->exhausted()) {
          set_exhausted(false); return;
        }
        state_ = State::kExhausted;
        break;

      case State::kYieldOriginalFirst:
        if (original_ && !original_->exhausted()) {
           reference_quality_ = original_->Peek()->quality();
           set_exhausted(false); return;
        }
        state_ = State::kYieldCorrectedShadow;
        break;

      case State::kYieldCorrectedShadow:
        if (corrected_ && !corrected_->exhausted()) {
           set_exhausted(false); return;
        }
        state_ = State::kYieldOriginalRemaining;
        break;

      case State::kYieldOriginalRemaining:
        if (original_ && !original_->exhausted()) {
           set_exhausted(false); return;
        }
        state_ = State::kExhausted;
        break;

      case State::kExhausted:
        set_exhausted(true); return;
    }
  }
  set_exhausted(true);
}

}  // namespace rime