#include <vector>

#include "unufo_types.h"

namespace unufo {

void transfer_patch(const Bitmap<uint8_t>& data, int bpp,
        const Bitmap<uint8_t>& confidence_map,
        const Matrix<Coordinates>& transfer_map,
        const Matrix<int>& transfer_belief,
        const Coordinates& position, const Coordinates& source,
        int belief, const std::vector<int>& best_color_diff);

int get_difference_color_adjustment(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        int comp_patch_radius,
        const Coordinates& candidate,
        const Coordinates& position,
        std::vector<int>& best_color_diff,
        int best, int bpp,
        int max_adjustment, bool equal_adjustment);

int get_difference(const Bitmap<uint8_t>& data,
        const Bitmap<uint8_t>& confidence_map,
        int comp_patch_radius,
        const Coordinates& candidate,
        const Coordinates& position, int best);

}

