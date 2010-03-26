/*
   The Resynthesizer - A GIMP plug-in for resynthesizing textures
   Copyright (C) 2000 2008  Paul Francis Harrison
   Copyright (C) 2002  Laurent Despeyroux
   Copyright (C) 2002  David Rodríguez García

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <gtk/gtk.h>

#include <vector>
#include <utility>
#include <algorithm>

using namespace std;

//#include <boost/thread/thread.hpp>
//#include <boost/thread/future.hpp>

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#include "esynth_types.h"
#include "esynth_consts.h"
#include "esynth_gimp_comm.h"
#include "esynth_geometry.h"
#include "bench.h"

/* Helpers for the main function */
static FILE* logfile;

static int diff_table[512];
static int* diff_table_0; // shifted so that diff_table_0[0] == diff_table[256] == 0
static int max_diff;

static int input_bytes;
static int comp_patch_radius, transfer_patch_radius;

static bool equal_adjustment;
static int max_adjustment;

static bool invent_gradients;
static bool use_ref_layer;

// we must fill selection subset of data
// using ref_mask subset of ref_layer for inspiration
// status holds current state of point filling
static Bitmap<Pixelel> data, data_mask, ref_layer, ref_mask;
static Bitmap<uint8_t> confidence_map;
static int sel_x1, sel_y1, sel_x2, sel_y2;

// data_points is a queue of points to be filled,
// it can contain duplicates, which mean points re-analysis
//
// sorted_offsets is an array of points near origin, beginning with (0,0)
// and sorted by distance from origin (see Coordinates::operator< for 
// current definition of 'distance')
static vector<Coordinates> data_points, sorted_offsets;
static vector<Coordinates> ref_points(0);

static int best;
static Coordinates best_point;
static vector<int> best_color_diff(0);

static double neglog_cauchy(double x)
{
    return log(x*x+1.0);
}

static void make_offset_list(void)
{
    sorted_offsets.resize(0);
    int half_side = sqrt(max_neighbours);
    for(int y=-half_side-1; y<half_side+1; ++y)
        for(int x=-half_side-1; x<half_side+1; ++x)
            sorted_offsets.push_back(Coordinates(x,y));

    sort(sorted_offsets.begin(), sorted_offsets.end());
}

static void setup_metric(float autism)
{
    // TODO: improve patch difference metric
    for(int i=-256;i<256;i++) {
        //double value = neglog_cauchy(i/256.0/autism) /
        //neglog_cauchy(1.0/autism) * 65536.0;
        //diff_table[256+i] = int(value);
        
        diff_table[256+i] = i*i;
    }
    diff_table_0 = diff_table + 256;
    max_diff = diff_table[0];
}

// structural complexity of point's neighbourhood
static int get_complexity(const Coordinates& point)
{
    // TODO: improve complexity metric
    int confidence_sum = 0;

    // calculate std dev
    // TODO: optimize
    vector<Coordinates> defined_points(0);
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates point_off = point + Coordinates(ox, oy);
            if (*confidence_map.at(point_off)) {
                confidence_sum += *confidence_map.at(point_off);
                defined_points.push_back(point_off);
            }
        }

    int defined_count = defined_points.size();

    int mean_values[input_bytes];
    for (int j = 0; j<input_bytes; ++j)
        mean_values[j] = 0;
    for (int i = 0; i<defined_count; ++i) {
        Pixelel* colors = data.at(defined_points[i]);
        for (int j = 0; j<input_bytes; ++j)
            mean_values[j] = colors[j];
    }

    int stddev[input_bytes];
    for (int j = 0; j<input_bytes; ++j)
        stddev[j] = 0;
    for (int i = 0; i<defined_count; ++i) {
        Pixelel* colors = data.at(defined_points[i]);
        for (int j = 0; j<input_bytes; ++j) {
            int c = mean_values[j] - colors[j];
            stddev[j] += c*c;
        }
    }

    int result = 0;
    for (int j = 0; j<input_bytes; ++j)
        result += stddev[j];
    result /= defined_count;

    // multiply by average confidence among defined points
    result *= (confidence_sum/defined_count);

    return result;
}

