#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "typo_filter.h"

using namespace rime;

static void rime_typo_initialize() {
  LOG(INFO) << "registering components from module 'typo'.";
  Registry& r = Registry::instance();
  r.Register("typo_filter", new Component<TypoFilter>);
}

static void rime_typo_finalize() {
}

RIME_REGISTER_MODULE(typo)