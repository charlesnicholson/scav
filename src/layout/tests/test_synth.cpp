#include "layout/tests/test_synth.h"

#include "scav/scav_core.h"
#include "scav/scav_layout_c.h"

#include <cstdint>
#include <vector>

namespace scav {

Chart nested_2k_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  for (uint32_t r = 0; r < 8; ++r) {
    SubmachineId parent{ root };
    StateId last{ INVALID };
    for (uint32_t d = 0; d < 16; ++d) {
      std::vector<StateId> level;
      level.reserve(15);
      for (uint32_t k = 0; k < 15; ++k) {
        level.push_back(build_state(c, parent, {}, StateKind::Normal, {}));
      }
      StateId const comp{ build_state(c, parent, {}, StateKind::Normal, {}) };
      for (uint32_t k = 1; k < level.size(); ++k) {
        build_trans(c, level[k - 1], level[k], TransKind::External, {});
        build_trans(c, level[k], comp, TransKind::External, {});
      }
      if (last.v != INVALID) { build_trans(c, comp, last, TransKind::External, {}); }
      last = comp;
      parent = build_submachine(c, comp, {}, {});
    }
  }
  return c;
}

Chart flat_2k_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "flat", {}) };
  std::vector<StateId> all;
  all.reserve(2048);
  for (uint32_t i = 0; i < 2048; ++i) {
    all.push_back(build_state(c, root, {}, StateKind::Normal, {}));
  }
  for (uint32_t i = 1; i < all.size(); ++i) {
    build_trans(c, all[i - 1], all[i], TransKind::External, {});
    if ((i % 16) == 0) { build_trans(c, all[i], all[i - 16], TransKind::External, {}); }
  }
  return c;
}

Chart sealed_chart() {
  Chart c;
  SubmachineId const root{ build_chart(c, "t", {}) };
  StateId const bar{ build_state(c, root, "S", StateKind::Fork, {}) };
  StateId const outer{ build_state(c, root, "P", StateKind::Normal, {}) };
  SubmachineId const inner{ build_submachine(c, outer, {}, {}) };
  StateId const deep{ build_state(c, inner, "C", StateKind::Normal, {}) };
  build_trans(c, bar, deep, TransKind::External, {});
  return c;
}

scav_profile sealed_profile(scav_profile const &base) {
  scav_profile p{ base };
  p.pad = 16;
  p.rank_sep = 0;
  p.node_sep = 576;
  return p;
}

}  // namespace scav