int purge_already_filled()
{
    int result = 0;
    for(size_t i=0; i < data_points.size(); ++i) {
        Coordinates position = data_points[i];

        // check if this point has been already set
        if (*confidence_map.at(position)) {
            data_points.erase(data_points.begin() + i--);
            ++result;
        }
    }
    return result;
}

void get_edge_points(vector<pair<int, Coordinates>>& edge_points)
{
    for(size_t i=0; i < data_points.size(); ++i) {
        Coordinates position = data_points[i];
        bool island_flag = true;
        for (int ox=-1; ox<=1; ++ox)
            for (int oy=-1; oy<=1; ++oy)
                if (*confidence_map.at(position + Coordinates(ox, oy)))
                    island_flag = false;
        if (!island_flag) {
            int complexity = get_complexity(data_points[i]);
            edge_points.push_back(std::make_pair(complexity, data_points[i]));
        }
    }
    sort(edge_points.begin(), edge_points.end());

    // TODO: partition the edge_points array according to complexity threshold
    // process high complexity points with patch searching
    // process low complexity points with extrapolation
    // or interpolation.. depends on where you're looking from

    // leave only the most important edge_points
    if (edge_points.size() > important_count)
        edge_points.erase(edge_points.begin(),
            edge_points.end()-edge_points.size()/2);
}

void transfer_patch(const Coordinates& position, const Coordinates& source)
{
    for (int ox=-transfer_patch_radius; ox<=transfer_patch_radius; ++ox)
        for (int oy=-transfer_patch_radius; oy<=transfer_patch_radius; ++oy) {
            Coordinates offset(ox, oy);
            Coordinates near_dst = position + offset;
            Coordinates near_src = source + offset;
            // transfer only defined points and only to undefined points
            if (clip(data, near_dst) && clip(data, near_src) &&
                !(*confidence_map.at(near_dst)) &&
                *confidence_map.at(near_src))
            {
                for(int j=0;j<input_bytes;j++) {
                    int new_color = data.at(near_src)[j] + best_color_diff[j];
                    //if (new_color < -256) new_color = 0;
                    //if (new_color >  255) new_color = 255;
                    data.at(near_dst)[j] = new_color;
                }
                // TODO: better confidence transfer
                *confidence_map.at(near_dst) = max(10,
                    *confidence_map.at(near_src) - 5); 
            }
        }
}

// TODO: consider mirroring and rotation by passing orientation
// collect pixels defined both near pos and near candidate
inline int collect_defined_in_both_areas(const Coordinates& position, const Coordinates& candidate,
                Pixelel* def_n_p, Pixelel* def_n_c,
                int& defined_only_near_pos)
{
    int defined_count = 0;
    defined_only_near_pos = 0;

    Pixelel* d_n_p =        data.at(position  + Coordinates(-comp_patch_radius, -comp_patch_radius));
    Pixelel* d_n_c =        data.at(candidate + Coordinates(-comp_patch_radius, -comp_patch_radius));
    uint8_t* ds_n_p = confidence_map.at(position  + Coordinates(-comp_patch_radius, -comp_patch_radius));
    uint8_t* ds_n_c = confidence_map.at(candidate + Coordinates(-comp_patch_radius, -comp_patch_radius));

    int d_shift  = 4*(data.width - (comp_patch_radius<<1) - 1);
    for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
        for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox) {
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
    return defined_count;
}

static int get_difference_color_adjustment(const Coordinates& candidate,
                          const Coordinates& position,
                          vector<int>& best_color_diff)
{
    int max_defined_size = 4*(2*comp_patch_radius + 1)*(2*comp_patch_radius + 1);
    int defined_only_near_pos = 0;
    int accum[4];
    //memset(accum, 0, 4*sizeof(int));
    *((int64_t*)accum) = 0LL;
    *((int64_t*)accum+1) = 0LL;

    Pixelel  defined_near_pos [max_defined_size];
    Pixelel  defined_near_cand[max_defined_size];

    int compared_count = collect_defined_in_both_areas(position, candidate,
            defined_near_pos, defined_near_cand, defined_only_near_pos);

    if (compared_count) {
        int sum = defined_only_near_pos*max_diff;

        Pixelel* def_n_p = defined_near_pos;
        Pixelel* def_n_c = defined_near_cand;

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
                accum[j] = color_diff_sum/input_bytes;
        }

        def_n_p = defined_near_pos;
        def_n_c = defined_near_cand;
        for (int i=0; i<compared_count; ++i) {
            for (int j=0; j<4; ++j) {
                int d = int(def_n_c[j]) + accum[j];
                // do not allow color clipping
                if (d < 0 || d > 255)
                    return best+1;
                d -= int(def_n_p[j]);
                sum += diff_table_0[d];
            }
            def_n_p += 4;
            def_n_c += 4;
        }

        if (sum < best)
           best_color_diff.assign(accum, accum+input_bytes);
        return sum;
    } else
        return best;
}

