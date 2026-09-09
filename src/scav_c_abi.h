#ifndef SCAV_C_ABI_H_INCLUDED
#define SCAV_C_ABI_H_INCLUDED

// The ABI checks more than one library's C surface performs, which is why they
// live at the src root rather than in either -- the same reason as
// scav_c_handles.h, and libscavdraw reaches libscavlayout's POD header without
// linking its code. Nothing installs this.

#include "scav/scav_layout_c.h"

namespace scav {

// Every table's declared stride against this build's row size, counts ignored:
// a stride is what the caller's own header says a row is, so a member that
// header does not have reads as zero and is refused before anything walks a
// row. One definition, because a fifth table would otherwise be checked in one
// entry point and not the other.
inline bool spaces_strides_agree(scav_spaces const &s) {
  return (s.box_state_stride == sizeof(scav_box_space)) &&
         (s.box_sub_stride == sizeof(scav_box_space)) &&
         (s.path_clear_stride == sizeof(scav_path_clear)) &&
         (s.path_box_stride == sizeof(scav_path_box));
}

}  // namespace scav

#endif  // SCAV_C_ABI_H_INCLUDED
