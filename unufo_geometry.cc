#include "unufo_geometry.h"

#include "unufo_consts.h"

namespace unufo {

// TODO: consider mirroring and rotation by passing orientation
// collect pixels defined both near pos and near candidate
int collect_defined_in_both_areas(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        const Matrix<int>& transfer_belief,
        const Coordinates& position, const Coordinates& candidate,
        int area_size,
        uint8_t* def_n_p, uint8_t* def_n_c,
        int& defined_only_near_pos)
{
    int defined_count = 0;
    defined_only_near_pos = 0;

    // figure out if we need to check boundaries in-loop 
    bool far_from_boundary = (position.x -  area_size >= 0 &&
                              position.y -  area_size >= 0 &&
                              position.x +  area_size < data.width &&
                              position.y +  area_size < data.height &&
                              candidate.x - area_size >= 0 &&
                              candidate.y - area_size >= 0 &&
                              candidate.x + area_size < data.width &&
                              candidate.y + area_size < data.height);

    uint8_t* d_n_p =  data.at(position  + Coordinates(-area_size, -area_size));
    uint8_t* d_n_c =  data.at(candidate + Coordinates(-area_size, -area_size));
    uint8_t* ds_n_p = confidence_map.at(position  + Coordinates(-area_size, -area_size));
    uint8_t* ds_n_c = confidence_map.at(candidate + Coordinates(-area_size, -area_size));

    int d_shift  = 4*(data.width - (2*area_size + 1));
    if (far_from_boundary) {
        // branch without in-loop boundary checking
        for (int oy=-area_size; oy<=area_size; ++oy) {
            for (int ox=-area_size; ox<=area_size; ++ox) {
                if (*ds_n_c && *ds_n_p)
                {
                    ++defined_count;

                    // 4 byte copy
                    *((int32_t*)def_n_p) = *((int32_t*)d_n_p);
                    *((int32_t*)def_n_c) = *((int32_t*)d_n_c);

                    def_n_p += 4;
                    def_n_c += 4;
                } else if (!*ds_n_c) {
                    // also collect number of points defined only near destination pos
                    ++defined_only_near_pos;
                }
                d_n_p  += 4;
                d_n_c  += 4;
                ds_n_p += 4;
                ds_n_c += 4;
            }
            d_n_p  += d_shift;
            d_n_c  += d_shift;
            ds_n_p += d_shift;
            ds_n_c += d_shift;
        }
    } else {
        // branch with in-loop boundary checking
        for (int oy=-area_size; oy<=area_size; ++oy) {
            for (int ox=-area_size; ox<=area_size; ++ox) {
                if (position.x + ox >= 0 &&
                    position.y + oy >= 0 &&
                    position.x + ox < data.width &&
                    position.y + oy < data.height &&
                    candidate.x + ox >= 0 &&
                    candidate.y + oy >= 0 &&
                    candidate.x + ox < data.width &&
                    candidate.y + oy < data.height)
                {
                    if (*ds_n_c && *ds_n_p)
                    {
                        ++defined_count;

                        // 4 byte copy
                        *((int32_t*)def_n_p) = *((int32_t*)d_n_p);
                        *((int32_t*)def_n_c) = *((int32_t*)d_n_c);

                        def_n_p += 4;
                        def_n_c += 4;
                    } else if (!*ds_n_c) {
                        // also collect number of points defined only near destination pos
                        ++defined_only_near_pos;
                    }
                }
                d_n_p  += 4;
                d_n_c  += 4;
                ds_n_p += 4;
                ds_n_c += 4;
            }
            d_n_p  += d_shift;
            d_n_c  += d_shift;
            ds_n_p += d_shift;
            ds_n_c += d_shift;
        }
    }
    return defined_count;
}

}