static int get_difference(const Coordinates& candidate,
                          const Coordinates& position)
{
    int max_defined_size = 4*(2*comp_patch_radius + 1)*(2*comp_patch_radius + 1);
    int defined_only_near_pos = 0;

    Pixelel  defined_near_pos [max_defined_size];
    Pixelel  defined_near_cand[max_defined_size];

    int compared_count = collect_defined_in_both_areas(position, candidate,
            defined_near_pos, defined_near_cand, defined_only_near_pos);

    if (compared_count) {
        int sum = defined_only_near_pos*max_diff;

        Pixelel* def_n_p = defined_near_pos;
        Pixelel* def_n_c = defined_near_cand;
        for (int i=0; i<compared_count; ++i) {
            for (int j=0; j<4; ++j) {
                int d = int(def_n_c[j]) - int(def_n_p[j]);
                sum += diff_table_0[d];
            }

            def_n_p += 4;
            def_n_c += 4;
        }

        return sum;
    } else
        return best;
}

// return patch dissimilarity with linear gradient patch
// algorithm is sort of poor man linear regression
static int get_gradientness(const Coordinates& position, 
        int mean_values[], int grad_x[], int grad_y[])
{
    int defined_count = 0;
    int defined_ox_count = 0;
    int defined_oy_count = 0;

    memset(mean_values, 0, input_bytes*sizeof(int));
    memset(grad_x, 0, input_bytes*sizeof(int));
    memset(grad_y, 0, input_bytes*sizeof(int));

    // calculate mean values
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates near_pos = position + Coordinates(ox, oy);
            if (clip(data, near_pos) && *confidence_map.at(near_pos)) {
                ++defined_count;
                for (int j = 0; j<input_bytes; ++j)
                    mean_values[j] += data.at(near_pos)[j];
            }
        }

    for (int j = 0; j<input_bytes; ++j)
        mean_values[j] /= defined_count;

    // calculate gradient
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates near_pos = position + Coordinates(ox, oy);
            if (clip(data, near_pos) && *confidence_map.at(near_pos)) {
                if (ox) {
                    ++defined_ox_count;
                    for (int j = 0; j<input_bytes; ++j)
                        grad_x[j] += (data.at(near_pos)[j] - mean_values[j])/ox;
                }
                if (oy) {
                    ++defined_oy_count;
                    for (int j = 0; j<input_bytes; ++j)
                        grad_y[j] += (data.at(near_pos)[j] - mean_values[j])/oy;
                }
            }
        }

    for (int j = 0; j<input_bytes; ++j) {
        grad_x[j] /= defined_ox_count;
        grad_y[j] /= defined_oy_count;
    }

    // calculate dissimilarity with gradient
    int error_sum = 0;
    for (int ox=-comp_patch_radius; ox<=comp_patch_radius; ++ox)
        for (int oy=-comp_patch_radius; oy<=comp_patch_radius; ++oy) {
            Coordinates near_pos = position + Coordinates(ox, oy);
            if (clip(data, near_pos) && *confidence_map.at(near_pos)) 
                for (int j = 0; j<input_bytes; ++j) {
                    int d = (data.at(near_pos)[j] - mean_values[j] - (grad_x[j]*ox) - (grad_y[j]*oy));
                    error_sum += diff_table_0[d];
                }
        }

    return error_sum;
}

