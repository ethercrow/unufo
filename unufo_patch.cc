#include "unufo_patch.h"

#include "unufo_geometry.h"
#include "unufo_pixel.h"

using namespace std;

namespace unufo {

void transfer_patch(const Bitmap<uint8_t>& data, int bpp,
        const Bitmap<uint8_t>& confidence_map,
        const Matrix<Coordinates>& transfer_map,
        const Matrix<int>& transfer_belief,
        const Coordinates& position, const Coordinates& source,
        int belief, const vector<int>& best_color_diff)
{
    for(int j=0; j<bpp; j++) {
        int new_color = data.at(source)[j] + best_color_diff[j];
        data.at(position)[j] = new_color;
    }
    // TODO: better confidence transfer
    *confidence_map.at(position) = *confidence_map.at(source);
    *transfer_map.at(position) = source;
    *transfer_belief.at(position) = belief;
}

int get_difference_color_adjustment(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        const Matrix<int>& transfer_belief,
        int comp_patch_radius,
        const Coordinates& candidate,
        const Coordinates& position,
        vector<int>& best_color_diff,
        int best, int bpp,
        int max_adjustment, bool equal_adjustment)
{
    int max_defined_size = 4*(2*comp_patch_radius + 1)*(2*comp_patch_radius + 1);
    int defined_only_near_pos;

    int accum[4] = {0, 0, 0, 0};

    uint8_t defined_near_pos [max_defined_size];
    uint8_t defined_near_cand[max_defined_size];

    int compared_count = collect_defined_in_both_areas(data,
            confidence_map, transfer_belief,
            position, candidate,
            comp_patch_radius,
            defined_near_pos, defined_near_cand,
            defined_only_near_pos);

    if (!compared_count)
        return best;

    int sum = defined_only_near_pos*max_diff;

    uint8_t* def_n_p = defined_near_pos;
    uint8_t* def_n_c = defined_near_cand;

    for (int i=0; i<compared_count; ++i) {
        for (int j=0; j<4; ++j)
            accum[j] += (int(def_n_p[j]) - int(def_n_c[j]));
        def_n_p += 4;
        def_n_c += 4;
    }

    for(int j=0; j<4; ++j) {
        accum[j] /= compared_count;
        if (accum[j] < -max_adjustment)
            accum[j] = -max_adjustment;

        if (accum[j] > max_adjustment)
            accum[j] = max_adjustment;
    }

    if (equal_adjustment) {
        int color_diff_sum = 0;
        for(int j=0; j<4; ++j)
            color_diff_sum += accum[j];
        for(int j=0; j<4; ++j)
            accum[j] = color_diff_sum/bpp;
    }

    def_n_p = defined_near_pos;
    def_n_c = defined_near_cand;
    for (int i=0; i<compared_count; ++i) {
        for (int j=0; j<4; ++j) {
            int c = int(def_n_c[j]) + accum[j];
            // do not allow color clipping
            if (c < 0 || c > 255)
                return best;
            sum += pixel_diff(c, def_n_p[j]);
        }
        def_n_p += 4;
        def_n_c += 4;
    }

    if (sum < best)
       best_color_diff.assign(accum, accum+bpp);
    return sum;
}

int get_difference(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        const Matrix<int>& transfer_belief,
        int comp_patch_radius,
        const Coordinates& candidate,
        const Coordinates& position, int best)
{
    int max_defined_size = 4*(2*comp_patch_radius + 1)*(2*comp_patch_radius + 1);
    int defined_only_near_pos;

    uint8_t defined_near_pos [max_defined_size];
    uint8_t defined_near_cand[max_defined_size];

    int compared_count = collect_defined_in_both_areas(data,
            confidence_map, transfer_belief,
            position, candidate,
            comp_patch_radius,
            defined_near_pos, defined_near_cand,
            defined_only_near_pos);

    if (compared_count) {
        int sum = defined_only_near_pos*max_diff;

        uint8_t* def_n_p = defined_near_pos;
        uint8_t* def_n_c = defined_near_cand;
        for (int i=0; i<compared_count; ++i) {
            for (int j=0; j<4; ++j) {
                sum += pixel_diff(def_n_c[j], def_n_p[j]);
            }

            def_n_p += 4;
            def_n_c += 4;
        }

        return sum;
    } else
        return best;
}

int get_complexity(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        const Matrix<int>& transfer_belief,
        const Coordinates& point, int comp_patch_radius,
        int bpp)
{
    // TODO: improve complexity metric
    int confidence_sum = 0;
    int defined_count = 0;

    // get mean color
    Coordinates defined_points[comp_patch_radius*comp_patch_radius*4 + comp_patch_radius*4 + 1];
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates point_off = point + Coordinates(ox, oy);
            if (clip(data, point_off) && *confidence_map.at(point_off)) {
                confidence_sum += *confidence_map.at(point_off);
                defined_points[defined_count++] = point_off;
            }
        }

    if (!defined_count) {
        return -1;
    }

    int mean_values[bpp];
    for (int j = 0; j<bpp; ++j)
        mean_values[j] = 0;
    for (int i = 0; i<defined_count; ++i) {
        uint8_t* colors = data.at(defined_points[i]);
        for (int j = 0; j<bpp; ++j)
            mean_values[j] = colors[j];
    }

    // compute local deviation
    // spatial weight function is 1/(1+sqared_distance_from_point)
    int weighted_dev;
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates point_off = point + Coordinates(ox, oy);
            if (clip(data, point_off) && *confidence_map.at(point_off))
                for (int j = 0; j<bpp; ++j) {
                    int d = (data.at(point_off)[j] - mean_values[j]);
                    weighted_dev += d*d/(1+ox*ox+oy*oy);
                }
        }

    // multiply by average confidence among defined points
    weighted_dev *= (confidence_sum/defined_count);

    return weighted_dev;
}

}

