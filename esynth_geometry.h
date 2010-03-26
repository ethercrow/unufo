#ifndef ESYNTH_GEOMETRY_H
#define ESYNTH_GEOMETRY_H

#include "esynth_types.h"

inline bool clip(Bitmap<Pixelel> &image, const Coordinates& point)
{
    if (point.x < 0 ||
        point.y < 0 ||
        point.x >= image.width ||
        point.y >= image.height)
        return false;
    return true;
}

bool wrap_or_clip(Parameters &parameters, Bitmap<Pixelel> &image, Coordinates &point)
{ 
    while(point.x < 0)
        if (parameters.h_tile)
            point.x += image.width;
        else
            return false;

    while(point.x >= image.width)
        if (parameters.h_tile)
            point.x -= image.width;
        else
            return false;

    while(point.y < 0)
        if (parameters.v_tile)
            point.y += image.height;
        else
            return false;

    while(point.y >= image.height)
        if (parameters.v_tile)
            point.y -= image.height;
        else
            return false;

    return true;
}

#endif // ESYNTH_GEOMETRY_H

