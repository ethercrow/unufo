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

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <gtk/gtk.h>

#include "bench.h"
#include "unufo_consts.h"
#include "unufo_geometry.h"
#include "unufo_gimp_comm.h"
#include "unufo_patch.h"
#include "unufo_pixel.h"
#include "unufo_types.h"
#include "unufo_utils.h"

using namespace std;
using namespace unufo;

/* Macro to define the usual plugin main function */
MAIN()

/* Helpers for the main function */
static int input_bytes;
static int comp_patch_radius;

static bool equal_adjustment;
static int max_adjustment;

static bool use_ref_layer;

// we must fill selection subset of data
// using ref_mask subset of ref_layer for inspiration
// status holds current state of point filling
static Bitmap<uint8_t> data, data_mask, ref_layer, ref_mask;
static Bitmap<uint8_t> confidence_map;
static int sel_x1, sel_y1, sel_x2, sel_y2;

// data_points is a queue of points to be filled,
// it can contain duplicates, which mean points re-analysis
//
// sorted_offsets is an array of points near origin, beginning with (0,0)
// and sorted by distance from origin (see Coordinates::operator< for 
// current definition of 'distance')
static vector<Coordinates> ref_points(0);

static int best;
static Coordinates best_point;
static vector<int> best_color_diff(0);

static Matrix<Coordinates> transfer_map;
static Matrix<int> transfer_belief;

struct already_filled_pred
{
    bool operator()(const Coordinates& position) {
        return *transfer_belief.at(position) >= 0;
    }
};

void purge_already_filled(vector<Coordinates>& data_points)
{
    data_points.erase(
        std::remove_if(data_points.begin(), data_points.end(), already_filled_pred()),
        data_points.end());
}

void get_edge_points(const vector<Coordinates>& data_points, vector<pair<int, Coordinates>>& edge_points)
{
    for(size_t i=0; i < data_points.size(); ++i) {
        Coordinates position = data_points[i];
        bool island_flag = true;
        for (int ox=-1; ox<=1; ++ox)
            for (int oy=-1; oy<=1; ++oy)
                if (*confidence_map.at(position + Coordinates(ox, oy)))
                    island_flag = false;
        if (!island_flag) {
            START_TIMER
            int complexity = get_complexity(data, confidence_map, transfer_belief,
                    data_points[i], comp_patch_radius, input_bytes);
            STOP_TIMER("get_complexity")
            edge_points.push_back(std::make_pair(complexity, data_points[i]));
        }
    }
    sort(edge_points.begin(), edge_points.end());
    edge_points.erase(edge_points.begin(),
        upper_bound(edge_points.begin(), edge_points.end(),
            make_pair(-1, Coordinates(0, 0))));

    // leave only the most important edge_points
    if (edge_points.size() > important_count)
        edge_points.erase(edge_points.begin(),
            edge_points.end()-edge_points.size()/2);
}

static inline bool try_point(const Coordinates& candidate,
                             const Coordinates& position,
                             int& best,
                             Coordinates& best_point,
                             vector<int>& best_color_diff)
{
    int difference;
    if (max_adjustment)
        difference = get_difference_color_adjustment(data,
            transfer_belief, comp_patch_radius,
            candidate, position, best_color_diff, best,
            input_bytes, max_adjustment, equal_adjustment);
    else
        difference = get_difference(data,
            transfer_belief, comp_patch_radius,
            candidate, position, best);

    if (best <= difference)
        return false;
    best = difference;
    best_point = candidate;
    return true;
}

class refine_callable
{
public:
    refine_callable(int n, const Coordinates& position): n_{n}, position_{position}{}