static inline void try_point(const Coordinates& candidate,
                             const Coordinates& position,
                             int& best,
                             Coordinates& best_point,
                             vector<int>& best_color_diff)
{
    int difference;
    if (max_adjustment)
        difference = get_difference_color_adjustment(candidate, position, best_color_diff);
    else
        difference = get_difference(candidate, position);

    if (best <= difference)
        return;
    best = difference;
    best_point = candidate;
}

class refine_callable
{
public:
    refine_callable(int n, const Coordinates& position): n_(n), position_(position){}

    Coordinates operator()() {
        // thread local vars
        int tl_best = 1<<30;
        Coordinates tl_best_point;
        vector<int> tl_best_color_diff(4, 0);

        if (use_ref_layer) {
            for (int j=0; j<n_; ++j) {
                const Coordinates& candidate = ref_points[rand()%ref_points.size()];
                try_point(candidate, position_, tl_best, tl_best_point, tl_best_color_diff);
            }
        } else {
            for (int j=0; j<n_; ++j) {
                int x, y;
                // FIXME: this will suck with large rectangular selections with small borders
                do {
                    x = sel_x1 + rand()%(sel_x2 - sel_x1);
                    y = sel_y1 + rand()%(sel_y2 - sel_y1);
                } while (data_mask.at(x,y)[0]);
                try_point(Coordinates(x, y), position_, tl_best, tl_best_point, tl_best_color_diff);
            }
        }

        return tl_best_point;
    }
private:
    int n_;
    Coordinates position_;
};


/* This is the main function. */

