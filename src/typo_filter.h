#ifndef RIME_TYPO_FILTER_H_
#define RIME_TYPO_FILTER_H_

#include <rime/filter.h>
#include <rime/component.h>
#include <rime/translation.h>
#include <rime/translator.h>
#include <rime/config.h>
#include <marisa.h>
#include <string>

namespace rime {

class TypoFilter : public Filter {
 public:
  explicit TypoFilter(const Ticket& ticket);
  virtual ~TypoFilter() = default;

  virtual an<Translation> Apply(an<Translation> translation,
                                CandidateList* candidates) override;

 private:
  std::string GetCorrectedInput(const std::string& input, int& correction_count,
                                size_t& max_correction_len, const std::string& segment_tag,
                                bool is_pinyin, std::string& out_local_text,
                                const std::string& delimiters) const;

  void LoadCorrections(Engine* engine, const std::string& input_type,
                       const std::string& custom_file);

  std::string DetectInputType(Config* config) const;

  marisa::Trie trie_;
  bool trie_loaded_ = false;
  std::string current_key_ = "";

  bool is_enabled_ = false;
  bool show_corrected_preedit_ = false;
  std::string correction_hint_;
  an<Translator> translator_;
};

}  // namespace rime

#endif  // RIME_TYPO_FILTER_H_