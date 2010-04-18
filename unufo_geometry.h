#ifndef ESYNTH_GEOMETRY_H
#define ESYNTH_GEOMETRY_H

#include "unufo_types.h"

inline bool clip(Bitmap<uint8_t> &image, const Coordinates& point)
{
    if (point.x < 0 ||
        point.y < 0 ||
        point.x >= image.width ||
        point.y >= image.height)
        return false;
    return true;
}

#endif // ESYNTH_GEOMETRY_H

