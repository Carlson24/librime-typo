#ifndef RIME_TYPO_FILTER_H_
#define RIME_TYPO_FILTER_H_

#include <rime/filter.h>
#include <rime/component.h>
#include <rime/translation.h>
#include <rime/translator.h>
#include <rime/config.h>

#include <unordered_map>
#include <string>

namespace rime {

class TypoFilter : public Filter {
 public:
  explicit TypoFilter(const Ticket& ticket);
  virtual ~TypoFilter() = default;

  virtual an<Translation> Apply(an<Translation> translation,
                                CandidateList* candidates) override;

 private:
  std::string GetCorrectedInput(const std::string& input, int& correction_count, size_t& max_correction_len) const;
  void LoadCorrections(Engine* engine, const std::string& input_type, const std::string& custom_file);
  std::string DetectInputType(Config* config) const;

  std::unordered_map<std::string, std::string> correction_map_;
  size_t min_depth_ = 0;
  size_t max_depth_ = 0;
  std::string current_key_ = "";
  
  bool is_enabled_ = false;
  bool show_corrected_preedit_ = false; // 默认 false: 启用等价位置替换
  an<Translator> translator_;
};

class TypoTranslation : public Translation {
 public:
  TypoTranslation(an<Translation> original, 
                  an<Translation> corrected, 
                  size_t start, 
                  size_t end,
                  bool exclusive_override,
                  bool show_corrected_preedit,
                  const std::string& original_preedit,
                  const std::string& raw_segment); // 传入实际敲击的纯字母片段
  virtual ~TypoTranslation() = default;

  virtual bool Next() override;
  virtual an<Candidate> Peek() override;

 private:
  enum class State {
    kExclusiveOverride,
    kYieldOriginalFirst,
    kYieldCorrectedShadow,
    kYieldOriginalRemaining,
    kExhausted
  };

  void EvaluateState();

  an<Translation> original_;
  an<Translation> corrected_;
  size_t start_;
  size_t end_;
  std::string original_preedit_;
  std::string raw_segment_;
  bool show_corrected_preedit_;
  
  State state_; 
  double reference_quality_ = 0.0;
};

}  // namespace rime

#endif  // RIME_TYPO_FILTER_H_