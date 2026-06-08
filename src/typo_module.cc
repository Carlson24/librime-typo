#include <rime/common.h>
#include <rime/registry.h>
#include <rime_api.h>
#include "typo_filter.h"

namespace rime {

// 显式声明组件构建器，兼容所有 Rime 版本
class TypoFilterComponent : public Filter::Component {
 public:
  Filter* Create(const Ticket& ticket) override {
    return new TypoFilter(ticket);
  }
};

}  // namespace rime

static void rime_typo_initialize() {
  using namespace rime;

  LOG(INFO) << "registering components from module 'typo'.";
  Registry& r = Registry::instance();

  // 将 typo_filter 注册到 Rime 组件系统
  r.Register("typo_filter", new TypoFilterComponent());
}

static void rime_typo_finalize() {
}

RIME_REGISTER_MODULE(typo)