    Coordinates operator()() {
        // thread local vars
        int tl_best{INT_MAX};
        Coordinates tl_best_point;
        vector<int> tl_best_color_diff{0, 0, 0, 0};

        // TODO: unify these branches, use ref_points with border
        // bonus point: this will fix the FIXME dozen lines below
        if (use_ref_layer) {
            int ref_points_size{ref_points.size()};
            if (n_ < ref_points_size) { // random guesses
                for (int j=0; j<n_; ++j) {
                    const Coordinates& candidate{ref_points[rand()%ref_points_size]};
                    try_point(candidate, position_, tl_best, tl_best_point, tl_best_color_diff);
                }
            } else { // exhaustive search
                for (int j=0; j<ref_points_size; ++j) {
                    const Coordinates& candidate{ref_points[j]};
                    try_point(candidate, position_, tl_best, tl_best_point, tl_best_color_diff);
                }
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

    int64_t perf_overall          = 0;
    int64_t perf_random_search    = 0;
    int64_t perf_refinement       = 0;

    int converge_count = 0;

    int64_t perf_fill_undo        = 0;
    int64_t perf_edge_points      = 0;
    struct timespec perf_tmp;

    clock_gettime(CLOCK_REALTIME, &perf_tmp);
    perf_overall -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

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

    if (!get_parameters_from_list(&parameters, nparams, param)) {
        UNUFO_LOG("get_parameters_from_list failed\n")
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Get drawable */
    drawable = gimp_drawable_get(param[WORK_LAYER_PARAM_ID].data.d_drawable);

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
        ref_drawable = gimp_drawable_get(param[REF_LAYER_PARAM_ID].data.d_drawable);
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

    equal_adjustment = parameters.equal_adjustment;
    max_adjustment   = parameters.max_adjustment;

    input_bytes  = drawable->bpp;

    /* Fetch the whole image data */
    fetch_image_and_mask(drawable, data, input_bytes, data_mask, 255, sel_x1, sel_y1, sel_x2, sel_y2);

    confidence_map.resize(data.width,data.height,1);
    transfer_map.resize(data.width,data.height);
    transfer_belief.resize(data.width,data.height);

    vector<Coordinates> data_points(0);

    for(int y=0;y<confidence_map.height;y++)
        for(int x=0;x<confidence_map.width;x++) {
            if (!data_mask.at(x,y)[0]) {
                // ground truth
                *confidence_map.at(x,y) = 255;
                *transfer_belief.at(x,y) = 0;
            } else {
                // point to fill
                *confidence_map.at(x,y) = 0;
                *transfer_belief.at(x,y) = -1;
                data_points.push_back(Coordinates(x,y));
            }
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

    UNUFO_LOG("gimp setup dragons end\n")
    //////////////////////////////
    // Gimp setup dragons END
    //////////////////////////////

    /* little geometry so that sel_x1 and sel_y1 are now corpus_offset */

    if (sel_x2 >= data.width - comp_patch_radius)
        sel_x2 = data.width - comp_patch_radius - 1;

    if (sel_y2 >= data.height - comp_patch_radius)
        sel_y2 = data.height - comp_patch_radius - 1;

    sel_x1 -= (ref_layer.width  - (sel_x2-sel_x1))/2;
    sel_x1 = max(comp_patch_radius, sel_x1);
    sel_y1 -= (ref_layer.height - (sel_y2-sel_y1))/2;
    sel_y1 = max(comp_patch_radius, sel_y1);

    sel_x2 = min(sel_x1 + ref_layer.width, data.width - comp_patch_radius - 1);
    sel_y2 = min(sel_y1 + ref_layer.height, data.height - comp_patch_radius - 1);

    /* Sanity check */

    if (!data_points.size()) {
        gimp_message("The output image is too small.");
        values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR;
        return;
    }

    /* Do it */

    int total_points = data_points.size();
    vector<Coordinates> data_points_backup(data_points);

    UNUFO_LOG("status  dimensions: (%d, %d)\n", confidence_map.width, confidence_map.height)
    UNUFO_LOG("data dimensions: (%d, %d)\n", data.width, data.height)
    UNUFO_LOG("ref_layer dimensions: (%d, %d, %d, %d)\n", sel_x1, sel_y1, sel_x2-sel_x1, sel_y2-sel_y1)
    UNUFO_LOG("total points to be filled: %d\n", total_points)

    int points_to_go = total_points;
    while (points_to_go > 0) {
        gimp_progress_update(
            float(in_loop_pass_count)/(in_loop_pass_count + refine_pass_count)*
            (1.0-float(points_to_go)/(total_points)));

        purge_already_filled(data_points);
        points_to_go = data_points.size();

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_edge_points -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        // fill edge_points with points that are near already filled points
        // that ensures inward propagation
        // first element in pair is complexity of point neighbourhood
        vector<pair<int, Coordinates>> edge_points(0);

        get_edge_points(data_points, edge_points);
        size_t edge_points_size = edge_points.size();

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_edge_points += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        // find best-fit patches for edge_points
        // TODO: this for is parallelizable
        for(int i=0; i < edge_points_size; ++i) {
            Coordinates position = edge_points[i].second;

            best = INT_MAX;
            best_color_diff.assign(input_bytes, 0);

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_random_search -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            refine_callable refiner(parameters.tries, position);
            Coordinates cand = refiner();
            try_point(cand, position, best, best_point, best_color_diff);

            clock_gettime(CLOCK_REALTIME, &perf_tmp);
            perf_random_search += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

            START_TIMER
            transfer_patch(data, input_bytes,
                    confidence_map, transfer_map, transfer_belief,
                    position, best_point, best, best_color_diff);
            STOP_TIMER("transfer_patch")
        }

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_refinement -= perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

        for (int p=0; p<in_loop_pass_count; ++p) {
            int p_mod2 = p%2;
            int i_begin;
            int i_end;
            int i_inc;
            if (p_mod2) {
                i_begin = edge_points_size-1;
                i_end   = -1;
                i_inc   = -1;
            } else {
                i_begin = 0;
                i_end   = edge_points_size;
                i_inc   = 1;
            }
            bool converged = true;

            for(int i=i_begin; i != i_end; i+=i_inc) {
                Coordinates position = edge_points[i].second;
                int best = INT_MAX;
                Coordinates best_point = *transfer_map.at(position);

                // coherence propagation
                for (int ox=-1; ox<=1; ++ox)
                    for (int oy=-1; oy<=1; ++oy) {
                        Coordinates offset(ox, oy);
                        Coordinates neighbour = position + offset;
                        if (clip(data, neighbour) && *data_mask.at(neighbour)) {
                            auto neighbour_src_p = transfer_map.at(neighbour);
                            if (*(reinterpret_cast<uint64_t*>(neighbour_src_p))) {
                                Coordinates near_neighbour_src = *neighbour_src_p - offset;
                                if (clip(data, near_neighbour_src) &&
                                    try_point(near_neighbour_src, position, best, best_point, best_color_diff))
                                {
                                    transfer_patch(data, input_bytes,
                                            confidence_map, transfer_map, transfer_belief,
                                            position, best_point, best, best_color_diff);
                                    converged = false;
                                }
                            }
                        }
                    }

                // random search
                int search_range = max(data.width, data.height);
                while (search_range > 0) {
                    int ox = rand()%search_range;
                    int oy = rand()%search_range;
                    Coordinates offset(ox, oy);
                    Coordinates near_src = *transfer_map.at(position) + offset;
                    if ((ox||oy) && clip(data, near_src) && !*data_mask.at(near_src)) {
                        int best = *transfer_belief.at(position);
                        Coordinates best_point = *transfer_map.at(position);
                        if (try_point(near_src - offset,
                            position, best, best_point, best_color_diff))
                        {
                            transfer_patch(data, input_bytes,
                                    confidence_map, transfer_map, transfer_belief,
                                    position, best_point, best, best_color_diff);
                            converged = false;
                        }
                    }
                    search_range /= 2;
                }
            }
            if (converged) {
                ++converge_count;
                break;
            }
        }

        clock_gettime(CLOCK_REALTIME, &perf_tmp);
        perf_refinement += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

#ifndef NDEBUG
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
#endif

        if (!edge_points_size)
            break;
    }

    for (int p=0; p<refine_pass_count; ++p) {
        gimp_progress_update(float(in_loop_pass_count + p)/(in_loop_pass_count + refine_pass_count));
        int p_mod2 = p%2;
        int i_begin;
        int i_end;
        int i_inc;
        if (p_mod2) {
            i_begin = total_points-1;
            i_end   = -1;
            i_inc   = -1;
        } else {
            i_begin = 0;
            i_end   = total_points;
            i_inc   = 1;
        }
        bool converged = true;
        for(int i=i_begin; i != i_end; i+=i_inc) {
            Coordinates position = data_points_backup[i];
            int best = INT_MAX;
            Coordinates best_point = *transfer_map.at(position);

            // coherence propagation
            for (int ox=-1; ox<=1; ++ox)
                for (int oy=-1; oy<=1; ++oy) {
                    Coordinates offset(ox, oy);
                    Coordinates neighbour = position + offset;
                    if (clip(data, neighbour) && *data_mask.at(neighbour)) {
                        auto neighbour_src_p = transfer_map.at(neighbour);
                        if (*(reinterpret_cast<uint64_t*>(neighbour_src_p))) {
                            Coordinates near_neighbour_src = *neighbour_src_p - offset;
                            if (clip(data, near_neighbour_src) &&
                                try_point(near_neighbour_src, position, best, best_point, best_color_diff))
                            {
                                transfer_patch(data, input_bytes,
                                        confidence_map, transfer_map, transfer_belief,
                                        position, best_point, best, best_color_diff);
                                converged = false;
                            }
                        }
                    }
                }

            // random search
            int search_range = max(data.width, data.height);
            while (search_range > 0) {
                int ox = rand()%search_range;
                int oy = rand()%search_range;
                Coordinates offset(ox, oy);
                Coordinates near_src = *transfer_map.at(position) + offset;
                if ((ox||oy) && clip(data, near_src) && !*data_mask.at(near_src)) {
                    int best = *transfer_belief.at(position);
                    Coordinates best_point = *transfer_map.at(position);
                    if (try_point(near_src - offset,
                        position, best, best_point, best_color_diff))
                    {
                        transfer_patch(data, input_bytes,
                                confidence_map, transfer_map, transfer_belief,
                                position, best_point, best, best_color_diff);
                        converged = false;
                    }
                }
                search_range /= 2;
            }
        }
    }

    clock_gettime(CLOCK_REALTIME, &perf_tmp);
    perf_overall += perf_tmp.tv_nsec + 1000000000LL*perf_tmp.tv_sec;

    UNUFO_LOG("\n%d points left unfilled\n", points_to_go)
    UNUFO_LOG("populating edge_points took %lld usec\n", perf_edge_points/1000)
    UNUFO_LOG("random search took %lld usec\n", perf_random_search/1000)
    UNUFO_LOG("refinement took %lld usec\n", perf_refinement/1000)
    UNUFO_LOG("early converge count: %d\n", converge_count)
    UNUFO_LOG("updating undo stack took %lld usec\n", perf_fill_undo/1000)
    UNUFO_LOG("overall time: %lld usec\n", perf_overall/1000)

    /* Write result back to the GIMP, clean up */

    /* Write result to region */
    data.to_drawable(drawable, 0, 0, 0);

    /* Voodoo to update actual image */
    gimp_drawable_flush(drawable);
    gimp_drawable_merge_shadow(drawable->drawable_id,TRUE);
    gimp_drawable_update(drawable->drawable_id,0,0,data.width,data.height);

    gimp_drawable_detach(drawable);
    gimp_drawable_detach(corpus_drawable);

    gimp_displays_flush();
}

