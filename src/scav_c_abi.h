#ifndef SCAV_C_ABI_H_INCLUDED
#define SCAV_C_ABI_H_INCLUDED

// ABI checks shared by more than one library's C surface; not installed.
// libscavdraw includes it for layout's POD types without linking libscavlayout.

#include "scav/scav_layout_c.h"

namespace scav {

// True when each table's declared stride equals this build's row size; counts are
// ignored. A stride the caller's header lacks reads as zero and fails.
inline bool spaces_strides_agree(scav_spaces const &s) {
  return (s.box_state_stride == sizeof(scav_box_space)) &&
         (s.box_sub_stride == sizeof(scav_box_space)) &&
         (s.path_clear_stride == sizeof(scav_path_clear)) &&
         (s.path_box_stride == sizeof(scav_path_box));
}

}  // namespace scav

#endif  // SCAV_C_ABI_H_INCLUDED