static void run(const gchar*,
        gint nparams,
        const GimpParam *param,
        gint *nreturn_vals,
        GimpParam **return_vals)
{
    static GimpParam values[1];
    Parameters parameters;
    GimpDrawable *drawable, *corpus_drawable, *ref_drawable;

    logfile = fopen(LOG_FILE, "wt");

    //////////////////////////////
    // Gimp setup dragons BEGIN
    //////////////////////////////

    srand(time(0));
    textdomain("resynthesize") ;

    /* Unless anything goes wrong, result is success */
    *nreturn_vals = 1;
    *return_vals = values;
    values[0].type = GIMP_PDB_STATUS;
    values[0].data.d_status = GIMP_PDB_SUCCESS;

    init_gtk();

    if (!get_parameters_from_list(&parameters, nparams, param)) {
        fprintf(logfile, "get_parameters_from_list failed\n");
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Get drawable */
    drawable = gimp_drawable_get(param[2].data.d_drawable);

    if (!gimp_drawable_is_rgb(drawable->drawable_id) &&
            !gimp_drawable_is_gray(drawable->drawable_id)) {
        gimp_message(_("Bad color mode, must be RGB* or GRAY*"));
        gimp_drawable_detach(drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    corpus_drawable = gimp_drawable_get(parameters.corpus_id);
    if (corpus_drawable->bpp != drawable->bpp) {
        gimp_message(_("The input texture and output image must have the same number of color channels."));
        gimp_drawable_detach(drawable);
        gimp_drawable_detach(corpus_drawable);
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    use_ref_layer = parameters.use_ref_layer;

    if (use_ref_layer) {
        ref_drawable = gimp_drawable_get(param[19].data.d_drawable);
        if (ref_drawable->bpp != drawable->bpp) {
            gimp_message(_("Working layer and reference layer must have the same color depth"));
            gimp_drawable_detach(drawable);
            gimp_drawable_detach(corpus_drawable);
            gimp_drawable_detach(ref_drawable);
            values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
            return;
        }
    }

    gimp_progress_init(_("Resynthesize"));
    gimp_progress_update(0.0);

    comp_patch_radius     = parameters.comp_size;
    transfer_patch_radius = parameters.transfer_size;

    equal_adjustment = parameters.equal_adjustment;
    max_adjustment   = parameters.max_adjustment;
    invent_gradients = parameters.invent_gradients;

    input_bytes  = drawable->bpp;

    /* Fetch the whole image data */
    fetch_image_and_mask(drawable, data, input_bytes, data_mask, 255, sel_x1, sel_y1, sel_x2, sel_y2);

    confidence_map.size(data.width,data.height,1);

    data_points.resize(0);

    for(int y=0;y<confidence_map.height;y++)
        for(int x=0;x<confidence_map.width;x++) {
            *confidence_map.at(x,y) = 0;

            if (parameters.use_border && data_mask.at(x,y)[0] == 0)
                *confidence_map.at(x,y) = 255;

            if (data_mask.at(x,y)[0] != 0)
                data_points.push_back(Coordinates(x,y));
        }

    /* Fetch the ref_layer */

    if (!parameters.use_ref_layer) {
        // resynthesizer legacy
        fetch_image_and_mask(corpus_drawable, ref_layer, input_bytes, ref_mask, 0);
    } else {
        // Fetch reference_mask layer
        fetch_image_and_mask(ref_drawable, ref_layer, input_bytes, ref_mask, 0);
        for(int y=0;y<ref_layer.height;y++)
            for(int x=0;x<ref_layer.width;x++)
                if ((ref_layer.at(x,y)[0] | ref_layer.at(x,y)[3]) &&
                    !data_mask.at(x,y)[0])
                {
                    ref_points.push_back(Coordinates(x,y));
                }
    }

    fprintf(logfile, "gimp setup dragons end\n");
    //////////////////////////////
    // Gimp setup dragons END
    //////////////////////////////

    /* little geometry so that sel_x1 and sel_y1 are now corpus_offset */

    if (sel_x2 >= data.width - max(comp_patch_radius, transfer_patch_radius))
        sel_x2 = data.width - max(comp_patch_radius, transfer_patch_radius) - 1;

    if (sel_y2 >= data.height - max(comp_patch_radius, transfer_patch_radius))
        sel_y2 = data.height - max(comp_patch_radius, transfer_patch_radius) - 1;

    sel_x1 -= (ref_layer.width  - (sel_x2-sel_x1))/2;
    sel_x1 = max(max(comp_patch_radius, transfer_patch_radius), sel_x1);
    sel_y1 -= (ref_layer.height - (sel_y2-sel_y1))/2;
    sel_y1 = max(max(comp_patch_radius, transfer_patch_radius), sel_y1);

    sel_x2 = min(sel_x1 + ref_layer.width, data.width - max(comp_patch_radius, transfer_patch_radius) - 1);
    sel_y2 = min(sel_y1 + ref_layer.height, data.height - max(comp_patch_radius, transfer_patch_radius) - 1);

    /* Sanity check */

    if (!data_points.size()) {
        gimp_message("The output image is too small.");
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Setup */
    int64_t perf_neighbour_search = 0;
    int64_t perf_refinement       = 0;
    int64_t perf_interpolation    = 0;
    int64_t perf_fill_undo        = 0;
    int64_t perf_edge_points      = 0;
    struct timespec perf_tmp;

    make_offset_list();

    setup_metric(parameters.autism);

    /* Do it */

    int total_points = data_points.size();

    fprintf(logfile, "status  dimensions: (%d, %d)\n", confidence_map.width, confidence_map.height);
    fprintf(logfile, "data dimensions: (%d, %d)\n", data.width, data.height);
    fprintf(logfile, "ref_layer dimensions: (%d, %d, %d, %d)\n", sel_x1, sel_y1, sel_x2-sel_x1, sel_y2-sel_y1);
    fprintf(logfile, "total points to be filled: %d\n", total_points);
    fflush(logfile);

    int points_to_go = total_points;
    while (points_to_go > 0) {
        gimp_progress_update(1.0-float(points_to_go)/total_points);
        points_to_go -= purge_already_filled();

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_edge_points -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        // fill edge_points with points that are near already filled points
        // that ensures inward propagation
        // first element in pair is complexity of point neighbourhood
        vector<pair<int, Coordinates>> edge_points(0);

        START_TIMER
        get_edge_points(edge_points);
        STOP_TIMER("get_edge_points")
        size_t edge_points_size = edge_points.size();

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_edge_points += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;


        // find best-fit patches for edge_points
        for(int i=0; i < edge_points_size; ++i) {
            Coordinates position = edge_points[i].second;
            if (*confidence_map.at(position))
                continue;

            best = 1<<30;
            best_color_diff.assign(input_bytes, 0);

            ///////////////////////////
            // Neighbour search BEGIN
            ///////////////////////////
            
            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_neighbour_search -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            const int sorted_offsets_size = sorted_offsets.size();

            for(int j=0, n_neighbours=0; j<sorted_offsets_size; ++j) {
                Coordinates candidate = position + sorted_offsets[j];
                if (wrap_or_clip(parameters, data, candidate) && 
                        *confidence_map.at(candidate))
                {
                    START_TIMER
                    try_point(candidate, position, best, best_point, best_color_diff);
                    STOP_TIMER("try_point")
                    if (++n_neighbours >= parameters.neighbours) break;
                }
            }

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_neighbour_search += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            ///////////////////////////
            // Neighbour search END
            ///////////////////////////

            ///////////////////////////
            // Random refinement BEGIN
            ///////////////////////////

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_refinement -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            /*
            vector<boost::shared_future<Coordinates>> futures(0);
            for (int t=0; t<max_threads; ++t) {
                refine_callable refiner(parameters.tries/max_threads, position);
                boost::packaged_task<Coordinates> pt(refiner);
                boost::shared_future<Coordinates> f = pt.get_future();
                boost::thread refine_thread(boost::move(pt));
                futures.push_back(f);
            }
            boost::wait_for_all(futures.begin(), futures.end());


            for (int t=0; t<max_threads; ++t) {
                const Coordinates cand = futures[t].get();
                try_point(cand, position, best, best_point, best_color_diff);
            }
            */

            refine_callable refiner(parameters.tries, position);
            Coordinates cand = refiner();
            try_point(cand, position, best, best_point, best_color_diff);

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_refinement += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            ///////////////////////////
            // Random refinement END 
            ///////////////////////////

            ///////////////////////////
            // Try interpolation and transfer BEGIN
            ///////////////////////////

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_interpolation -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            int mean_values[input_bytes];
            int grad_x[input_bytes];
            int grad_y[input_bytes];
            if (invent_gradients &&
                get_gradientness(position, mean_values, grad_x, grad_y) < best)
            {
                for (int ox=-transfer_patch_radius; ox<=transfer_patch_radius; ++ox)
                    for (int oy=-transfer_patch_radius; oy<=transfer_patch_radius; ++oy) {
                        Coordinates near_pos = position + Coordinates(ox, oy);
                        if (clip(data, near_pos) && !*confidence_map.at(near_pos)) {
                            for (int j = 0; j<input_bytes; ++j)
                                data.at(near_pos)[j] = mean_values[j] + (grad_x[j]*ox) + (grad_y[j]*oy);
                            *confidence_map.at(near_pos) = 20;
                        }
                    }
            } else
                transfer_patch(position, best_point);

            ///////////////////////////
            // Try interpolation and transfer END
            ///////////////////////////
            
            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_interpolation += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        }

        if (!edge_points_size)
            break;

        if (update_undo_stack) {
            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_fill_undo -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            /* Write result to region */
            data.to_drawable(drawable, 0,0, 0);

            /* Voodoo to update actual image */
            gimp_drawable_flush(drawable);
            gimp_drawable_merge_shadow(drawable->drawable_id,TRUE);
            gimp_drawable_update(drawable->drawable_id,0,0,data.width,data.height);

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_fill_undo += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;
        }
    }

    fprintf(logfile, "\n%d points left unfilled\n", points_to_go);
    fprintf(logfile, "populating edge_points took %lld usec\n", perf_edge_points/1000);
    fprintf(logfile, "neighbour search took %lld usec\n", perf_neighbour_search/1000);
    fprintf(logfile, "refinement took %lld usec\n", perf_refinement/1000);
    fprintf(logfile, "interpolation took %lld usec\n", perf_interpolation/1000);
    fprintf(logfile, "updating undo stack took %lld usec\n", perf_fill_undo/1000);
    fclose(logfile);

    /* Write result back to the GIMP, clean up */

    /* Write result to region */
    data.to_drawable(drawable, 0,0, 0);

    /* Voodoo to update actual image */
    gimp_drawable_flush(drawable);
    gimp_drawable_merge_shadow(drawable->drawable_id,TRUE);
    gimp_drawable_update(drawable->drawable_id,0,0,data.width,data.height);

    gimp_drawable_detach(drawable);
    gimp_drawable_detach(corpus_drawable);

    gimp_displays_flush();
}
