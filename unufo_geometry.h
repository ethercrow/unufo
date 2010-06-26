#ifndef ESYNTH_GEOMETRY_H
#define ESYNTH_GEOMETRY_H

#include "unufo_types.h"

namespace unufo {

inline bool clip(Bitmap<uint8_t> &image, const Coordinates& point)
{
    if (point.x < 0 ||
        point.y < 0 ||
        point.x >= image.width ||
        point.y >= image.height)
        return false;
    return true;
}

int collect_defined_in_both_areas(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        const Coordinates& position, const Coordinates& candidate,
        int area_size,
        uint8_t* def_n_p, uint8_t* def_n_c,
        int& defined_only_near_pos, int& confidence_sum);
}

#endif // ESYNTH_GEOMETRY_H

